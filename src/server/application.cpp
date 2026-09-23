#include "server/application.hpp"

#include "infra/files.hpp"
#include "contract/wire.hpp"
#include "xray/process_adapter.hpp"
#include "xray/runtime_assembler.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <random>
#include <unistd.h>

namespace xrm {
namespace {

Id new_id() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char digits[] = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) id.push_back('-');
    id.push_back(digits[bytes[index] >> 4]);
    id.push_back(digits[bytes[index] & 0x0f]);
  }
  return id;
}

AppError invalid(std::string message, std::string subject) {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject), std::nullopt};
}

}  // namespace

Application::Application(Store& store, ProcessService& processes,
                         HttpClient& http, uv_loop_t& loop, std::filesystem::path data_root,
                         std::string application_version, Id boot_id,
                         SubscriptionService& subscriptions)
    : store_(&store), subscriptions_(&subscriptions), processes_(&processes), http_(&http),
      application_version_(std::move(application_version)), boot_id_(std::move(boot_id)),
      data_root_(std::move(data_root)),
      profiles_(std::make_unique<ProfileService>(store, data_root_ / "profiles")),
      runtime_writer_(std::make_unique<RuntimeWriter>(data_root_)),
      core_manager_(std::make_unique<CoreManager>(loop, store, processes, *runtime_writer_)),
      xray_control_(std::make_unique<XrayControl>(processes)),
      metrics_client_(std::make_unique<MetricsClient>(loop, http)),
      operations_(boot_id_) {
  core_manager_->on_output([this](LogStream stream, std::string bytes) {
    logs_.append(stream, bytes);
  });
}

Json Application::server_info() const {
  return Json{{"version", application_version_}, {"protocol", 1}, {"boot_id", boot_id_}};
}

Result<NodeSummary> Application::add_manual_node(std::string name, Json outbound) {
  if (name.empty()) return std::unexpected(invalid("节点名称不能为空", "name"));
  if (!outbound.is_object()) return std::unexpected(invalid("outbound必须是原生JSON对象", "outbound"));
  if (!outbound.contains("protocol") || !outbound.at("protocol").is_string() ||
      outbound.at("protocol").get<std::string>().empty()) {
    return std::unexpected(invalid("outbound必须包含非空protocol", "outbound.protocol"));
  }
  Node node{new_id(), std::nullopt, name, std::nullopt, {}, true, false,
            RawOutbound{std::move(outbound)}};
  if (auto stored = store_->put_node(node); !stored) return std::unexpected(stored.error());
  return summarize_node(node);
}

Result<Page<NodeSummary>> Application::list_nodes(const NodeQuery& query,
                                                  PageRequest request) const {
  auto nodes = store_->list_nodes(query, request);
  if (!nodes) return std::unexpected(nodes.error());
  Page<NodeSummary> page;
  page.total = nodes->total;
  page.next_offset = nodes->next_offset;
  page.items.reserve(nodes->items.size());
  for (const auto& node : nodes->items) page.items.push_back(summarize_node(node));
  return page;
}

Result<Node> Application::get_node(const Id& id) const { return store_->get_node(id); }

Result<NodeSummary> Application::copy_node(const Id& id, std::string new_name) {
  if (new_name.empty()) return std::unexpected(invalid("节点名称不能为空", "name"));
  auto original = store_->get_node(id);
  if (!original) return std::unexpected(original.error());
  original->id = new_id();
  original->source_id.reset();
  original->upstream_id.reset();
  original->source_missing = false;
  original->original_name = std::move(new_name);
  original->alias.reset();
  original->labels.clear();
  original->enabled = true;
  if (auto saved = store_->put_node(*original); !saved) return std::unexpected(saved.error());
  return summarize_node(*original);
}

Result<NodeSummary> Application::set_node_enabled(const Id& id, bool enabled) {
  auto node = store_->get_node(id);
  if (!node) return std::unexpected(node.error());
  node->enabled = enabled;
  if (auto saved = store_->put_node(*node); !saved) return std::unexpected(saved.error());
  return summarize_node(*node);
}

Result<NodeSummary> Application::set_node_alias(const Id& id, std::optional<std::string> alias) {
  auto node = store_->get_node(id);
  if (!node) return std::unexpected(node.error());
  node->alias = std::move(alias);
  if (auto saved = store_->put_node(*node); !saved) return std::unexpected(saved.error());
  return summarize_node(*node);
}

Result<NodeSummary> Application::set_node_labels(const Id& id, std::vector<std::string> labels) {
  auto node = store_->get_node(id);
  if (!node) return std::unexpected(node.error());
  node->labels = std::move(labels);
  if (auto saved = store_->put_node(*node); !saved) return std::unexpected(saved.error());
  return summarize_node(*node);
}

Store& Application::store() { return *store_; }
SubscriptionService& Application::subscriptions() { return *subscriptions_; }

Result<std::vector<CoreInstall>> Application::list_cores() const {
  return store_->cores();
}

void Application::register_core(std::filesystem::path path, std::string name,
                                Completion<CoreInstall> completion) {
  if (path.empty() || name.empty()) {
    completion(std::unexpected(invalid("核心路径和名称不能为空", "path")));
    return;
  }
  if (!path.is_absolute() || ::access(path.c_str(), X_OK) != 0) {
    completion(std::unexpected(invalid("核心必须是可执行文件的绝对路径", "path")));
    return;
  }
  const auto id = new_id();
  auto* store = store_;
  const auto core_name = std::move(name);
  const auto core_path = std::move(path);
  auto accepted = processes_->execute(
      version_command(core_path), Milliseconds{5000},
      [completion, store, id, core_name, core_path](
          Result<ProcessResult> process_result) mutable {
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
  if (!accepted) completion(std::unexpected(accepted.error()));
}

ProfileService& Application::profiles() { return *profiles_; }
Operations& Application::operations() { return operations_; }
CoreManager& Application::core_manager() { return *core_manager_; }
LogBuffer& Application::logs() { return logs_; }

Result<BuildInput> Application::build_input(const Id& profile_id) {
  auto profile = profiles_->snapshot(profile_id);
  if (!profile) return std::unexpected(profile.error());
  auto groups = profiles_->groups(profile_id);
  if (!groups) return std::unexpected(groups.error());
  auto observation = profiles_->observation(profile_id);
  if (!observation) return std::unexpected(observation.error());
  profile->bindings.groups = std::move(*groups);
  profile->bindings.observation = std::move(*observation);
  auto nodes = store_->all_nodes();
  if (!nodes) return std::unexpected(nodes.error());
  return BuildInput{std::move(*profile), std::move(*nodes)};
}

Result<void> Application::select(const Id& profile_id, std::optional<Target> target) {
  auto profile = profiles_->get(profile_id);
  if (!profile) return std::unexpected(profile.error());
  if (target && target->kind == TargetKind::node) {
    auto node = store_->get_node(target->id_or_tag);
    if (!node) return std::unexpected(node.error());
    if (!node->enabled || node->source_missing)
      return std::unexpected(AppError{ErrorCode::not_found, "节点已停用或来源缺失",
                                      target->id_or_tag, std::nullopt});
  } else if (target && target->kind == TargetKind::group) {
    auto groups = profiles_->groups(profile_id);
    if (!groups) return std::unexpected(groups.error());
    if (std::none_of(groups->begin(), groups->end(), [&](const GroupBinding& group) {
          return group.id == target->id_or_tag;
        }))
      return std::unexpected(AppError{ErrorCode::not_found, "找不到策略组", target->id_or_tag, std::nullopt});
  }
  auto profile_path = store_->profile_path(profile_id);
  if (!profile_path) return std::unexpected(profile_path.error());
  auto current = read_text(*profile_path / "profile.json", 8 * 1024 * 1024);
  if (!current) return std::unexpected(current.error());
  profile->selected = std::move(target);
  return profiles_->save(profile_id, ProfileEdit{{*current}, std::move(*profile)});
}

void Application::connect(const Id& profile_id, std::optional<Target> target,
                          Completion<ActivationResult> completion) {
  if (target) {
    auto selected = select(profile_id, std::move(target));
    if (!selected) { completion(std::unexpected(selected.error())); return; }
  }
  auto input = build_input(profile_id);
  if (!input) { completion(std::unexpected(input.error())); return; }
  auto core = store_->get_core(input->settings.profile.core_id);
  if (!core) { completion(std::unexpected(core.error())); return; }
  auto accepted = core_manager_->activate(
      ActivationRequest{ActivationInput{BuildRequest{std::move(*input), std::move(*core)}},
                        ActivationMode::start_or_replace}, completion);
  if (!accepted) completion(std::unexpected(accepted.error()));
}

void Application::check(const Id& profile_id, Completion<ActivationResult> completion) {
  auto input = build_input(profile_id);
  if (!input) { completion(std::unexpected(input.error())); return; }
  auto core = store_->get_core(input->settings.profile.core_id);
  if (!core) { completion(std::unexpected(core.error())); return; }
  auto accepted = core_manager_->activate(
      ActivationRequest{ActivationInput{BuildRequest{std::move(*input), std::move(*core)}},
                        ActivationMode::check_only}, completion);
  if (!accepted) completion(std::unexpected(accepted.error()));
}

void Application::dump(const Id& profile_id, Completion<Json> completion) {
  auto input = build_input(profile_id);
  if (!input) { completion(std::unexpected(input.error())); return; }
  auto core = store_->get_core(input->settings.profile.core_id);
  if (!core) { completion(std::unexpected(core.error())); return; }
  auto files = assemble(*input);
  if (!files) { completion(std::unexpected(files.error())); return; }
  const auto run_id = "dump-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto directory = runtime_writer_->materialize(*files, DirectoryPurpose::preview, run_id);
  if (!directory) { completion(std::unexpected(directory.error())); return; }
  const auto& environment = input->settings.profile.environment;
  RunSpec spec{core->path, directory->confdir,
               environment.cwd.empty() ? std::filesystem::current_path() : environment.cwd,
               environment.process_environment};
  auto accepted = processes_->execute(dump_command(spec), Milliseconds{30000},
      [completion, confdir = directory->confdir](Result<ProcessResult> result) mutable {
        if (!result) { completion(std::unexpected(result.error())); return; }
        if (result->exit.exit_code != 0 || result->exit.term_signal != 0) {
          completion(std::unexpected(AppError{ErrorCode::core_check_failed,
              "Xray dump失败", result->stderr_text, std::nullopt}));
          return;
        }
        completion(Json{{"confdir", confdir.string()}, {"stdout", result->stdout_text},
                        {"stderr", result->stderr_text}});
      });
  if (!accepted) completion(std::unexpected(accepted.error()));
}

Result<PreparedDirectory> Application::render(const Id& profile_id) {
  auto input = build_input(profile_id);
  if (!input) return std::unexpected(input.error());
  auto files = assemble(*input);
  if (!files) return std::unexpected(files.error());
  const auto id = "preview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  return runtime_writer_->materialize(*files, DirectoryPurpose::preview, id);
}

Result<void> Application::export_config(const Id& profile_id, std::string_view source,
                                        const std::filesystem::path& destination) {
  if (source == "runtime") {
    const auto status = core_manager_->status();
    if (!status.active) return std::unexpected(AppError{ErrorCode::not_found,
        "当前没有可导出的运行配置", std::nullopt, std::nullopt});
    return runtime_writer_->export_files(status.active->spec.confdir, destination);
  }
  if (source != "saved")
    return std::unexpected(AppError{ErrorCode::invalid_argument, "source必须为saved或runtime",
                                    std::string(source), std::nullopt});
  auto preview = render(profile_id);
  if (!preview) return std::unexpected(preview.error());
  return runtime_writer_->export_files(preview->confdir, destination);
}

namespace {
const char* core_state_name(CoreState state) {
  switch (state) {
    case CoreState::stopped: return "stopped";
    case CoreState::starting: return "starting";
    case CoreState::running: return "running";
    case CoreState::stopping: return "stopping";
    case CoreState::failed: return "failed";
  }
  return "failed";
}
const char* health_name(RuntimeHealth health) {
  switch (health) {
    case RuntimeHealth::process_only: return "process_only";
    case RuntimeHealth::control_ready: return "control_ready";
    case RuntimeHealth::control_unavailable: return "control_unavailable";
  }
  return "process_only";
}
}

Result<Json> Application::runtime_status() const {
  const auto status = core_manager_->status();
  const bool override_current = status.active && override_run_id_ == status.active->run_id;
  auto target_json = [](const std::optional<Target>& target) -> Json {
    if (!target) return nullptr;
    const char* kind = target->kind == TargetKind::node ? "node"
        : target->kind == TargetKind::group ? "group"
        : target->kind == TargetKind::native_outbound ? "native_outbound" : "native_balancer";
    return Json{{"kind", kind}, {"id_or_tag", target->id_or_tag}};
  };
  Json active = nullptr;
  if (status.active) {
    const auto core = store_->get_core(status.active->core_id);
    active = Json{{"run_id", status.active->run_id}, {"profile_id", status.active->profile_id},
                  {"core_id", status.active->core_id}, {"confdir", status.active->spec.confdir.string()},
                  {"core_version", core ? core->reported_version : "unknown"},
                  {"selection", target_json(status.active->applied_selection)}};
  }
  Json saved_target = nullptr;
  if (status.active) {
    auto profile = profiles_->get(status.active->profile_id);
    if (profile) saved_target = target_json(profile->selected);
  }
  Json error = nullptr;
  if (status.last_error) error = error_to_json(*status.last_error);
  return Json{{"state", core_state_name(status.state)}, {"desired_running", status.desired_running},
              {"pid", status.pid ? Json(*status.pid) : Json(nullptr)}, {"health", health_name(status.health)},
              {"active", std::move(active)}, {"saved_target", std::move(saved_target)},
              {"actual_target", status.active ? target_json(status.active->applied_selection) : Json(nullptr)},
              {"override_state", override_current
                  ? (override_state_ == OverrideState::confirmed ? "confirmed"
                      : override_state_ == OverrideState::unknown ? "unknown" : "none") : "none"},
              {"runtime_override", override_current && runtime_override_ ? Json(*runtime_override_) : Json(nullptr)},
              {"error", std::move(error)}};
}

Result<ModuleChangePlan> Application::plan_module_changes(const Id& profile_id,
    std::string_view action, const Json& arguments) const {
  if (action != "enable_traffic_statistics" && action != "disable_traffic_statistics")
    return std::unexpected(invalid("不支持的跨模块模块动作", std::string(action)));
  const auto enabled = action == "enable_traffic_statistics";
  const auto listen = arguments.value("metrics_listen", std::string{});
  auto snapshot = profiles_->snapshot(profile_id);
  if (!snapshot) return std::unexpected(snapshot.error());
  return plan_traffic_stats(*snapshot, enabled, listen);
}

SaveManyResult Application::save_module_changes(const Id& profile_id, const ModuleChangePlan& plan) {
  return xrm::save_module_changes(*profiles_, profile_id, plan);
}

Json Application::runtime_metrics(const Id& connection_id, bool watch) {
  if (watch) metrics_viewers_.insert(connection_id);
  else metrics_viewers_.erase(connection_id);
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
    for (const auto& [key, filename] : std::array<std::pair<std::string_view, std::string_view>, 3>{
             std::pair{"stats", "08_stats.json"}, {"policy", "09_policy.json"}, {"metrics", "07_metrics.json"}}) {
      if (!std::filesystem::exists(directory / filename)) missing.push_back(key);
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
          const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
          const bool loopback = host == "127.0.0.1" || host == "localhost" || host == "[::1]";
          if (loopback && parsed.ec == std::errc{} && parsed.ptr == port_text.data() + port_text.size() &&
              port > 0 && port <= 65535)
            endpoint = "http://" + listen + "/debug/vars";
        }
      }
    } catch (const std::exception&) {
      missing.push_back("metrics endpoint unavailable");
    }
  }
  if (record) {
    if (metrics_run_id_ != record->run_id) {
      metrics_run_id_ = record->run_id;
      metrics_client_->attach(*record, endpoint);
    }
    if (!endpoint && std::find(missing.begin(), missing.end(), Json("metrics")) == missing.end())
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
    for (const auto& value : view.sample->traffic)
      counters.push_back(Json{{"scope", value.scope}, {"tag", value.tag},
          {"uplink", value.uplink ? Json(*value.uplink) : Json(nullptr)},
          {"downlink", value.downlink ? Json(*value.downlink) : Json(nullptr)}});
    Json observations = Json::array();
    for (const auto& value : view.sample->observations)
      observations.push_back(Json{{"tag", value.tag},
          {"alive", value.alive ? Json(*value.alive) : Json(nullptr)},
          {"latency_ms", value.latency_ms ? Json(*value.latency_ms) : Json(nullptr)}});
    result["sample"] = Json{{"run_id", view.sample->run_id}, {"received_at", view.sample->received_at},
        {"traffic", std::move(counters)}, {"observations", std::move(observations)}};
  }
  return result;
}

void Application::release_metrics_viewer(const Id& connection_id) {
  metrics_viewers_.erase(connection_id);
  metrics_client_->set_viewer_count(metrics_viewers_.size());
}

Result<ControlTarget> Application::control_target() const {
  const auto status = core_manager_->status();
  if (!status.active || status.state != CoreState::running)
    return std::unexpected(AppError{ErrorCode::not_found, "没有正在运行的核心", std::nullopt, std::nullopt});
  const auto api_path = status.active->spec.confdir / "06_api.json";
  const auto inbounds_path = status.active->spec.confdir / "02_inbounds.json";
  auto api_text = read_text(api_path, 1024 * 1024);
  auto inbounds_text = read_text(inbounds_path, 8 * 1024 * 1024);
  if (!api_text || !inbounds_text)
    return std::unexpected(AppError{ErrorCode::unsupported, "当前运行配置没有可用的 Xray API 模块/入站",
        !api_text ? api_path.string() : inbounds_path.string(), std::nullopt});
  try {
    const auto api_root = Json::parse(*api_text).at("api");
    const auto tag = api_root.at("tag").get<std::string>();
    const auto inbounds_root = Json::parse(*inbounds_text).at("inbounds");
    if (!inbounds_root.is_array()) throw std::invalid_argument("inbounds is not an array");
    for (const auto& inbound : inbounds_root) {
      if (inbound.value("tag", "") != tag) continue;
      const auto listen = inbound.value("listen", "");
      const auto port = inbound.at("port").get<std::uint16_t>();
      if (listen != "127.0.0.1" && listen != "::1" && listen != "localhost")
        return std::unexpected(AppError{ErrorCode::unsupported,
            "运行中的 Xray API 入站不是 loopback，拒绝使用控制端点", listen, std::nullopt});
      return ControlTarget{status.active->spec, listen + ":" + std::to_string(port), Milliseconds{3000}};
    }
    return std::unexpected(AppError{ErrorCode::unsupported, "找不到与 API tag 对应的 loopback 入站", tag, std::nullopt});
  } catch (const std::exception& error) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
        "无法读取运行 Xray API 配置: " + std::string(error.what()), api_path.string(), std::nullopt});
  }
}

Result<Id> Application::override_runtime_group(std::string balancer, std::string outbound,
                                               Completion<OverrideResult> completion) {
  if (balancer.empty() || outbound.empty())
    return std::unexpected(invalid("需要 balancer 与 outbound tag", "group.override"));
  auto target = control_target();
  if (!target) return std::unexpected(target.error());
  const auto run_id = core_manager_->status().active->run_id;
  return xray_control_->override_outbound(std::move(*target), std::move(balancer), std::move(outbound),
      [this, run_id, completion = std::move(completion)](Result<OverrideResult> response) mutable {
        if (!response) { completion(std::unexpected(response.error())); return; }
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

Result<Id> Application::clear_runtime_group_override(std::string balancer,
                                                     Completion<OverrideResult> completion) {
  auto target = control_target();
  if (!target) return std::unexpected(target.error());
  const auto run_id = core_manager_->status().active->run_id;
  return xray_control_->clear_override(std::move(*target), std::move(balancer),
      [this, run_id, completion = std::move(completion)](Result<OverrideResult> response) mutable {
        if (!response) { completion(std::unexpected(response.error())); return; }
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

Result<Json> Application::show_config(const Id& profile_id, std::string_view source,
                                      std::optional<std::string> module) const {
  if (source == "source") {
    if (!module) return std::unexpected(AppError{ErrorCode::invalid_argument,
        "读取source时必须指定module", std::nullopt, std::nullopt});
    auto view = profiles_->module(profile_id, *module);
    if (!view) return std::unexpected(view.error());
    return Json{{"source_file", (std::filesystem::path("modules") / view->setting.filename).generic_string()},
                {"runtime_file", view->setting.filename.generic_string()},
                {"text", view->document ? Json(view->document->source_text) : Json(nullptr)}};
  }
  if (source == "runtime") {
    const auto status = core_manager_->status();
    if (!status.active) return std::unexpected(AppError{ErrorCode::not_found,
        "当前没有运行配置", std::nullopt, std::nullopt});
    Json files = Json::array();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(status.active->spec.confdir, ec), end;
         it != end && !ec; it.increment(ec)) {
      if (!module || it->path().filename().string().find(*module) != std::string::npos)
        files.push_back(Json{{"file", it->path().filename().string()},
                             {"text", read_text(it->path(), 8 * 1024 * 1024).value_or("")}});
    }
    if (ec) return std::unexpected(AppError{ErrorCode::io_error, "无法读取运行目录", ec.message(), std::nullopt});
    return Json{{"runtime_dir", status.active->spec.confdir.string()}, {"files", std::move(files)}};
  }
  return std::unexpected(AppError{ErrorCode::invalid_argument, "source必须为source或runtime",
                                  std::string(source), std::nullopt});
}

void Application::recover(Completion<ActivationResult> completion) {
  auto result = core_manager_->restore_after_server_start(std::move(completion));
  if (!result) return;
}

void Application::shutdown(std::function<void()> drained) {
  subscriptions_->stop_schedule();
  metrics_client_->shutdown();
  core_manager_->shutdown(std::move(drained));
}

}  // namespace xrm
