// XRM: Coordinates daemon business operations and core lifecycle.

#include "server/application.hpp"

#include "contract/wire.hpp"
#include "infra/files.hpp"
#include "profile/routing_presets.hpp"
#include "subscription/parser.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/process_adapter.hpp"
#include "xray/runtime_assembler.hpp"

#include <openssl/evp.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace xrm {
namespace {

Id new_id() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto &byte : bytes)
    byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char digits[] = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10)
      id.push_back('-');
    id.push_back(digits[bytes[index] >> 4]);
    id.push_back(digits[bytes[index] & 0x0f]);
  }
  return id;
}

AppError invalid(std::string message, std::string subject) {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject),
          std::nullopt};
}

Result<std::vector<Node>> source_nodes(Store &store, const Id &source_id) {
  std::vector<Node> nodes;
  std::size_t offset = 0;
  for (;;) {
    auto page = store.list_nodes(
        NodeQuery{.source_id = source_id, .include_missing = true},
        {offset, 1000});
    if (!page)
      return std::unexpected(page.error());
    nodes.insert(nodes.end(), page->items.begin(), page->items.end());
    if (!page->next_offset)
      break;
    offset = *page->next_offset;
  }
  return nodes;
}

Json connection_settings(const Node &node) {
  Json settings = Json(node).at("settings");
  if (settings.value("kind", "") == "raw" && settings.contains("value") &&
      settings["value"].is_object())
    settings["value"].erase("tag");
  return settings;
}

std::string snapshot_token(const Json &snapshot) {
  const auto bytes = snapshot.dump();
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1)
    return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string token;
  token.reserve(length * 2);
  for (unsigned index = 0; index < length; ++index) {
    token.push_back(hex[digest[index] >> 4]);
    token.push_back(hex[digest[index] & 0x0f]);
  }
  return token;
}

} // namespace

struct Application::GeodataSchedule {
  uv_timer_t timer{};
  Application *owner = nullptr;
};

Application::Application(Store &store, ProcessService &processes,
                         HttpClient &http, uv_loop_t &loop,
                         std::filesystem::path data_root,
                         std::string application_version, Id boot_id,
                         SubscriptionService &subscriptions)
    : store_(&store), subscriptions_(&subscriptions), processes_(&processes),
      http_(&http), application_version_(std::move(application_version)),
      boot_id_(std::move(boot_id)), data_root_(std::move(data_root)),
      profiles_(
          std::make_unique<ProfileService>(store, data_root_ / "profiles")),
      runtime_writer_(std::make_unique<RuntimeWriter>(data_root_)),
      core_manager_(std::make_unique<CoreManager>(loop, store, processes,
                                                  *runtime_writer_)),
      xray_control_(std::make_unique<XrayControl>(processes)),
      metrics_client_(std::make_unique<MetricsClient>(loop, http)),
      core_installer_(std::make_unique<CoreInstaller>(http, processes,
                                                      data_root_ / "cores")),
      node_test_service_(
          std::make_unique<NodeTestService>(loop, processes, http)),
      geodata_cache_(std::make_unique<GeodataCache>(http, processes, loop,
                                                    data_root_ / "geodata")),
      operations_(boot_id_) {
  core_manager_->on_output([this](LogStream stream, std::string bytes) {
    logs_.append(stream, bytes);
  });
  core_manager_->on_output_closed(
      [this](LogStream stream) { logs_.finish(stream); });
  geodata_schedule_ = new GeodataSchedule{};
  geodata_schedule_->owner = this;
  if (uv_timer_init(&loop, &geodata_schedule_->timer) != 0) {
    delete geodata_schedule_;
    geodata_schedule_ = nullptr;
    throw std::runtime_error("无法初始化Geodata自动更新计时器");
  }
  geodata_schedule_->timer.data = geodata_schedule_;
  schedule_geodata_updates();
}

Application::~Application() {
  if (!geodata_schedule_)
    return;
  auto *schedule = std::exchange(geodata_schedule_, nullptr);
  schedule->owner = nullptr;
  uv_timer_stop(&schedule->timer);
  if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&schedule->timer)))
    uv_close(reinterpret_cast<uv_handle_t *>(&schedule->timer),
             [](uv_handle_t *handle) {
               delete static_cast<GeodataSchedule *>(handle->data);
             });
}

void Application::schedule_geodata_updates() {
  if (!geodata_schedule_ || !geodata_schedule_->owner)
    return;
  uv_timer_stop(&geodata_schedule_->timer);
  auto records = store_->geodata_resources();
  if (!records)
    return;
  const auto now = std::chrono::duration_cast<Milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::optional<std::uint64_t> delay;
  for (const auto &record : *records) {
    if (!record.auto_update || record.update_interval.count() <= 0 ||
        record.validator_core_id.empty() ||
        geodata_inflight_.contains(record.id))
      continue;
    const auto next =
        record.last_attempt > 0
            ? record.last_attempt + std::chrono::duration_cast<Milliseconds>(
                                        record.update_interval)
                                        .count()
            : now;
    const auto wait =
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, next - now));
    if (!delay || wait < *delay)
      delay = wait;
  }
  if (delay)
    uv_timer_start(
        &geodata_schedule_->timer,
        [](uv_timer_t *timer) {
          auto *schedule = static_cast<GeodataSchedule *>(timer->data);
          if (schedule->owner)
            schedule->owner->on_geodata_schedule();
        },
        *delay, 0);
}

void Application::on_geodata_schedule() {
  auto records = store_->geodata_resources();
  if (!records)
    return;
  const auto now = std::chrono::duration_cast<Milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  for (const auto &record : *records) {
    if (!record.auto_update || record.update_interval.count() <= 0 ||
        record.validator_core_id.empty() ||
        geodata_inflight_.contains(record.id))
      continue;
    const auto interval =
        std::chrono::duration_cast<Milliseconds>(record.update_interval)
            .count();
    if (record.last_attempt > 0 && record.last_attempt + interval > now)
      continue;
    auto started = update_geodata_resource(record.id, record.validator_core_id);
    if (!started)
      (void)store_->commit_geodata_resource_result(
          record.id, record.settings_revision, std::nullopt, now,
          started.error().message);
  }
  schedule_geodata_updates();
}

Json Application::server_info() const {
  return Json{{"version", application_version_},
              {"protocol", 1},
              {"boot_id", boot_id_}};
}

Result<NodeSummary> Application::add_manual_node(std::string name,
                                                 Json outbound) {
  if (name.empty())
    return std::unexpected(invalid("节点名称不能为空", "name"));
  if (!outbound.is_object())
    return std::unexpected(invalid("outbound必须是原生JSON对象", "outbound"));
  if (!outbound.contains("protocol") || !outbound.at("protocol").is_string() ||
      outbound.at("protocol").get<std::string>().empty()) {
    return std::unexpected(
        invalid("outbound必须包含非空protocol", "outbound.protocol"));
  }
  Node node{new_id(), std::nullopt, name,  std::nullopt,
            {},       true,         false, RawOutbound{std::move(outbound)}};
  if (auto stored = store_->put_node(node); !stored)
    return std::unexpected(stored.error());
  return summarize_node(node);
}

Result<Id> Application::start_node_test(const Id &node_id, NodeTestKind kind,
                                        std::optional<std::string> target_url,
                                        std::optional<Id> profile_id,
                                        Milliseconds timeout,
                                        std::size_t max_body_bytes) {
  auto node = store_->get_node(node_id);
  if (!node)
    return std::unexpected(node.error());
  if (!node->enabled || node->source_missing)
    return std::unexpected(
        invalid("已禁用或已移除节点不能开始新测试", node_id));
  auto test_settings = store_->load_program_settings();
  if (!test_settings)
    return std::unexpected(test_settings.error());
  const auto configured_tests = test_settings->value("tests", Json::object());
  const auto concurrency = configured_tests.value("concurrency", 3);
  if (node_test_processes_.size() >= static_cast<std::size_t>(concurrency))
    return std::unexpected(
        AppError{ErrorCode::busy, "节点测试并发上限已达到", node_id, {}});
  std::filesystem::path core_binary;
  if (kind == NodeTestKind::url || kind == NodeTestKind::download) {
    if (!profile_id || profile_id->empty())
      return std::unexpected(
          invalid("代理测试需要当前 Profile 与选定核心", "profile_id"));
    auto profile = profiles_->get(*profile_id);
    if (!profile)
      return std::unexpected(profile.error());
    auto core = store_->get_core(profile->core_id);
    if (!core)
      return std::unexpected(core.error());
    core_binary = core->path;
    if (!target_url || target_url->empty()) {
      for (const auto *key : {"observatory", "burstObservatory"}) {
        auto module = profiles_->module(*profile_id, key);
        if (!module || !module->setting.included || !module->document)
          continue;
        auto parsed =
            Json::parse(module->document->source_text, nullptr, false);
        if (!parsed.is_object() || !parsed.contains(key) ||
            !parsed[key].is_object())
          continue;
        const auto &native = parsed[key];
        if (std::string_view(key) == "observatory" &&
            native.contains("probeUrl") && native["probeUrl"].is_string())
          target_url = native["probeUrl"].get<std::string>();
        else if (std::string_view(key) == "burstObservatory" &&
                 native.contains("pingConfig") &&
                 native["pingConfig"].is_object() &&
                 native["pingConfig"].contains("destination") &&
                 native["pingConfig"]["destination"].is_string())
          target_url = native["pingConfig"]["destination"].get<std::string>();
        if (target_url && !target_url->empty())
          break;
      }
    }
    if (!target_url || target_url->empty()) {
      if (test_settings->contains("tests") &&
          (*test_settings)["tests"].is_object()) {
        const auto &tests = (*test_settings)["tests"];
        if (tests.contains("default_url") && tests["default_url"].is_string())
          target_url = tests["default_url"].get<std::string>();
      }
    }
    if (!target_url || target_url->empty())
      return std::unexpected(invalid(
          "请指定测试 URL 或在 Config → Tests 设置默认目标", "target_url"));
  }
  NodeTestRequest request;
  request.node = *node;
  request.kind = kind;
  request.target_url = target_url.value_or("");
  request.timeout = timeout;
  request.max_body_bytes = max_body_bytes;
  request.xray_binary = core_binary;
  request.temporary_root = data_root_ / "node-tests";
  const Json snapshot = Json(*node).at("settings");
  const Id operation = operations_.begin();
  const auto completed = std::make_shared<bool>(false);
  auto started = node_test_service_->start(
      std::move(request), [this, operation, node_id, snapshot,
                           completed](Result<NodeTestResult> result) {
        *completed = true;
        node_test_processes_.erase(operation);
        if (!result) {
          operations_.complete(operation, std::unexpected(result.error()));
          return;
        }
        const auto status =
            result->status == NodeTestStatus::success     ? "success"
            : result->status == NodeTestStatus::failed    ? "failed"
            : result->status == NodeTestStatus::cancelled ? "cancelled"
                                                          : "not_applicable";
        Json output{
            {"node_id", result->node_id},
            {"kind", result->kind == NodeTestKind::icmp       ? "icmp"
                     : result->kind == NodeTestKind::tcp      ? "tcp"
                     : result->kind == NodeTestKind::download ? "download"
                                                              : "url"},
            {"status", status},
            {"target", result->target},
            {"sampled_at", result->sampled_at},
            {"latency_ms",
             result->latency_ms ? Json(*result->latency_ms) : Json(nullptr)},
            {"bytes", result->received_bytes ? Json(*result->received_bytes)
                                             : Json(nullptr)},
            {"bytes_per_second", result->bytes_per_second
                                     ? Json(*result->bytes_per_second)
                                     : Json(nullptr)},
            {"message", result->message}};
        auto current = store_->get_node(node_id);
        output["stale"] = !current || Json(*current).at("settings") != snapshot;
        if (current) {
          node_test_results_[node_id] = output;
          node_test_snapshots_[node_id] = snapshot;
        } else {
          node_test_results_.erase(node_id);
          node_test_snapshots_.erase(node_id);
        }
        operations_.complete(operation, output);
      });
  if (!started) {
    operations_.complete(operation, std::unexpected(started.error()));
    return std::unexpected(started.error());
  }
  if (!*completed)
    node_test_processes_[operation] = *started;
  return operation;
}

Result<void> Application::cancel_node_test(const Id &operation_id) {
  const auto found = node_test_processes_.find(operation_id);
  if (found == node_test_processes_.end())
    return std::unexpected(AppError{
        ErrorCode::not_found, "没有正在运行的节点测试", operation_id, {}});
  return node_test_service_->cancel(found->second);
}

Json Application::node_test_results() const {
  Json results = Json::array();
  for (const auto &[id, cached] : node_test_results_) {
    auto current = store_->get_node(id);
    if (current) {
      auto result = cached;
      if (auto baseline = node_test_snapshots_.find(id);
          baseline != node_test_snapshots_.end())
        result["stale"] = Json(*current).at("settings") != baseline->second;
      results.push_back(std::move(result));
    }
  }
  return results;
}

Result<NodeSummary> Application::add_manual_typed_node(std::string name,
                                                       Json settings) {
  if (name.empty())
    return std::unexpected(invalid("节点名称不能为空", "name"));
  if (!settings.is_object() || !settings.contains("kind") ||
      !settings["kind"].is_string() || settings["kind"] != "stream")
    return std::unexpected(
        invalid("typed节点需要kind=stream的settings对象", "settings"));
  Node candidate;
  try {
    candidate = Json{{"id", "validation-node"},
                     {"source_id", nullptr},
                     {"upstream_id", nullptr},
                     {"original_name", name},
                     {"alias", nullptr},
                     {"labels", Json::array()},
                     {"enabled", true},
                     {"source_missing", false},
                     {"settings", std::move(settings)}}
                    .get<Node>();
  } catch (const std::exception &exception) {
    return std::unexpected(invalid(
        "typed节点参数无效: " + std::string(exception.what()), "settings"));
  }
  if (auto outbound = write_outbound(candidate, "validation-node"); !outbound)
    return std::unexpected(outbound.error());
  candidate.id = new_id();
  if (auto stored = store_->put_node(candidate); !stored)
    return std::unexpected(stored.error());
  return summarize_node(candidate);
}

Result<NodeSummary>
Application::add_manual_share_uri(std::string uri,
                                  std::optional<std::string> name) {
  auto parsed = parse_share_uri(uri);
  if (!parsed)
    return std::unexpected(parsed.error());
  const auto display_name = name.value_or(parsed->name);
  if (display_name.empty())
    return std::unexpected(invalid("节点名称不能为空", "name"));
  Node candidate{"validation-node",
                 std::nullopt,
                 display_name,
                 std::nullopt,
                 {},
                 true,
                 false,
                 std::move(parsed->settings)};
  if (auto outbound = write_outbound(candidate, "validation-node"); !outbound)
    return std::unexpected(outbound.error());
  candidate.id = new_id();
  if (auto stored = store_->put_node(candidate); !stored)
    return std::unexpected(stored.error());
  return summarize_node(candidate);
}

Result<NodeSummary> Application::replace_manual_node(const Id &id,
                                                     std::string name,
                                                     Json settings) {
  auto original = store_->get_node(id);
  if (!original)
    return std::unexpected(original.error());
  if (original->source_id)
    return std::unexpected(AppError{
        ErrorCode::unsupported, "订阅节点须先复制为手工节点后才能编辑连接参数",
        id, std::nullopt});
  if (name.empty())
    return std::unexpected(invalid("节点名称不能为空", "name"));
  if (!settings.is_object() || !settings.contains("kind") ||
      !settings["kind"].is_string() ||
      (settings["kind"] != "stream" && settings["kind"] != "raw"))
    return std::unexpected(
        invalid("settings必须是stream或raw节点对象", "settings"));
  Json encoded = *original;
  encoded["original_name"] = std::move(name);
  encoded["settings"] = std::move(settings);
  Node candidate;
  try {
    candidate = encoded.get<Node>();
  } catch (const std::exception &exception) {
    return std::unexpected(
        invalid("节点参数无效: " + std::string(exception.what()), "settings"));
  }
  if (auto outbound = write_outbound(candidate, "validation-node"); !outbound)
    return std::unexpected(outbound.error());
  if (auto stored = store_->put_node(candidate); !stored)
    return std::unexpected(stored.error());
  return summarize_node(candidate);
}

Result<void> Application::remove_manual_node(const Id &id) {
  auto node = store_->get_node(id);
  if (!node)
    return std::unexpected(node.error());
  if (node->source_id)
    return std::unexpected(AppError{ErrorCode::unsupported,
                                    "订阅节点由来源管理；只能删除手工节点", id,
                                    std::nullopt});
  auto profiles = profiles_->list();
  if (!profiles)
    return std::unexpected(profiles.error());
  for (const auto &summary : *profiles) {
    auto profile = profiles_->get(summary.id);
    if (!profile)
      return std::unexpected(profile.error());
    if (profile->selected && profile->selected->kind == TargetKind::node &&
        profile->selected->id_or_tag == id)
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "节点仍是 Profile 保存的默认出口",
                                      summary.name, std::nullopt});
    if (std::find(profile->additional_node_ids.begin(),
                  profile->additional_node_ids.end(),
                  id) != profile->additional_node_ids.end())
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "节点仍在 Profile 附加节点列表中",
                                      summary.name, std::nullopt});
    auto groups = profiles_->groups(summary.id);
    if (!groups)
      return std::unexpected(groups.error());
    for (const auto &group : *groups) {
      if (group.selected_node_id == id)
        return std::unexpected(AppError{ErrorCode::conflict,
                                        "节点仍是手选组的保存选择", group.name,
                                        std::nullopt});
      if (const auto *explicit_ids =
              std::get_if<ExplicitMembers>(&group.members);
          explicit_ids && std::find(explicit_ids->node_ids.begin(),
                                    explicit_ids->node_ids.end(),
                                    id) != explicit_ids->node_ids.end())
        return std::unexpected(AppError{ErrorCode::conflict,
                                        "节点仍被策略组显式引用", group.name,
                                        std::nullopt});
    }
    auto observation = profiles_->observation(summary.id);
    if (!observation)
      return std::unexpected(observation.error());
    if (std::find(observation->additional_node_ids.begin(),
                  observation->additional_node_ids.end(),
                  id) != observation->additional_node_ids.end())
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "节点仍在观测附加列表中", summary.name,
                                      std::nullopt});
    auto modules = profiles_->modules(summary.id);
    if (!modules)
      return std::unexpected(modules.error());
    for (const auto &module : *modules)
      if (module.document &&
          module.document->source_text.find(node_tag(id)) != std::string::npos)
        return std::unexpected(AppError{ErrorCode::conflict,
                                        "节点的管理 tag 仍出现在原生模块中",
                                        module.descriptor.key, std::nullopt});
  }
  auto runtime = runtime_status();
  if (!runtime)
    return std::unexpected(runtime.error());
  if (runtime->contains("actual_target") &&
      (*runtime)["actual_target"].is_object() &&
      (*runtime)["actual_target"].value("kind", "") == "node" &&
      (*runtime)["actual_target"].value("id_or_tag", "") == id)
    return std::unexpected(AppError{
        ErrorCode::conflict, "节点正在由核心使用，请先连接其他出口或停止核心",
        id, std::nullopt});
  return store_->erase_node(id);
}

Result<Page<NodeSummary>> Application::list_nodes(const NodeQuery &query,
                                                  PageRequest request) const {
  auto nodes = store_->list_nodes(query, request);
  if (!nodes)
    return std::unexpected(nodes.error());
  Page<NodeSummary> page;
  page.total = nodes->total;
  page.next_offset = nodes->next_offset;
  page.items.reserve(nodes->items.size());
  for (const auto &node : nodes->items)
    page.items.push_back(summarize_node(node));
  return page;
}

Result<Node> Application::get_node(const Id &id) const {
  return store_->get_node(id);
}

Result<Json> Application::preview_node_history(const Id &source_id) const {
  auto source = store_->get_source(source_id);
  if (!source)
    return std::unexpected(source.error());
  auto state = store_->get_source_state(source_id);
  if (!state)
    return std::unexpected(state.error());
  auto nodes = source_nodes(*store_, source_id);
  if (!nodes)
    return std::unexpected(nodes.error());

  std::map<Id, std::vector<std::string>> references;
  auto profiles = profiles_->list();
  if (!profiles)
    return std::unexpected(profiles.error());
  for (const auto &summary : *profiles) {
    auto profile = profiles_->get(summary.id);
    if (!profile)
      return std::unexpected(profile.error());
    const auto prefix = "Profile " + summary.name;
    if (profile->selected && profile->selected->kind == TargetKind::node)
      references[profile->selected->id_or_tag].push_back(prefix +
                                                         " default selection");
    for (const auto &id : profile->additional_node_ids)
      references[id].push_back(prefix + " additional nodes");
    auto groups = profiles_->groups(summary.id);
    if (!groups)
      return std::unexpected(groups.error());
    for (const auto &group : *groups) {
      if (group.selected_node_id)
        references[*group.selected_node_id].push_back(
            prefix + " group " + group.name + " selected node");
      if (const auto *explicit_ids =
              std::get_if<ExplicitMembers>(&group.members))
        for (const auto &id : explicit_ids->node_ids)
          references[id].push_back(prefix + " group " + group.name +
                                   " explicit members");
    }
    auto observation = profiles_->observation(summary.id);
    if (!observation)
      return std::unexpected(observation.error());
    for (const auto &id : observation->additional_node_ids)
      references[id].push_back(prefix + " observation additional nodes");
    auto modules = profiles_->modules(summary.id);
    if (!modules)
      return std::unexpected(modules.error());
    for (const auto &module : *modules) {
      if (!module.document)
        continue;
      for (const auto &node : *nodes) {
        if (!node.source_missing)
          continue;
        if (module.document->source_text.find(node_tag(node.id)) !=
            std::string::npos)
          references[node.id].push_back(prefix + " module " +
                                        module.descriptor.key + " managed tag");
      }
    }
  }
  auto runtime = runtime_status();
  if (runtime && runtime->contains("actual_target") &&
      (*runtime)["actual_target"].is_object()) {
    const auto &target = (*runtime)["actual_target"];
    if (target.value("kind", "") == "node")
      references[target.value("id_or_tag", "")].push_back(
          "currently running target");
  }

  std::map<std::string, std::vector<const Node *>> current_by_row;
  std::map<std::string, std::vector<const Node *>> current_by_connection;
  std::size_t current_count = 0;
  std::size_t history_count = 0;
  auto row_key = [](const Node &node) {
    return Json::array(
               {connection_settings(node), node.original_name,
                node.upstream_id ? Json(*node.upstream_id) : Json(nullptr)})
        .dump();
  };
  for (const auto &node : *nodes) {
    if (node.source_missing) {
      ++history_count;
      continue;
    }
    ++current_count;
    current_by_row[row_key(node)].push_back(&node);
    current_by_connection[connection_settings(node).dump()].push_back(&node);
  }
  Json candidates = Json::array();
  for (const auto &node : *nodes) {
    if (!node.source_missing)
      continue;
    const auto exact = current_by_row.find(row_key(node));
    const auto same_connection =
        current_by_connection.find(connection_settings(node).dump());
    const Node *keeper = nullptr;
    std::string reason;
    if (exact != current_by_row.end() && exact->second.size() == 1) {
      keeper = exact->second.front();
      reason = "redundant_history";
    } else if (exact != current_by_row.end())
      reason = "ambiguous_current";
    else if (same_connection != current_by_connection.end()) {
      if (same_connection->second.size() == 1)
        keeper = same_connection->second.front();
      reason = "different_name_or_upstream_id";
    } else
      reason = "removed_from_source";
    Json blockers = Json::array();
    if (reason != "redundant_history")
      blockers.push_back(reason);
    if (keeper &&
        (node.alias != keeper->alias || node.labels != keeper->labels ||
         node.enabled != keeper->enabled))
      blockers.push_back("user_attributes_differ");
    const auto found_refs = references.find(node.id);
    Json node_refs = found_refs == references.end() ? Json::array()
                                                    : Json(found_refs->second);
    if (!node_refs.empty())
      blockers.push_back("known_reference");
    candidates.push_back(
        Json{{"id", node.id},
             {"name", node.alias.value_or(node.original_name)},
             {"original_name", node.original_name},
             {"status", "history_source_missing"},
             {"source_name", source->name},
             {"keep_id", keeper ? keeper->id : ""},
             {"keep_name",
              keeper ? keeper->alias.value_or(keeper->original_name) : ""},
             {"keep_status", keeper ? "current" : "none"},
             {"reason", reason},
             {"eligible", blockers.empty()},
             {"blockers", std::move(blockers)},
             {"references", std::move(node_refs)}});
  }
  Json stored_nodes = Json::array();
  for (const auto &node : *nodes)
    stored_nodes.push_back(Json(node));
  const auto token = snapshot_token(Json{{"source", Json(*source)},
                                         {"state", Json(*state)},
                                         {"nodes", std::move(stored_nodes)},
                                         {"references", references}});
  if (token.empty())
    return std::unexpected(AppError{ErrorCode::io_error, "无法生成历史预览令牌",
                                    source_id, std::nullopt});
  return Json{{"source_id", source_id},
              {"source_name", source->name},
              {"token", token},
              {"current_count", current_count},
              {"history_count", history_count},
              {"candidates", std::move(candidates)}};
}

Result<Json> Application::confirm_node_history(const Id &source_id,
                                               const Json &selected) {
  if (!selected.is_object() || !selected.contains("token") ||
      !selected["token"].is_string() || !selected.contains("ids") ||
      !selected["ids"].is_array() || selected["ids"].empty())
    return std::unexpected(invalid("预览令牌和待处理ID不能为空", "selection"));
  std::vector<Id> ids;
  for (const auto &id : selected["ids"]) {
    if (!id.is_string() || id.get<std::string>().empty())
      return std::unexpected(invalid("历史记录ID无效", "ids"));
    ids.push_back(id.get<std::string>());
  }
  if (std::set<Id>(ids.begin(), ids.end()).size() != ids.size())
    return std::unexpected(invalid("历史记录ID重复", "ids"));
  auto preview = preview_node_history(source_id);
  if (!preview)
    return std::unexpected(preview.error());
  if (preview->at("token") != selected.at("token"))
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "来源、引用或节点已变化，请重新预览",
                                    source_id, std::nullopt});
  for (const auto &id : ids) {
    const auto found = std::find_if(
        (*preview)["candidates"].begin(), (*preview)["candidates"].end(),
        [&](const Json &candidate) { return candidate.value("id", "") == id; });
    if (found == (*preview)["candidates"].end() ||
        !found->value("eligible", false))
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "所选记录不是可安全处理的历史重复项", id,
                                      "重新预览并检查引用或属性冲突"});
  }
  auto state = store_->get_source_state(source_id);
  if (!state)
    return std::unexpected(state.error());
  auto nodes = source_nodes(*store_, source_id);
  if (!nodes)
    return std::unexpected(nodes.error());
  if (auto pruned = store_->prune_history(source_id, *state, *nodes, ids);
      !pruned)
    return std::unexpected(pruned.error());
  return Json{{"removed", ids.size()},
              {"source_id", source_id},
              {"runtime_changed", false}};
}

Result<NodeSummary> Application::copy_node(const Id &id, std::string new_name) {
  if (new_name.empty())
    return std::unexpected(invalid("节点名称不能为空", "name"));
  auto original = store_->get_node(id);
  if (!original)
    return std::unexpected(original.error());
  original->id = new_id();
  original->source_id.reset();
  original->upstream_id.reset();
  original->source_missing = false;
  original->original_name = std::move(new_name);
  original->alias.reset();
  original->labels.clear();
  original->enabled = true;
  if (auto saved = store_->put_node(*original); !saved)
    return std::unexpected(saved.error());
  return summarize_node(*original);
}

Result<NodeSummary> Application::set_node_enabled(const Id &id, bool enabled) {
  auto node = store_->get_node(id);
  if (!node)
    return std::unexpected(node.error());
  node->enabled = enabled;
  if (auto saved = store_->put_node(*node); !saved)
    return std::unexpected(saved.error());
  return summarize_node(*node);
}

Result<NodeSummary>
Application::set_node_alias(const Id &id, std::optional<std::string> alias) {
  auto node = store_->get_node(id);
  if (!node)
    return std::unexpected(node.error());
  node->alias = std::move(alias);
  if (auto saved = store_->put_node(*node); !saved)
    return std::unexpected(saved.error());
  return summarize_node(*node);
}

Result<NodeSummary>
Application::set_node_labels(const Id &id, std::vector<std::string> labels) {
  auto node = store_->get_node(id);
  if (!node)
    return std::unexpected(node.error());
  node->labels = std::move(labels);
  if (auto saved = store_->put_node(*node); !saved)
    return std::unexpected(saved.error());
  return summarize_node(*node);
}

Result<NodeSummary>
Application::edit_node_form(const Id &id, std::string_view field, Json value) {
  auto original = store_->get_node(id);
  if (!original)
    return std::unexpected(original.error());
  if (original->source_id)
    return std::unexpected(AppError{ErrorCode::unsupported,
                                    "订阅节点须先复制为手工节点后才能编辑", id,
                                    std::nullopt});
  Json encoded = *original;
  if (!encoded.at("settings").is_object() ||
      encoded.at("settings").value("kind", "") != "stream")
    return std::unexpected(
        AppError{ErrorCode::unsupported,
                 "Raw outbound没有typed "
                 "transport/security表单；请使用原生编辑或保存typed节点",
                 id, std::nullopt});
  if (field != "transport" && field != "security" && field != "sockopt" &&
      field != "finalmask" && field != "mux")
    return std::unexpected(
        invalid("未知的typed节点表单字段", std::string(field)));
  encoded["settings"][std::string(field)] = std::move(value);
  Node candidate;
  try {
    candidate = encoded.get<Node>();
  } catch (const Json::exception &exception) {
    return std::unexpected(
        invalid("typed节点字段无效: " + std::string(exception.what()),
                "settings." + std::string(field)));
  }
  Result<Node> changed = std::unexpected(invalid("节点表单更新失败", id));
  const auto &stream = std::get<StreamNode>(candidate.settings);
  if (field == "transport")
    changed = edit_node_transport(*original, stream.transport);
  else if (field == "security")
    changed = edit_node_security(*original, stream.security);
  else if (field == "sockopt")
    changed = edit_node_sockopt(*original, stream.sockopt);
  else if (field == "finalmask")
    changed = edit_node_finalmask(*original, stream.finalmask);
  else
    changed = edit_node_mux(*original, stream.mux);
  if (!changed)
    return std::unexpected(changed.error());
  if (auto saved = store_->put_node(*changed); !saved)
    return std::unexpected(saved.error());
  return summarize_node(*changed);
}

Store &Application::store() { return *store_; }
SubscriptionService &Application::subscriptions() { return *subscriptions_; }

Result<std::vector<CoreInstall>> Application::list_cores() const {
  return store_->cores();
}

Result<void> Application::remove_managed_core(const Id &core_id) {
  auto core = store_->get_core(core_id);
  if (!core)
    return std::unexpected(core.error());
  if (!core->managed)
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "系统 Xray 只能登记和使用，不能由 XRM 删除",
                                    core_id, std::nullopt});
  auto profiles = profiles_->list();
  if (!profiles)
    return std::unexpected(profiles.error());
  for (const auto &summary : *profiles) {
    auto profile = profiles_->get(summary.id);
    if (!profile)
      return std::unexpected(profile.error());
    if (profile->core_id == core_id)
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Xray 仍被 Profile 使用", summary.name,
                                      std::nullopt});
  }
  auto resources = store_->geodata_resources();
  if (!resources)
    return std::unexpected(resources.error());
  for (const auto &resource : *resources)
    if (resource.validator_core_id == core_id)
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Xray仍被Geodata资源验证设置引用",
                                      resource.name, std::nullopt});
  const auto status = core_manager_->status();
  if (status.active && status.active->core_id == core_id)
    return std::unexpected(AppError{ErrorCode::conflict, "Xray 当前仍在运行",
                                    core_id, std::nullopt});
  const auto prefix = std::string("managed-");
  if (!core_id.starts_with(prefix) || core_id.size() == prefix.size())
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "托管核心登记路径无效", core_id,
                                    std::nullopt});
  const auto tag = core_id.substr(prefix.size());
  if (tag == "." || tag == ".." || tag.find('/') != std::string::npos ||
      tag.find('\\') != std::string::npos ||
      core->path != data_root_ / "cores" / tag / "xray")
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "托管核心目录与登记不符，未删除",
                                    core->path.string(), std::nullopt});
  std::error_code error;
  std::filesystem::remove_all(core->path.parent_path(), error);
  if (error)
    return std::unexpected(AppError{ErrorCode::io_error, "删除托管核心目录失败",
                                    error.message(), std::nullopt});
  return store_->remove_core(core_id);
}

Result<Id>
Application::core_releases(bool include_prerelease,
                           Completion<std::vector<CoreRelease>> completion) {
  return core_installer_->releases(include_prerelease, std::move(completion));
}

Result<Id> Application::install_core(InstallRequest request,
                                     Completion<CoreInstall> completion) {
  auto *store = store_;
  return core_installer_->install(
      std::move(request), [store, completion = std::move(completion)](
                              Result<CoreInstall> result) mutable {
        if (!result) {
          completion(std::unexpected(result.error()));
          return;
        }
        if (auto saved = store->put_core(*result); !saved) {
          std::error_code ignored;
          std::filesystem::remove_all(result->path.parent_path(), ignored);
          completion(std::unexpected(saved.error()));
          return;
        }
        completion(std::move(*result));
      });
}

void Application::use_core(const Id &profile_id, const Id &core_id,
                           Completion<ActivationResult> completion) {
  auto profile = profiles_->get(profile_id);
  if (!profile) {
    completion(std::unexpected(profile.error()));
    return;
  }
  auto core = store_->get_core(core_id);
  if (!core) {
    completion(std::unexpected(core.error()));
    return;
  }
  if (profile->core_id == core_id) {
    completion(ActivationResult{ActivationEffect::checked_only, std::nullopt,
                                std::nullopt});
    return;
  }
  auto profile_path = store_->profile_path(profile_id);
  if (!profile_path) {
    completion(std::unexpected(profile_path.error()));
    return;
  }
  auto current = read_text(*profile_path / "profile.json", 8 * 1024 * 1024);
  if (!current) {
    completion(std::unexpected(current.error()));
    return;
  }
  const auto previous_core_id = profile->core_id;
  profile->core_id = core_id;
  if (auto saved =
          profiles_->save(profile_id, ProfileEdit{{*current}, *profile});
      !saved) {
    completion(std::unexpected(saved.error()));
    return;
  }

  const auto status = core_manager_->status();
  if (!status.active || status.active->profile_id != profile_id) {
    completion(ActivationResult{ActivationEffect::checked_only, std::nullopt,
                                std::nullopt});
    return;
  }
  auto restore_profile = [this, profile_id,
                          previous_core_id]() -> Result<void> {
    auto value = profiles_->get(profile_id);
    if (!value)
      return std::unexpected(value.error());
    auto path = store_->profile_path(profile_id);
    if (!path)
      return std::unexpected(path.error());
    auto text = read_text(*path / "profile.json", 8 * 1024 * 1024);
    if (!text)
      return std::unexpected(text.error());
    value->core_id = previous_core_id;
    return profiles_->save(profile_id, ProfileEdit{{*text}, std::move(*value)});
  };
  auto done =
      std::make_shared<Completion<ActivationResult>>(std::move(completion));
  auto accepted = core_manager_->activate(
      ActivationRequest{
          ActivationInput{ReplayRequest{*status.active, std::move(*core)}},
          ActivationMode::start_or_replace},
      [done, restore_profile](Result<ActivationResult> result) mutable {
        if (!result || result->effect != ActivationEffect::running_new) {
          auto saved = restore_profile();
          if (!result) {
            (*done)(std::unexpected(result.error()));
            return;
          }
          if (!saved) {
            (*done)(std::unexpected(saved.error()));
            return;
          }
          if (result->attempt_error) {
            (*done)(std::unexpected(*result->attempt_error));
            return;
          }
          (*done)(std::unexpected(AppError{ErrorCode::core_check_failed,
                                           "切换核心未能运行新实例",
                                           std::nullopt, std::nullopt}));
          return;
        }
        (*done)(std::move(*result));
      });
  if (!accepted) {
    // activate() rejected the request before taking ownership of the callback.
    // The saved core ID must be restored even on this synchronous path.
    if (auto restored = restore_profile(); !restored) {
      (*done)(std::unexpected(restored.error()));
      return;
    }
    (*done)(std::unexpected(accepted.error()));
  }
}

void Application::register_core(std::filesystem::path path, std::string name,
                                Completion<CoreInstall> completion) {
  if (path.empty() || name.empty()) {
    completion(std::unexpected(invalid("核心路径和名称不能为空", "path")));
    return;
  }
  if (!path.is_absolute() || ::access(path.c_str(), X_OK) != 0) {
    completion(
        std::unexpected(invalid("核心必须是可执行文件的绝对路径", "path")));
    return;
  }
  const auto id = new_id();
  auto *store = store_;
  const auto core_name = std::move(name);
  const auto core_path = std::move(path);
  auto accepted = processes_->execute(
      version_command(core_path), Milliseconds{5000},
      [completion, store, id, core_name,
       core_path](Result<ProcessResult> process_result) mutable {
        if (!process_result) {
          completion(std::unexpected(process_result.error()));
          return;
        }
        auto version = read_core_version(*process_result);
        if (!version) {
          completion(std::unexpected(version.error()));
          return;
        }
        CoreInstall core{id, core_name, core_path, std::move(*version), false};
        if (auto saved = store->put_core(core); !saved) {
          completion(std::unexpected(saved.error()));
          return;
        }
        completion(std::move(core));
      });
  if (!accepted)
    completion(std::unexpected(accepted.error()));
}

ProfileService &Application::profiles() { return *profiles_; }
Operations &Application::operations() { return operations_; }
CoreManager &Application::core_manager() { return *core_manager_; }
LogBuffer &Application::logs() { return logs_; }

Result<BuildInput> Application::build_input(const Id &profile_id) {
  auto profile = profiles_->snapshot(profile_id);
  if (!profile)
    return std::unexpected(profile.error());
  auto groups = profiles_->groups(profile_id);
  if (!groups)
    return std::unexpected(groups.error());
  auto observation = profiles_->observation(profile_id);
  if (!observation)
    return std::unexpected(observation.error());
  profile->bindings.groups = std::move(*groups);
  profile->bindings.observation = std::move(*observation);
  auto nodes = store_->all_nodes();
  if (!nodes)
    return std::unexpected(nodes.error());
  if (profile->profile.enabled_source_ids) {
    const auto &allowed = *profile->profile.enabled_source_ids;
    std::erase_if(*nodes, [&](const Node &node) {
      return node.source_id && std::find(allowed.begin(), allowed.end(),
                                         *node.source_id) == allowed.end();
    });
  }
  return BuildInput{std::move(*profile), std::move(*nodes)};
}

Result<void> Application::select(const Id &profile_id,
                                 std::optional<Target> target) {
  auto profile = profiles_->get(profile_id);
  if (!profile)
    return std::unexpected(profile.error());
  if (target && target->kind == TargetKind::node) {
    auto node = store_->get_node(target->id_or_tag);
    if (!node)
      return std::unexpected(node.error());
    if (!node->enabled || node->source_missing)
      return std::unexpected(AppError{ErrorCode::not_found,
                                      "节点已停用或来源缺失", target->id_or_tag,
                                      std::nullopt});
    if (node->source_id && profile->enabled_source_ids &&
        std::find(profile->enabled_source_ids->begin(),
                  profile->enabled_source_ids->end(),
                  *node->source_id) == profile->enabled_source_ids->end())
      return std::unexpected(AppError{
          ErrorCode::conflict,
          "节点来源未用于当前 Profile；请在 Subscriptions 中启用该来源",
          node->source_id, std::nullopt});
  } else if (target && target->kind == TargetKind::group) {
    auto groups = profiles_->groups(profile_id);
    if (!groups)
      return std::unexpected(groups.error());
    if (std::none_of(groups->begin(), groups->end(),
                     [&](const GroupBinding &group) {
                       return group.id == target->id_or_tag;
                     }))
      return std::unexpected(AppError{ErrorCode::not_found, "找不到策略组",
                                      target->id_or_tag, std::nullopt});
  }
  auto profile_path = store_->profile_path(profile_id);
  if (!profile_path)
    return std::unexpected(profile_path.error());
  auto current = read_text(*profile_path / "profile.json", 8 * 1024 * 1024);
  if (!current)
    return std::unexpected(current.error());
  profile->selected = std::move(target);
  return profiles_->save(profile_id,
                         ProfileEdit{{*current}, std::move(*profile)});
}

Result<void> Application::set_profile_sources(const Id &profile_id,
                                              std::vector<Id> source_ids) {
  auto profile = profiles_->get(profile_id);
  if (!profile)
    return std::unexpected(profile.error());
  std::sort(source_ids.begin(), source_ids.end());
  source_ids.erase(std::unique(source_ids.begin(), source_ids.end()),
                   source_ids.end());
  for (const auto &id : source_ids) {
    auto source = store_->get_source(id);
    if (!source)
      return std::unexpected(source.error());
  }
  auto path = store_->profile_path(profile_id);
  if (!path)
    return std::unexpected(path.error());
  auto current = read_text(*path / "profile.json", 8 * 1024 * 1024);
  if (!current)
    return std::unexpected(current.error());
  profile->enabled_source_ids = std::move(source_ids);
  return profiles_->save(profile_id,
                         ProfileEdit{{*current}, std::move(*profile)});
}

Result<std::vector<RuntimeAssetFile>>
Application::resolve_runtime_assets(const ProfileSettings &profile) const {
  std::vector<RuntimeAssetFile> assets;
  if (profile.geodata_resource_ids.empty())
    return assets;
  if (profile.environment.asset_dir ||
      profile.environment.process_environment.contains("XRAY_LOCATION_ASSET")) {
    return std::unexpected(
        AppError{ErrorCode::conflict,
                 "Profile已有资源目录设置；不能隐式覆盖或丢弃该目录的其他资源",
                 profile.id, "先明确整理资源目录，再应用共享Geodata绑定"});
  }
  for (const auto &[file_name, id] : profile.geodata_resource_ids) {
    const auto file = std::filesystem::path(file_name);
    if (file.empty() || file != file.filename() || file == "." || file == "..")
      return std::unexpected(
          invalid("Geodata绑定文件名必须是单个文件名", file_name));
    auto resource = store_->get_geodata_resource(id);
    if (!resource)
      return std::unexpected(resource.error());
    if (resource->file_name != file_name || !resource->current ||
        resource->current->file_name != file_name) {
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Geodata资源尚无匹配的已验证版本", id,
                                      "先更新资源，再显式连接或应用"});
    }
    assets.push_back({resource->current->path, file_name});
  }
  return assets;
}

Result<void>
Application::set_profile_geodata(const Id &profile_id,
                                 std::map<std::string, Id> resources) {
  auto profile = profiles_->get(profile_id);
  if (!profile)
    return std::unexpected(profile.error());
  profile->geodata_resource_ids = std::move(resources);
  auto assets = resolve_runtime_assets(*profile);
  if (!assets)
    return std::unexpected(assets.error());
  auto path = store_->profile_path(profile_id);
  if (!path)
    return std::unexpected(path.error());
  auto current = read_text(*path / "profile.json", 8 * 1024 * 1024);
  if (!current)
    return std::unexpected(current.error());
  return profiles_->save(profile_id,
                         ProfileEdit{{*current}, std::move(*profile)});
}

Result<std::vector<GeodataResourceRecord>>
Application::list_geodata_resources() const {
  return store_->geodata_resources();
}

Result<GeodataResourceRecord>
Application::get_geodata_resource(const Id &id) const {
  return store_->get_geodata_resource(id);
}

Result<GeodataResourceRecord>
Application::put_geodata_resource(GeodataResourceRecord record) {
  if (record.id.empty())
    record.id = new_id();
  if (record.auto_update && record.update_interval.count() > 0 &&
      record.validator_core_id.empty())
    return std::unexpected(
        invalid("自动更新需要指定用于验证的核心", record.id));
  if (!record.validator_core_id.empty()) {
    auto core = store_->get_core(record.validator_core_id);
    if (!core)
      return std::unexpected(core.error());
  }
  auto saved = store_->put_geodata_resource(record);
  if (saved)
    schedule_geodata_updates();
  return saved;
}

Result<void> Application::remove_geodata_resource(const Id &id) {
  auto ids = store_->profile_ids();
  if (!ids)
    return std::unexpected(ids.error());
  for (const auto &profile_id : *ids) {
    auto profile = profiles_->get(profile_id);
    if (!profile)
      return std::unexpected(profile.error());
    for (const auto &[file_name, resource_id] : profile->geodata_resource_ids) {
      if (resource_id == id)
        return std::unexpected(
            AppError{ErrorCode::conflict, "Geodata资源仍被Profile使用",
                     profile->name + ":" + file_name, std::nullopt});
    }
  }
  auto removed = store_->remove_geodata_resource(id);
  if (removed)
    schedule_geodata_updates();
  return removed;
}

Result<Id> Application::update_geodata_resource(const Id &resource_id,
                                                const Id &core_id) {
  if (geodata_inflight_.contains(resource_id))
    return std::unexpected(AppError{ErrorCode::busy,
                                    "该Geodata资源已有更新任务", resource_id,
                                    std::nullopt});
  auto resource = store_->get_geodata_resource(resource_id);
  if (!resource)
    return std::unexpected(resource.error());
  auto core = store_->get_core(core_id);
  if (!core)
    return std::unexpected(core.error());
  GeodataAcquireRequest request;
  request.resource_id = resource->id;
  request.kind = resource->kind;
  request.file_name = resource->file_name;
  request.url = resource->url;
  request.local_source = resource->local_path;
  request.validator_binary = core->path;
  const auto revision = resource->settings_revision;
  const auto operation = operations_.begin();
  auto accepted = geodata_cache_->acquire(
      std::move(request), [this, operation, resource_id,
                           revision](Result<GeodataAssetVersion> result) {
        geodata_inflight_.erase(resource_id);
        const auto now =
            std::chrono::duration_cast<Milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        auto saved = store_->commit_geodata_resource_result(
            resource_id, revision,
            result ? std::optional<GeodataAssetVersion>(*result) : std::nullopt,
            now, result ? "success" : result.error().message);
        if (!saved)
          operations_.complete(operation, std::unexpected(saved.error()));
        else if (!result)
          operations_.complete(operation, std::unexpected(result.error()));
        else
          operations_.complete(operation,
                               Json{{"resource_id", resource_id},
                                    {"version_id", result->version_id},
                                    {"bytes", result->bytes},
                                    {"labels", result->labels}});
        schedule_geodata_updates();
      });
  if (!accepted) {
    operations_.complete(operation, std::unexpected(accepted.error()));
    return std::unexpected(accepted.error());
  }
  geodata_inflight_.insert(resource_id);
  schedule_geodata_updates();
  return operation;
}

Result<Json>
Application::plan_routing_preset(const Id &profile_id,
                                 std::string_view preset_name) const {
  RoutingPresetRequest request;
  if (preset_name == "bypass_mainland")
    request.preset = RoutingPreset::bypass_mainland;
  else if (preset_name == "blacklist_proxy")
    request.preset = RoutingPreset::blacklist_proxy;
  else if (preset_name == "global_proxy")
    request.preset = RoutingPreset::global_proxy;
  else if (preset_name == "direct_only")
    request.preset = RoutingPreset::direct_only;
  else if (preset_name == "custom_native")
    request.preset = RoutingPreset::custom_native;
  else
    return std::unexpected(invalid("未知分流预设", std::string(preset_name)));
  auto profile = profiles_->get(profile_id);
  if (!profile)
    return std::unexpected(profile.error());
  auto routing = profiles_->module(profile_id, "routing");
  if (!routing)
    return std::unexpected(routing.error());
  if (!routing->document)
    return std::unexpected(invalid("routing模块尚无原生内容", profile_id));
  request.routing = *routing->document;
  auto outbounds = profiles_->module(profile_id, "outbounds");
  if (!outbounds)
    return std::unexpected(outbounds.error());
  if (outbounds->document) {
    try {
      auto root = Json::parse(outbounds->document->source_text);
      if (root.contains("outbounds") && root["outbounds"].is_array())
        for (const auto &outbound : root["outbounds"])
          if (outbound.is_object() && outbound.contains("tag") &&
              outbound["tag"].is_string())
            request.outbound_tags.insert(outbound["tag"].get<std::string>());
    } catch (const Json::exception &error) {
      return std::unexpected(invalid(
          "outbounds原生内容无效: " + std::string(error.what()), profile_id));
    }
  }
  auto nodes = store_->all_nodes();
  if (!nodes)
    return std::unexpected(nodes.error());
  for (const auto &node : *nodes) {
    if (!node.enabled || node.source_missing)
      continue;
    if (node.source_id && profile->enabled_source_ids &&
        std::find(profile->enabled_source_ids->begin(),
                  profile->enabled_source_ids->end(),
                  *node.source_id) == profile->enabled_source_ids->end())
      continue;
    request.outbound_tags.insert(node_tag(node.id));
  }
  auto groups = profiles_->groups(profile_id);
  if (!groups)
    return std::unexpected(groups.error());
  for (const auto &group : *groups)
    if (group.balancer.is_object() && group.balancer.contains("tag") &&
        group.balancer["tag"].is_string())
      request.balancer_tags.insert(group.balancer["tag"].get<std::string>());
  try {
    const auto root = Json::parse(routing->document->source_text);
    const auto &native = root.at("routing");
    if (native.contains("balancers") && native["balancers"].is_array())
      for (const auto &balancer : native["balancers"])
        if (balancer.is_object() && balancer.contains("tag") &&
            balancer["tag"].is_string())
          request.balancer_tags.insert(balancer["tag"].get<std::string>());
  } catch (const Json::exception &error) {
    return std::unexpected(invalid(
        "routing原生内容无效: " + std::string(error.what()), profile_id));
  }
  request.protected_inbound_tags.insert("api");
  if (profile->selected) {
    const auto &selected = *profile->selected;
    if (selected.kind == TargetKind::node)
      request.proxy =
          RoutingProxyTarget{"outboundTag", node_tag(selected.id_or_tag)};
    else if (selected.kind == TargetKind::group) {
      const auto found =
          std::find_if(groups->begin(), groups->end(), [&](const auto &group) {
            return group.id == selected.id_or_tag;
          });
      if (found != groups->end() && found->balancer.contains("tag") &&
          found->balancer["tag"].is_string())
        request.proxy = RoutingProxyTarget{
            "balancerTag", found->balancer["tag"].get<std::string>()};
    } else if (selected.kind == TargetKind::native_outbound)
      request.proxy = RoutingProxyTarget{"outboundTag", selected.id_or_tag};
    else if (selected.kind == TargetKind::native_balancer)
      request.proxy = RoutingProxyTarget{"balancerTag", selected.id_or_tag};
  }
  for (const auto &[file_name, resource_id] : profile->geodata_resource_ids) {
    auto resource = store_->get_geodata_resource(resource_id);
    if (!resource)
      return std::unexpected(resource.error());
    if (!resource->current || resource->file_name != file_name)
      return std::unexpected(
          invalid("分流预设资源尚无匹配的已验证版本", resource_id));
    if (file_name == "geoip.dat" && resource->kind == GeoAssetKind::geoip)
      request.geoip_labels.insert(resource->current->labels.begin(),
                                  resource->current->labels.end());
    if (file_name == "geosite.dat" && resource->kind == GeoAssetKind::geosite)
      request.geosite_labels.insert(resource->current->labels.begin(),
                                    resource->current->labels.end());
  }
  auto planned = xrm::plan_routing_preset(request);
  if (!planned)
    return std::unexpected(planned.error());
  return Json{{"before", planned->native_before},
              {"after", planned->native_after},
              {"expected_text", planned->edit.base.expected_text.value_or("")},
              {"text", planned->edit.text},
              {"changed", planned->changed},
              {"required_geoip_labels", planned->required_geoip_labels},
              {"required_geosite_labels", planned->required_geosite_labels}};
}

Result<Json> Application::save_routing_preset(const Id &profile_id,
                                              std::string_view preset,
                                              std::string_view expected_text) {
  auto planned = plan_routing_preset(profile_id, preset);
  if (!planned)
    return std::unexpected(planned.error());
  if (planned->at("expected_text").get<std::string>() != expected_text)
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "routing原文已变化，请重新预览差异",
                                    profile_id, std::nullopt});
  if (!planned->at("changed").get<bool>())
    return Json{{"saved", false}, {"changed", false}};
  auto saved = profiles_->save_module(
      profile_id, ModuleEdit{"routing",
                             {std::string(expected_text)},
                             planned->at("text").get<std::string>()});
  if (!saved)
    return std::unexpected(saved.error());
  return Json{{"saved", true}, {"changed", true}, {"runtime_changed", false}};
}

void Application::connect(const Id &profile_id, std::optional<Target> target,
                          Completion<ActivationResult> completion) {
  if (target) {
    auto selected = select(profile_id, std::move(target));
    if (!selected) {
      completion(std::unexpected(selected.error()));
      return;
    }
  }
  auto input = build_input(profile_id);
  if (!input) {
    completion(std::unexpected(input.error()));
    return;
  }
  auto core = store_->get_core(input->settings.profile.core_id);
  if (!core) {
    completion(std::unexpected(core.error()));
    return;
  }
  auto assets = resolve_runtime_assets(input->settings.profile);
  if (!assets) {
    completion(std::unexpected(assets.error()));
    return;
  }
  auto accepted = core_manager_->activate(
      ActivationRequest{
          ActivationInput{BuildRequest{std::move(*input), std::move(*core),
                                       std::move(*assets)}},
          ActivationMode::start_or_replace},
      completion);
  if (!accepted)
    completion(std::unexpected(accepted.error()));
}

void Application::check(const Id &profile_id,
                        Completion<ActivationResult> completion) {
  auto input = build_input(profile_id);
  if (!input) {
    completion(std::unexpected(input.error()));
    return;
  }
  auto core = store_->get_core(input->settings.profile.core_id);
  if (!core) {
    completion(std::unexpected(core.error()));
    return;
  }
  auto assets = resolve_runtime_assets(input->settings.profile);
  if (!assets) {
    completion(std::unexpected(assets.error()));
    return;
  }
  auto accepted = core_manager_->activate(
      ActivationRequest{
          ActivationInput{BuildRequest{std::move(*input), std::move(*core),
                                       std::move(*assets)}},
          ActivationMode::check_only},
      completion);
  if (!accepted)
    completion(std::unexpected(accepted.error()));
}

void Application::dump(const Id &profile_id, Completion<Json> completion) {
  auto input = build_input(profile_id);
  if (!input) {
    completion(std::unexpected(input.error()));
    return;
  }
  auto core = store_->get_core(input->settings.profile.core_id);
  if (!core) {
    completion(std::unexpected(core.error()));
    return;
  }
  auto files = assemble(*input);
  if (!files) {
    completion(std::unexpected(files.error()));
    return;
  }
  auto assets = resolve_runtime_assets(input->settings.profile);
  if (!assets) {
    completion(std::unexpected(assets.error()));
    return;
  }
  std::vector<RuntimeWriter::AssetSnapshot> snapshots;
  for (const auto &asset : *assets)
    snapshots.push_back({asset.immutable_source, asset.runtime_name});
  const auto run_id =
      "dump-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  auto directory = runtime_writer_->materialize(
      *files, DirectoryPurpose::preview, run_id, snapshots);
  if (!directory) {
    completion(std::unexpected(directory.error()));
    return;
  }
  const auto &environment = input->settings.profile.environment;
  auto process_environment = environment.process_environment;
  if (!assets->empty())
    process_environment["XRAY_LOCATION_ASSET"] = directory->confdir.string();
  else if (environment.asset_dir &&
           !process_environment.contains("XRAY_LOCATION_ASSET"))
    process_environment["XRAY_LOCATION_ASSET"] =
        environment.asset_dir->string();
  RunSpec spec{core->path, directory->confdir,
               environment.cwd.empty() ? std::filesystem::current_path()
                                       : environment.cwd,
               std::move(process_environment)};
  auto accepted = processes_->execute(
      dump_command(spec), Milliseconds{30000},
      [completion,
       confdir = directory->confdir](Result<ProcessResult> result) mutable {
        if (!result) {
          completion(std::unexpected(result.error()));
          return;
        }
        if (result->exit.exit_code != 0 || result->exit.term_signal != 0) {
          completion(std::unexpected(
              AppError{ErrorCode::core_check_failed, "Xray dump失败",
                       result->stderr_text, std::nullopt}));
          return;
        }
        completion(Json{{"confdir", confdir.string()},
                        {"stdout", result->stdout_text},
                        {"stderr", result->stderr_text}});
      });
  if (!accepted)
    completion(std::unexpected(accepted.error()));
}

Result<PreparedDirectory> Application::render(const Id &profile_id) {
  auto input = build_input(profile_id);
  if (!input)
    return std::unexpected(input.error());
  auto files = assemble(*input);
  if (!files)
    return std::unexpected(files.error());
  auto assets = resolve_runtime_assets(input->settings.profile);
  if (!assets)
    return std::unexpected(assets.error());
  std::vector<RuntimeWriter::AssetSnapshot> snapshots;
  for (const auto &asset : *assets)
    snapshots.push_back({asset.immutable_source, asset.runtime_name});
  const auto id =
      "preview-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  return runtime_writer_->materialize(*files, DirectoryPurpose::preview, id,
                                      snapshots);
}

Result<void>
Application::export_config(const Id &profile_id, std::string_view source,
                           const std::filesystem::path &destination) {
  if (source == "runtime") {
    const auto status = core_manager_->status();
    if (!status.active)
      return std::unexpected(AppError{ErrorCode::not_found,
                                      "当前没有可导出的运行配置", std::nullopt,
                                      std::nullopt});
    return runtime_writer_->export_files(status.active->spec.confdir,
                                         destination);
  }
  if (source != "saved")
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "source必须为saved或runtime",
                                    std::string(source), std::nullopt});
  auto preview = render(profile_id);
  if (!preview)
    return std::unexpected(preview.error());
  return runtime_writer_->export_files(preview->confdir, destination);
}

namespace {
const char *core_state_name(CoreState state) {
  switch (state) {
  case CoreState::stopped:
    return "stopped";
  case CoreState::starting:
    return "starting";
  case CoreState::running:
    return "running";
  case CoreState::stopping:
    return "stopping";
  case CoreState::failed:
    return "failed";
  }
  return "failed";
}
const char *health_name(RuntimeHealth health) {
  switch (health) {
  case RuntimeHealth::process_only:
    return "process_only";
  case RuntimeHealth::control_ready:
    return "control_ready";
  case RuntimeHealth::control_unavailable:
    return "control_unavailable";
  }
  return "process_only";
}
} // namespace

Result<Json> Application::runtime_status() const {
  const auto status = core_manager_->status();
  const bool override_current =
      status.active && override_run_id_ == status.active->run_id;
  auto target_json = [](const std::optional<Target> &target) -> Json {
    if (!target)
      return nullptr;
    const char *kind = target->kind == TargetKind::node    ? "node"
                       : target->kind == TargetKind::group ? "group"
                       : target->kind == TargetKind::native_outbound
                           ? "native_outbound"
                           : "native_balancer";
    return Json{{"kind", kind}, {"id_or_tag", target->id_or_tag}};
  };
  Json active = nullptr;
  if (status.active) {
    const auto core = store_->get_core(status.active->core_id);
    active = Json{{"run_id", status.active->run_id},
                  {"profile_id", status.active->profile_id},
                  {"core_id", status.active->core_id},
                  {"confdir", status.active->spec.confdir.string()},
                  {"core_version", core ? core->reported_version : "unknown"},
                  {"selection", target_json(status.active->applied_selection)}};
    if (status.active->applied_selection &&
        status.active->applied_selection->kind == TargetKind::node) {
      const auto &selected_id = status.active->applied_selection->id_or_tag;
      const auto entry = std::find_if(
          status.active->index.nodes.begin(), status.active->index.nodes.end(),
          [&](const NodeRuntimeEntry &node) { return node.id == selected_id; });
      if (entry != status.active->index.nodes.end()) {
        active["selection_display_name"] = entry->display_name;
        active["selection_in_inventory"] =
            static_cast<bool>(store_->get_node(selected_id));
      }
    }
  }
  Json saved_target = nullptr;
  if (status.active) {
    auto profile = profiles_->get(status.active->profile_id);
    if (profile)
      saved_target = target_json(profile->selected);
  }
  Json error = nullptr;
  if (status.last_error)
    error = error_to_json(*status.last_error);
  return Json{{"state", core_state_name(status.state)},
              {"desired_running", status.desired_running},
              {"pid", status.pid ? Json(*status.pid) : Json(nullptr)},
              {"health", health_name(status.health)},
              {"active", std::move(active)},
              {"saved_target", std::move(saved_target)},
              {"actual_target",
               status.active ? target_json(status.active->applied_selection)
                             : Json(nullptr)},
              {"override_state",
               override_current
                   ? (override_state_ == OverrideState::confirmed ? "confirmed"
                      : override_state_ == OverrideState::unknown ? "unknown"
                                                                  : "none")
                   : "none"},
              {"runtime_override", override_current && runtime_override_
                                       ? Json(*runtime_override_)
                                       : Json(nullptr)},
              {"error", std::move(error)}};
}

Result<ModuleChangePlan>
Application::plan_module_changes(const Id &profile_id, std::string_view action,
                                 const Json &arguments) const {
  if (action == "enable_traffic_statistics" ||
      action == "disable_traffic_statistics") {
    const auto enabled = action == "enable_traffic_statistics";
    const auto listen = arguments.value("metrics_listen", std::string{});
    auto snapshot = profiles_->snapshot(profile_id);
    if (!snapshot)
      return std::unexpected(snapshot.error());
    return plan_traffic_stats(*snapshot, enabled, listen);
  }
  bool include_after_save = false;
  bool source_exists = true;
  auto module = [&](std::string_view key) -> Result<ModuleDocument> {
    auto view = profiles_->module(profile_id, key);
    if (!view)
      return std::unexpected(view.error());
    include_after_save = view->setting.included;
    if (!view->document) {
      source_exists = false;
      if (key != "env" && key != "version" && key != "geodata" &&
          key != "api" && key != "metrics" && key != "log")
        return std::unexpected(invalid(
            "源模块不存在且此表单没有安全的默认原生对象", std::string(key)));
      return ModuleDocument{std::string(key), view->setting.filename,
                            Json{{std::string(key), Json::object()}}.dump(2) +
                                "\n"};
    }
    source_exists = true;
    return *view->document;
  };
  Result<ModuleEdit> edit =
      std::unexpected(invalid("未知的专用模块表单", std::string(action)));
  if (action == "env.set" || action == "env.unset") {
    auto document = module("env");
    if (!document)
      return std::unexpected(document.error());
    const auto key = arguments.value("key", std::string{});
    if (key.empty())
      return std::unexpected(invalid("env表单需要key", "key"));
    if (action == "env.set") {
      if (!arguments.contains("value") || !arguments["value"].is_string())
        return std::unexpected(invalid("env.set需要字符串value", "value"));
      edit = put_env(*document, key, arguments["value"].get<std::string>());
    } else
      edit = erase_env(*document, key);
  } else if (action == "version.range") {
    auto document = module("version");
    if (!document)
      return std::unexpected(document.error());
    const auto min = arguments.value("min", Json(nullptr));
    const auto max = arguments.value("max", Json(nullptr));
    if ((!min.is_null() && !min.is_string()) ||
        (!max.is_null() && !max.is_string()))
      return std::unexpected(
          invalid("version min/max必须是字符串或null", "version"));
    edit = set_version_range(
        *document,
        min.is_null() ? std::nullopt
                      : std::optional<std::string>(min.get<std::string>()),
        max.is_null() ? std::nullopt
                      : std::optional<std::string>(max.get<std::string>()));
  } else if (action == "geodata.assets") {
    auto document = module("geodata");
    if (!document)
      return std::unexpected(document.error());
    if (!arguments.contains("assets") || !arguments["assets"].is_array())
      return std::unexpected(invalid("geodata.assets需要assets数组", "assets"));
    edit = replace_geodata_assets(*document,
                                  arguments["assets"].get<std::vector<Json>>());
  } else if (action == "geodata.schedule") {
    auto document = module("geodata");
    if (!document)
      return std::unexpected(document.error());
    const auto cron = arguments.value("cron", std::string{});
    if (cron.empty())
      return std::unexpected(invalid("geodata.schedule需要cron", "cron"));
    edit = set_geodata_schedule(*document, cron);
  } else if (action == "api.services") {
    auto document = module("api");
    if (!document)
      return std::unexpected(document.error());
    if (!arguments.contains("services") || !arguments["services"].is_array())
      return std::unexpected(
          invalid("api.services需要字符串services数组", "services"));
    try {
      edit = set_api_services(
          *document, arguments["services"].get<std::vector<std::string>>());
    } catch (const Json::exception &error) {
      return std::unexpected(invalid(error.what(), "services"));
    }
  } else if (action == "metrics.listen") {
    auto document = module("metrics");
    if (!document)
      return std::unexpected(document.error());
    const auto listen = arguments.value("listen", std::string{});
    if (listen.empty())
      return std::unexpected(invalid("metrics.listen需要listen", "listen"));
    edit = set_metrics_listener(*document, listen);
  } else if (action == "log.level") {
    auto document = module("log");
    if (!document)
      return std::unexpected(document.error());
    const auto level = arguments.value("level", std::string{});
    edit = set_log_level(*document, level.empty()
                                        ? std::nullopt
                                        : std::optional<std::string>(level));
  } else {
    return std::unexpected(invalid("不支持的模块表单", std::string(action)));
  }
  if (!edit)
    return std::unexpected(edit.error());
  if (!source_exists)
    edit->base.expected_text.reset();
  return ModuleChangePlan{{ModuleChange{std::move(*edit), include_after_save}},
                          {}};
}

SaveManyResult Application::save_module_changes(const Id &profile_id,
                                                const ModuleChangePlan &plan) {
  return xrm::save_module_changes(*profiles_, profile_id, plan);
}

Json Application::runtime_metrics(const Id &connection_id, bool watch) {
  if (watch)
    metrics_viewers_.insert(connection_id);
  else
    metrics_viewers_.erase(connection_id);
  metrics_client_->set_viewer_count(metrics_viewers_.size());
  const auto status = core_manager_->status();
  Json missing = Json::array();
  std::optional<std::string> endpoint;
  std::optional<RunRecord> record = status.active;
  if (!record || status.state != CoreState::running) {
    missing.push_back("core not running");
    record.reset();
  } else {
    const auto directory = record->spec.confdir;
    for (const auto &[key, filename] :
         std::array<std::pair<std::string_view, std::string_view>, 3>{
             std::pair{"stats", "08_stats.json"},
             {"policy", "09_policy.json"},
             {"metrics", "07_metrics.json"}}) {
      if (!std::filesystem::exists(directory / filename))
        missing.push_back(key);
    }
    try {
      const auto metrics_path = directory / "07_metrics.json";
      if (std::filesystem::exists(metrics_path)) {
        std::ifstream input(metrics_path);
        const auto root = Json::parse(input).at("metrics");
        const auto listen = root.at("listen").get<std::string>();
        const auto separator = listen.rfind(':');
        if (separator != std::string::npos) {
          const auto host = listen.substr(0, separator);
          const auto port_text = std::string_view(listen).substr(separator + 1);
          unsigned port = 0;
          const auto parsed = std::from_chars(
              port_text.data(), port_text.data() + port_text.size(), port);
          const bool loopback =
              host == "127.0.0.1" || host == "localhost" || host == "[::1]";
          if (loopback && parsed.ec == std::errc{} &&
              parsed.ptr == port_text.data() + port_text.size() && port > 0 &&
              port <= 65535)
            endpoint = "http://" + listen + "/debug/vars";
        }
      }
    } catch (const std::exception &) {
      missing.push_back("metrics endpoint unavailable");
    }
  }
  if (record) {
    if (metrics_run_id_ != record->run_id) {
      metrics_run_id_ = record->run_id;
      metrics_client_->attach(*record, endpoint);
    }
    if (!endpoint && std::find(missing.begin(), missing.end(),
                               Json("metrics")) == missing.end())
      missing.push_back("metrics endpoint unavailable");
  } else if (metrics_run_id_) {
    metrics_run_id_.reset();
    RunRecord empty{};
    metrics_client_->attach(empty, std::nullopt);
  }
  const auto view = metrics_client_->view();
  RuntimeMetricsDto dto{view.availability, view.sample, view.rates};
  Json result = dto;
  result["missing_conditions"] = std::move(missing);
  if (!result["missing_conditions"].empty()) {
    result["availability"] = "unavailable";
    result["sample"] = nullptr;
    result["rates"] = Json::array();
  } else if (!view.sample) {
    result["sample"] = nullptr;
  }
  if (view.sample && result["missing_conditions"].empty()) {
    Json counters = Json::array();
    for (const auto &value : view.sample->traffic)
      counters.push_back(
          Json{{"scope", value.scope},
               {"tag", value.tag},
               {"uplink", value.uplink ? Json(*value.uplink) : Json(nullptr)},
               {"downlink",
                value.downlink ? Json(*value.downlink) : Json(nullptr)}});
    Json observations = Json::array();
    for (const auto &value : view.sample->observations)
      observations.push_back(
          Json{{"tag", value.tag},
               {"alive", value.alive ? Json(*value.alive) : Json(nullptr)},
               {"latency_ms",
                value.latency_ms ? Json(*value.latency_ms) : Json(nullptr)}});
    result["sample"] = Json{{"run_id", view.sample->run_id},
                            {"received_at", view.sample->received_at},
                            {"traffic", std::move(counters)},
                            {"observations", std::move(observations)}};
  }
  return result;
}

void Application::release_metrics_viewer(const Id &connection_id) {
  metrics_viewers_.erase(connection_id);
  metrics_client_->set_viewer_count(metrics_viewers_.size());
}

Result<ControlTarget> Application::control_target() const {
  const auto status = core_manager_->status();
  if (!status.active || status.state != CoreState::running)
    return std::unexpected(AppError{ErrorCode::not_found, "没有正在运行的核心",
                                    std::nullopt, std::nullopt});
  const auto api_path = status.active->spec.confdir / "06_api.json";
  const auto inbounds_path = status.active->spec.confdir / "02_inbounds.json";
  auto api_text = read_text(api_path, 1024 * 1024);
  auto inbounds_text = read_text(inbounds_path, 8 * 1024 * 1024);
  if (!api_text || !inbounds_text)
    return std::unexpected(AppError{
        ErrorCode::unsupported, "当前运行配置没有可用的 Xray API 模块/入站",
        !api_text ? api_path.string() : inbounds_path.string(), std::nullopt});
  try {
    const auto api_root = Json::parse(*api_text).at("api");
    const auto tag = api_root.at("tag").get<std::string>();
    const auto inbounds_root = Json::parse(*inbounds_text).at("inbounds");
    if (!inbounds_root.is_array())
      throw std::invalid_argument("inbounds is not an array");
    for (const auto &inbound : inbounds_root) {
      if (inbound.value("tag", "") != tag)
        continue;
      const auto listen = inbound.value("listen", "");
      const auto port = inbound.at("port").get<std::uint16_t>();
      if (listen != "127.0.0.1" && listen != "::1" && listen != "localhost")
        return std::unexpected(
            AppError{ErrorCode::unsupported,
                     "运行中的 Xray API 入站不是 loopback，拒绝使用控制端点",
                     listen, std::nullopt});
      return ControlTarget{status.active->spec,
                           listen + ":" + std::to_string(port),
                           Milliseconds{3000}};
    }
    return std::unexpected(AppError{ErrorCode::unsupported,
                                    "找不到与 API tag 对应的 loopback 入站",
                                    tag, std::nullopt});
  } catch (const std::exception &error) {
    return std::unexpected(
        AppError{ErrorCode::invalid_argument,
                 "无法读取运行 Xray API 配置: " + std::string(error.what()),
                 api_path.string(), std::nullopt});
  }
}

Result<Id>
Application::override_runtime_group(std::string balancer, std::string outbound,
                                    Completion<OverrideResult> completion) {
  if (balancer.empty() || outbound.empty())
    return std::unexpected(
        invalid("需要 balancer 与 outbound tag", "group.override"));
  auto target = control_target();
  if (!target)
    return std::unexpected(target.error());
  const auto run_id = core_manager_->status().active->run_id;
  return xray_control_->override_outbound(
      std::move(*target), std::move(balancer), std::move(outbound),
      [this, run_id, completion = std::move(completion)](
          Result<OverrideResult> response) mutable {
        if (!response) {
          completion(std::unexpected(response.error()));
          return;
        }
        auto result = std::move(*response);
        if (result.confirmed) {
          override_run_id_ = run_id;
          runtime_override_ = result.actual_outbound;
          override_state_ = OverrideState::confirmed;
        } else {
          override_run_id_ = run_id;
          runtime_override_.reset();
          override_state_ = OverrideState::unknown;
        }
        completion(std::move(result));
      });
}

Result<Id> Application::clear_runtime_group_override(
    std::string balancer, Completion<OverrideResult> completion) {
  auto target = control_target();
  if (!target)
    return std::unexpected(target.error());
  const auto run_id = core_manager_->status().active->run_id;
  return xray_control_->clear_override(
      std::move(*target), std::move(balancer),
      [this, run_id, completion = std::move(completion)](
          Result<OverrideResult> response) mutable {
        if (!response) {
          completion(std::unexpected(response.error()));
          return;
        }
        auto result = std::move(*response);
        override_run_id_ = run_id;
        if (result.confirmed) {
          runtime_override_.reset();
          override_state_ = OverrideState::none;
        } else {
          runtime_override_.reset();
          override_state_ = OverrideState::unknown;
        }
        completion(std::move(result));
      });
}

Result<Json> Application::show_config(const Id &profile_id,
                                      std::string_view source,
                                      std::optional<std::string> module) const {
  if (source == "source") {
    if (!module)
      return std::unexpected(AppError{ErrorCode::invalid_argument,
                                      "读取source时必须指定module",
                                      std::nullopt, std::nullopt});
    auto view = profiles_->module(profile_id, *module);
    if (!view)
      return std::unexpected(view.error());
    return Json{{"source_file",
                 (std::filesystem::path("modules") / view->setting.filename)
                     .generic_string()},
                {"runtime_file", view->setting.filename.generic_string()},
                {"text", view->document ? Json(view->document->source_text)
                                        : Json(nullptr)}};
  }
  if (source == "runtime") {
    const auto status = core_manager_->status();
    if (!status.active)
      return std::unexpected(AppError{ErrorCode::not_found, "当前没有运行配置",
                                      std::nullopt, std::nullopt});
    Json files = Json::array();
    std::error_code ec;
    for (std::filesystem::directory_iterator
             it(status.active->spec.confdir, ec),
         end;
         it != end && !ec; it.increment(ec)) {
      if (!module ||
          it->path().filename().string().find(*module) != std::string::npos)
        files.push_back(Json{
            {"file", it->path().filename().string()},
            {"text", read_text(it->path(), 8 * 1024 * 1024).value_or("")}});
    }
    if (ec)
      return std::unexpected(AppError{ErrorCode::io_error, "无法读取运行目录",
                                      ec.message(), std::nullopt});
    return Json{{"runtime_dir", status.active->spec.confdir.string()},
                {"files", std::move(files)}};
  }
  return std::unexpected(AppError{ErrorCode::invalid_argument,
                                  "source必须为source或runtime",
                                  std::string(source), std::nullopt});
}

void Application::recover(Completion<ActivationResult> completion) {
  auto result =
      core_manager_->restore_after_server_start(std::move(completion));
  if (!result)
    return;
}

void Application::shutdown(std::function<void()> drained) {
  if (geodata_schedule_) {
    auto *schedule = std::exchange(geodata_schedule_, nullptr);
    schedule->owner = nullptr;
    uv_timer_stop(&schedule->timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&schedule->timer)))
      uv_close(reinterpret_cast<uv_handle_t *>(&schedule->timer),
               [](uv_handle_t *handle) {
                 delete static_cast<GeodataSchedule *>(handle->data);
               });
  }
  subscriptions_->stop_schedule();
  metrics_client_->shutdown();
  node_test_service_->shutdown([this, drained = std::move(drained)]() mutable {
    geodata_cache_->shutdown([this, drained = std::move(drained)]() mutable {
      core_manager_->shutdown(std::move(drained));
    });
  });
}

} // namespace xrm
