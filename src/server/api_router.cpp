#include "server/api_router.hpp"

#include "contract/methods.hpp"
#include "infra/files.hpp"

#include <limits>
#include <array>
#include <random>

namespace xrm {
namespace {

AppError bad_params(std::string message, std::string subject = "params") {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject), std::nullopt};
}

bool read_string(const Json& params, std::string_view key, std::string& value) {
  if (!params.contains(key) || !params.at(key).is_string()) return false;
  value = params.at(key).get<std::string>();
  return !value.empty();
}

bool read_size(const Json& params, std::string_view key, std::size_t fallback,
               std::size_t& value) {
  if (!params.contains(key)) { value = fallback; return true; }
  const auto& field = params.at(key);
  if (!field.is_number_unsigned() && !field.is_number_integer()) return false;
  try {
    if (field.get<std::int64_t>() < 0) return false;
    const auto parsed = field.get<std::uint64_t>();
    if (parsed > std::numeric_limits<std::size_t>::max()) return false;
    value = static_cast<std::size_t>(parsed);
    return true;
  } catch (const Json::exception&) {
    return false;
  }
}

Json page_to_json(const Page<NodeSummary>& page) {
  Json items = Json::array();
  for (const auto& item : page.items) items.push_back(item);
  return Json{{"items", std::move(items)}, {"total", page.total},
              {"next_offset", page.next_offset ? Json(*page.next_offset) : Json(nullptr)}};
}

Json refresh_result_json(const BatchRefreshResult& result) {
  Json completed = Json::array();
  for (const auto& report : result.completed)
    completed.push_back(Json{{"source_id", report.source_id}, {"added", report.added},
        {"updated", report.updated}, {"missing", report.missing},
        {"malformed", report.malformed}, {"unsupported", report.unsupported},
        {"partial", report.partial}});
  Json failed = Json::array();
  for (const auto& [id, error] : result.failed)
    failed.push_back(Json{{"source_id", id}, {"error", error_to_json(error)}});
  return Json{{"completed", std::move(completed)}, {"failed", std::move(failed)}};
}

Result<SubscriptionSummary> subscription_summary(Store& store, const Subscription& source) {
  auto state = store.get_source_state(source.id);
  if (!state) return std::unexpected(state.error());
  auto nodes = store.list_nodes(NodeQuery{.source_id = source.id}, {0, 1});
  if (!nodes) return std::unexpected(nodes.error());
  return SubscriptionSummary{source.id, source.name, source.auto_refresh,
      state->last_result, state->last_attempt, state->last_success, nodes->total,
      state->last_unsupported_count, state->last_malformed_count};
}

Id new_source_id() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char digits[] = "0123456789abcdef";
  Id id;
  id.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) id.push_back('-');
    id.push_back(digits[bytes[index] >> 4]);
    id.push_back(digits[bytes[index] & 0x0f]);
  }
  return id;
}

Json module_view_json(const ModuleView& view) {
  return Json{{"key", view.descriptor.key}, {"label", view.descriptor.label},
      {"source_file", (std::filesystem::path("modules") / view.setting.filename).generic_string()},
      {"runtime_file", view.setting.filename.generic_string()}, {"included", view.setting.included},
      {"text", view.document ? Json(view.document->source_text) : Json(nullptr)},
      {"editor_support", view.editor_support}};
}

Json module_change_plan_json(const ModuleChangePlan& plan) {
  Json changes = Json::array();
  for (const auto& change : plan.changes) {
    changes.push_back(Json{{"key", change.edit.key}, {"expected_text", change.edit.base.expected_text},
        {"text", change.edit.text}, {"include_after_save", change.include_after_save}});
  }
  Json notices = Json::array();
  for (const auto& notice : plan.notices)
    notices.push_back(Json{{"code", notice.code}, {"message", notice.message}});
  return Json{{"changes", std::move(changes)}, {"notices", std::move(notices)}};
}

Result<ModuleChangePlan> parse_module_change_plan(const Json& value) {
  if (!value.is_object() || !value.contains("changes") || !value.at("changes").is_array())
    return std::unexpected(bad_params("plan.changes必须是数组", "plan"));
  ModuleChangePlan plan;
  for (const auto& item : value.at("changes")) {
    if (!item.is_object() || !item.contains("key") || !item.at("key").is_string() ||
        !item.contains("text") || !item.at("text").is_string())
      return std::unexpected(bad_params("plan change需要key与text", "plan.changes"));
    EditBase base;
    if (item.contains("expected_text") && !item.at("expected_text").is_null()) {
      if (!item.at("expected_text").is_string())
        return std::unexpected(bad_params("expected_text必须是字符串或null", "plan.changes.expected_text"));
      base.expected_text = item.at("expected_text").get<std::string>();
    }
    const bool include = item.value("include_after_save", true);
    plan.changes.push_back({ModuleEdit{item.at("key").get<std::string>(), std::move(base),
        item.at("text").get<std::string>()}, include});
  }
  return plan;
}

Json save_many_result_json(const SaveManyResult& result) {
  Json value{{"saved", result.saved}, {"changed_modules", result.saved}};
  value["failure"] = result.failure ? error_to_json(*result.failure) : Json(nullptr);
  return value;
}

Result<std::optional<Target>> read_target(const Json& params) {
  if (!params.contains("target") || params.at("target").is_null())
    return std::optional<Target>{};
  const auto& value = params.at("target");
  if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string() ||
      !value.contains("id_or_tag") || !value.at("id_or_tag").is_string())
    return std::unexpected(bad_params("target需要kind与id_or_tag", "target"));
  const auto kind = value.at("kind").get<std::string>();
  TargetKind target_kind;
  if (kind == "node") target_kind = TargetKind::node;
  else if (kind == "group") target_kind = TargetKind::group;
  else if (kind == "native_outbound") target_kind = TargetKind::native_outbound;
  else if (kind == "native_balancer") target_kind = TargetKind::native_balancer;
  else return std::unexpected(bad_params("target.kind无效", "target.kind"));
  return std::optional<Target>(Target{target_kind, value.at("id_or_tag").get<std::string>()});
}

Json activation_json(const ActivationResult& result) {
  const char* effect = result.effect == ActivationEffect::checked_only ? "checked_only"
      : result.effect == ActivationEffect::running_new ? "running_new"
      : result.effect == ActivationEffect::restored_old ? "restored_old"
      : result.effect == ActivationEffect::stopped ? "stopped" : "unknown";
  Json active = nullptr;
  if (result.active)
    active = Json{{"run_id", result.active->run_id}, {"profile_id", result.active->profile_id},
                  {"confdir", result.active->spec.confdir.string()}};
  Json error = nullptr;
  if (result.attempt_error) error = error_to_json(*result.attempt_error);
  return Json{{"effect", effect}, {"active", std::move(active)}, {"attempt_error", std::move(error)}};
}

std::uint64_t log_cursor(const Json& params) {
  if (!params.contains("cursor") || !params.at("cursor").is_number_unsigned()) return 0;
  return params.at("cursor").get<std::uint64_t>();
}

Json log_stream_name(LogStream stream) {
  switch (stream) {
    case LogStream::server: return "server";
    case LogStream::core_stdout: return "core_stdout";
    case LogStream::core_stderr: return "core_stderr";
  }
  return "server";
}

}  // namespace

ApiRouter::ApiRouter(Application& application) : application_(&application) {}

void ApiRouter::dispatch(const Id& connection_id, RpcRequest request, RpcReply reply) {
  const auto id = request.id;
  const auto method = request.method;
  const auto params = std::move(request.params);
  auto fail = [&](AppError error) {
    reply(RpcResponse{1, id, false, std::nullopt, std::move(error)});
  };
  auto succeed = [&](Json value) {
    reply(RpcResponse{1, id, true, std::move(value), std::nullopt});
  };

  if (method == methods::server_info) {
    succeed(application_->server_info());
    return;
  }
  if (method == "settings.get") {
    auto settings = application_->store().load_program_settings();
    if (!settings) fail(settings.error()); else succeed(std::move(*settings));
    return;
  }
  if (method == "settings.update") {
    if (!params.is_object() || !params.contains("patch")) {
      fail(bad_params("settings.update需要patch对象", "patch")); return;
    }
    auto settings = application_->store().update_program_settings(params.at("patch"));
    if (!settings) fail(settings.error()); else succeed(std::move(*settings));
    return;
  }
  if (method == methods::node_add) {
    std::string name;
    const bool has_uri = params.contains("uri");
    const bool has_settings = params.contains("settings");
    const bool has_outbound = params.contains("outbound");
    if (static_cast<int>(has_uri) + static_cast<int>(has_settings) +
        static_cast<int>(has_outbound) != 1) {
      fail(bad_params("node.add需要uri、settings或outbound之一")); return;
    }
    Result<NodeSummary> added;
    if (has_uri) {
      std::string uri;
      if (!read_string(params, "uri", uri)) { fail(bad_params("node.add需要非空uri", "uri")); return; }
      std::optional<std::string> override_name;
      if (params.contains("name")) {
        if (!read_string(params, "name", name)) { fail(bad_params("name必须是非空字符串", "name")); return; }
        override_name = std::move(name);
      }
      added = application_->add_manual_share_uri(std::move(uri), std::move(override_name));
    } else {
      if (!read_string(params, "name", name)) { fail(bad_params("node.add需要非空name", "name")); return; }
      if (has_settings) {
        if (!params.at("settings").is_object()) { fail(bad_params("settings必须是对象", "settings")); return; }
        added = application_->add_manual_typed_node(std::move(name), params.at("settings"));
      } else {
        if (!params.at("outbound").is_object()) { fail(bad_params("outbound必须是对象", "outbound")); return; }
        added = application_->add_manual_node(std::move(name), params.at("outbound"));
      }
    }
    if (!added) fail(added.error());
    else succeed(Json(*added));
    return;
  }
  if (method == methods::node_update) {
    std::string id, name;
    if (!read_string(params, "id", id) || !read_string(params, "name", name) ||
        !params.contains("settings") || !params.at("settings").is_object()) {
      fail(bad_params("node.update需要id、name和settings对象")); return;
    }
    auto changed = application_->replace_manual_node(id, std::move(name), params.at("settings"));
    if (!changed) fail(changed.error()); else succeed(Json(*changed));
    return;
  }
  if (method == methods::node_remove) {
    std::string id;
    if (!read_string(params, "id", id)) { fail(bad_params("node.remove需要id")); return; }
    auto removed = application_->remove_manual_node(id);
    if (!removed) fail(removed.error()); else succeed(Json{{"removed", true}});
    return;
  }
  if (method == methods::node_list) {
    NodeQuery query;
    if (params.contains("text")) {
      if (!params.at("text").is_string()) {
        fail(bad_params("text必须是字符串", "text"));
        return;
      }
      query.text = params.at("text").get<std::string>();
    }
    std::size_t offset = 0;
    std::size_t limit = 100;
    if (!read_size(params, "offset", 0, offset) || !read_size(params, "limit", 100, limit)) {
      fail(bad_params("offset和limit必须是非负整数", "page"));
      return;
    }
    query.include_missing = true;
    auto page = application_->list_nodes(query, {offset, limit});
    if (!page) fail(page.error());
    else succeed(page_to_json(*page));
    return;
  }
  if (method == methods::node_get) {
    std::string node_id;
    if (!read_string(params, "id", node_id)) { fail(bad_params("node.get需要id")); return; }
    auto node = application_->get_node(node_id);
    if (!node) fail(node.error()); else succeed(Json(*node));
    return;
  }
  if (method == methods::node_copy) {
    std::string node_id, name;
    if (!read_string(params, "id", node_id) || !read_string(params, "name", name)) {
      fail(bad_params("node.copy需要id和name")); return;
    }
    auto copied = application_->copy_node(node_id, std::move(name));
    if (!copied) fail(copied.error()); else succeed(Json(*copied));
    return;
  }
  if (method == methods::node_set_enabled) {
    std::string node_id;
    if (!read_string(params, "id", node_id) || !params.contains("enabled") || !params["enabled"].is_boolean()) {
      fail(bad_params("node.set_enabled需要id和boolean enabled")); return;
    }
    auto changed = application_->set_node_enabled(node_id, params["enabled"].get<bool>());
    if (!changed) fail(changed.error()); else succeed(Json(*changed));
    return;
  }
  if (method == methods::node_set_alias) {
    std::string node_id;
    if (!read_string(params, "id", node_id) || !params.contains("alias") ||
        (!params["alias"].is_null() && !params["alias"].is_string())) {
      fail(bad_params("node.set_alias需要id和string/null alias")); return;
    }
    std::optional<std::string> alias;
    if (params["alias"].is_string()) alias = params["alias"].get<std::string>();
    auto changed = application_->set_node_alias(node_id, std::move(alias));
    if (!changed) fail(changed.error()); else succeed(Json(*changed));
    return;
  }
  if (method == methods::node_set_labels) {
    std::string node_id;
    if (!read_string(params, "id", node_id) || !params.contains("labels") || !params["labels"].is_array()) {
      fail(bad_params("node.set_labels需要id和labels数组")); return;
    }
    try {
      auto changed = application_->set_node_labels(node_id, params["labels"].get<std::vector<std::string>>());
      if (!changed) fail(changed.error()); else succeed(Json(*changed));
    } catch (const Json::exception& error) { fail(bad_params(error.what(), "labels")); }
    return;
  }
  if (method == methods::node_form_edit) {
    NodeFormEditDto edit;
    try { edit = params.get<NodeFormEditDto>(); }
    catch (const Json::exception& error) { fail(bad_params(error.what())); return; }
    if (edit.id.empty() || edit.field.empty()) { fail(bad_params("node.form_edit需要id与field")); return; }
    auto changed = application_->edit_node_form(edit.id, edit.field, std::move(edit.value));
    if (!changed) fail(changed.error()); else succeed(Json(*changed));
    return;
  }
  if (method == methods::node_history_preview) {
    std::string source_id;
    if (!read_string(params, "source_id", source_id)) { fail(bad_params("source_id required")); return; }
    auto result = application_->preview_node_history(source_id);
    if (!result) fail(result.error()); else succeed(std::move(*result));
    return;
  }
  if (method == methods::node_history_confirm) {
    std::string source_id, token;
    if (!read_string(params, "source_id", source_id) || !read_string(params, "token", token) ||
        !params.contains("ids") || !params.at("ids").is_array()) {
      fail(bad_params("source_id, token and ids required")); return;
    }
    Json selected{{"token", token}, {"ids", params.at("ids")}};
    auto result = application_->confirm_node_history(source_id, selected);
    if (!result) fail(result.error()); else succeed(std::move(*result));
    return;
  }
  if (method == methods::subscription_list) {
    auto sources = application_->store().list_sources();
    if (!sources) { fail(sources.error()); return; }
    Json values = Json::array();
    for (const auto& source : *sources) {
      auto summary = subscription_summary(application_->store(), source);
      if (!summary) { fail(summary.error()); return; }
      values.push_back(*summary);
    }
    succeed(std::move(values)); return;
  }
  if (method == methods::subscription_get) {
    std::string source_id;
    if (!read_string(params, "id", source_id)) { fail(bad_params("subscription.get需要id")); return; }
    auto source = application_->store().get_source(source_id);
    if (!source) { fail(source.error()); return; }
    auto state = application_->store().get_source_state(source_id);
    if (!state) { fail(state.error()); return; }
    succeed(Json{{"subscription", Json(*source)}, {"state", Json(*state)}}); return;
  }
  if (method == methods::subscription_add || method == methods::subscription_replace) {
    std::string name, url;
    if (method == methods::subscription_add &&
        (!read_string(params, "name", name) || !read_string(params, "url", url))) {
      fail(bad_params("subscription.add需要name和url")); return;
    }
    if (method == methods::subscription_replace && params.size() <= 1) {
      fail(bad_params("subscription.replace需要至少一个变更字段")); return;
    }
    Subscription source;
    if (method == methods::subscription_replace) {
      std::string source_id;
      if (!read_string(params, "id", source_id)) { fail(bad_params("subscription.replace需要id")); return; }
      auto old = application_->store().get_source(source_id);
      if (!old) { fail(old.error()); return; }
      source = std::move(*old);
      ++source.settings_revision;
      name = source.name;
      url = source.url;
    } else source.id = new_source_id();
    if (params.contains("name")) {
      if (!params["name"].is_string() || params["name"].get<std::string>().empty()) {
        fail(bad_params("name必须为非空字符串")); return;
      }
      name = params["name"].get<std::string>();
    }
    if (params.contains("url")) {
      if (!params["url"].is_string() || params["url"].get<std::string>().empty()) {
        fail(bad_params("url必须为非空字符串")); return;
      }
      url = params["url"].get<std::string>();
    }
    source.name = std::move(name);
    source.url = std::move(url);
    if (params.contains("user_agent")) {
      if (!params["user_agent"].is_string()) { fail(bad_params("user_agent必须是字符串")); return; }
      source.user_agent = params["user_agent"].get<std::string>();
    }
    if (params.contains("headers")) {
      if (!params["headers"].is_object()) { fail(bad_params("headers必须是对象")); return; }
      try { source.headers = params["headers"].get<std::map<std::string, std::string>>(); }
      catch (const Json::exception& error) { fail(bad_params(error.what(), "headers")); return; }
    }
    if (params.contains("auto_refresh")) {
      if (!params["auto_refresh"].is_boolean()) { fail(bad_params("auto_refresh必须是布尔值")); return; }
      source.auto_refresh = params["auto_refresh"].get<bool>();
    }
    if (params.contains("interval_seconds")) {
      if (!params["interval_seconds"].is_number_integer() || params["interval_seconds"].get<std::int64_t>() <= 0) {
        fail(bad_params("interval_seconds必须为正整数")); return;
      }
      source.interval = std::chrono::seconds(params["interval_seconds"].get<std::int64_t>());
    }
    if (params.contains("proxy_url")) {
      if (!params["proxy_url"].is_null() && !params["proxy_url"].is_string()) {
        fail(bad_params("proxy_url必须是字符串或null")); return;
      }
      if (params["proxy_url"].is_string()) source.route.proxy_url = params["proxy_url"].get<std::string>();
      else source.route.proxy_url.reset();
    }
    if (auto saved = application_->store().put_source(source); !saved) fail(saved.error());
    else succeed(Json{{"id", source.id}});
    return;
  }
  if (method == methods::subscription_remove) {
    std::string source_id;
    if (!read_string(params, "id", source_id)) { fail(bad_params("subscription.remove需要id")); return; }
    const bool keep_as_manual = params.value("keep_as_manual", false);
    auto removed = application_->store().remove_source(source_id, keep_as_manual);
    if (!removed) fail(removed.error()); else succeed(Json{{"removed", true}});
    return;
  }
  if (method == methods::subscription_refresh) {
    std::vector<Id> ids;
    if (params.contains("source_ids")) {
      if (!params["source_ids"].is_array()) { fail(bad_params("source_ids必须是数组")); return; }
      try { ids = params["source_ids"].get<std::vector<Id>>(); }
      catch (const Json::exception& error) { fail(bad_params(error.what(), "source_ids")); return; }
    }
    const auto operation = application_->operations().begin();
    auto accepted = application_->subscriptions().refresh(std::move(ids),
        [application = application_, operation](Result<BatchRefreshResult> result) {
          if (!result) application->operations().complete(operation, std::unexpected(result.error()));
          else application->operations().complete(operation, refresh_result_json(*result));
        });
    if (!accepted) {
      application_->operations().complete(operation, std::unexpected(accepted.error()));
      fail(accepted.error());
    } else succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::core_list) {
    auto cores = application_->list_cores();
    if (!cores) { fail(cores.error()); return; }
    Json result = Json::array();
    for (const auto& core : *cores) result.push_back(core);
    succeed(std::move(result));
    return;
  }
  if (method == methods::core_remove) {
    std::string core_id;
    if (!read_string(params, "core_id", core_id)) { fail(bad_params("core.remove需要core_id")); return; }
    auto removed = application_->remove_managed_core(core_id);
    if (!removed) { fail(removed.error()); return; }
    succeed(Json{{"removed", core_id}});
    return;
  }
  if (method == methods::core_releases) {
    const bool include_prerelease = params.value("include_prerelease", false);
    const auto operation = application_->operations().begin();
    auto accepted = application_->core_releases(include_prerelease,
        [application = application_, operation](Result<std::vector<CoreRelease>> result) {
          if (!result) { application->operations().complete(operation, std::unexpected(result.error())); return; }
          Json items = Json::array();
          for (const auto& release : *result) items.push_back(release);
          application->operations().complete(operation, Json{{"items", std::move(items)}});
        });
    if (!accepted) {
      application_->operations().complete(operation, std::unexpected(accepted.error()));
      fail(accepted.error());
    } else succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::core_install) {
    if (!params.contains("request")) { fail(bad_params("core.install需要request")); return; }
    InstallRequest request;
    try { request = params.at("request").get<InstallRequest>(); }
    catch (const Json::exception& error) { fail(bad_params(error.what(), "request")); return; }
    const auto operation = application_->operations().begin();
    auto accepted = application_->install_core(std::move(request),
        [application = application_, operation](Result<CoreInstall> result) {
          if (!result) application->operations().complete(operation, std::unexpected(result.error()));
          else application->operations().complete(operation, Json{{"install", *result}, {"status", "installed"}});
        });
    if (!accepted) {
      application_->operations().complete(operation, std::unexpected(accepted.error()));
      fail(accepted.error());
    } else succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::core_use) {
    CoreUseRequestDto request;
    try { request = params.get<CoreUseRequestDto>(); }
    catch (const Json::exception& error) { fail(bad_params(error.what())); return; }
    if (request.profile_id.empty() || request.core_id.empty()) {
      fail(bad_params("core.use需要profile_id和core_id")); return;
    }
    const auto operation = application_->operations().begin();
    application_->use_core(request.profile_id, request.core_id,
        [application = application_, operation](Result<ActivationResult> result) {
          if (!result) application->operations().complete(operation, std::unexpected(result.error()));
          else application->operations().complete(operation, activation_json(*result));
        });
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::core_register) {
    std::string path;
    std::string name;
    if (!read_string(params, "path", path) || !read_string(params, "name", name)) {
      fail(bad_params("core.register需要非空path和name", "path"));
      return;
    }
    application_->register_core(std::filesystem::path(std::move(path)), std::move(name),
                                [reply = std::move(reply), id](Result<CoreInstall> core) mutable {
      if (!core) {
        reply(RpcResponse{1, id, false, std::nullopt, core.error()});
      } else {
        reply(RpcResponse{1, id, true, Json(*core), std::nullopt});
      }
    });
    return;
  }
  if (method == methods::profile_create) {
    std::string name, core_id;
    if (!read_string(params, "name", name) ||
        (params.contains("core_id") && !params.at("core_id").is_string())) {
      fail(bad_params("profile.create需要name，core_id可暂时为空")); return;
    }
    core_id = params.value("core_id", "");
    RunEnvironment environment;
    if (params.contains("cwd") && params.at("cwd").is_string()) environment.cwd = params.at("cwd").get<std::string>();
    auto created = application_->profiles().create({name, core_id, params.value("preset", "local"), environment});
    if (!created) fail(created.error()); else succeed(Json(*created));
    return;
  }
  if (method == methods::profile_list) {
    auto profiles = application_->profiles().list();
    if (!profiles) { fail(profiles.error()); return; }
    Json values = Json::array();
    for (const auto& profile : *profiles)
      values.push_back(Json{{"id", profile.id}, {"name", profile.name}, {"core_id", profile.core_id}});
    succeed(std::move(values)); return;
  }
  if (method == methods::profile_get) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("profile.get需要profile_id")); return; }
    auto profile = application_->profiles().get(id);
    if (!profile) fail(profile.error()); else succeed(Json(*profile));
    return;
  }
  if (method == methods::profile_clone) {
    std::string id, name;
    if (!read_string(params, "profile_id", id) || !read_string(params, "name", name)) {
      fail(bad_params("profile.clone需要profile_id和name")); return;
    }
    auto cloned = application_->profiles().clone(id, name);
    if (!cloned) fail(cloned.error()); else succeed(Json(*cloned));
    return;
  }
  if (method == methods::profile_rename) {
    std::string id, name;
    if (!read_string(params, "profile_id", id) || !read_string(params, "name", name)) {
      fail(bad_params("profile.rename需要profile_id和name")); return;
    }
    auto renamed = application_->profiles().rename(id, std::move(name));
    if (!renamed) fail(renamed.error()); else succeed(Json{{"renamed", true}});
    return;
  }
  if (method == methods::profile_remove) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("profile.remove需要profile_id")); return; }
    auto removed = application_->profiles().remove(id);
    if (!removed) fail(removed.error()); else succeed(Json{{"removed", true}});
    return;
  }
  if (method == methods::profile_save) {
    std::string id;
    if (!read_string(params, "profile_id", id) || !params.contains("settings") || !params["settings"].is_object()) {
      fail(bad_params("profile.save需要profile_id与settings")); return;
    }
    EditBase base;
    if (params.contains("expected_text") && !params["expected_text"].is_null()) {
      if (!params["expected_text"].is_string()) { fail(bad_params("expected_text必须为字符串或null")); return; }
      base.expected_text = params["expected_text"].get<std::string>();
    }
    try {
      auto saved = application_->profiles().save(id, ProfileEdit{std::move(base), params["settings"].get<ProfileSettings>()});
      if (!saved) fail(saved.error()); else succeed(Json{{"saved", true}});
    } catch (const std::exception& error) { fail(bad_params(error.what(), "settings")); }
    return;
  }
  if (method == methods::profile_sources_set) {
    std::string id;
    if (!read_string(params, "profile_id", id) || !params.contains("source_ids") ||
        !params.at("source_ids").is_array()) {
      fail(bad_params("profile.sources.set需要profile_id和source_ids数组")); return;
    }
    try {
      auto changed = application_->set_profile_sources(id, params.at("source_ids").get<std::vector<Id>>());
      if (!changed) fail(changed.error());
      else succeed(Json{{"saved", true}, {"runtime_changed", false}});
    } catch (const Json::exception& error) { fail(bad_params(error.what(), "source_ids")); }
    return;
  }
  if (method == methods::module_list || method == methods::module_get) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("module操作需要profile_id")); return; }
    if (method == methods::module_get) {
      std::string key;
      if (!read_string(params, "module", key)) { fail(bad_params("module.get需要module")); return; }
      auto view = application_->profiles().module(id, key);
      if (!view) fail(view.error()); else succeed(module_view_json(*view));
    } else {
      auto views = application_->profiles().modules(id);
      if (!views) { fail(views.error()); return; }
      Json values = Json::array();
      for (const auto& view : *views) values.push_back(module_view_json(view));
      succeed(std::move(values));
    }
    return;
  }
  if (method == methods::module_save) {
    std::string id, key, text;
    if (!read_string(params, "profile_id", id) || !read_string(params, "module", key) ||
        !params.contains("text") || !params["text"].is_string()) {
      fail(bad_params("module.save需要profile_id、module与text")); return;
    }
    text = params["text"].get<std::string>();
    if (params.contains("expected_absent") && !params["expected_absent"].is_boolean()) {
      fail(bad_params("expected_absent必须为布尔值")); return;
    }
    const bool expected_absent = params.value("expected_absent", false);
    if (expected_absent && params.contains("expected_text") && !params["expected_text"].is_null()) {
      fail(bad_params("expected_absent不能与expected_text同时指定")); return;
    }
    EditBase base;
    if (params.contains("expected_text") && !params["expected_text"].is_null()) {
      if (!params["expected_text"].is_string()) { fail(bad_params("expected_text必须为字符串或null")); return; }
      base.expected_text = params["expected_text"].get<std::string>();
    } else if (!expected_absent) {
      auto current = application_->profiles().module(id, key);
      if (!current) { fail(current.error()); return; }
      if (current->document) base.expected_text = current->document->source_text;
    }
    auto saved = application_->profiles().save_module(id, ModuleEdit{key, std::move(base), std::move(text)});
    if (!saved) fail(saved.error()); else succeed(Json{{"status", "saved_only"}});
    return;
  }
  if (method == methods::module_change_plan) {
    std::string profile_id, action;
    if (!read_string(params, "profile_id", profile_id) || !read_string(params, "action", action)) {
      fail(bad_params("module.change_plan需要profile_id与action")); return;
    }
    const Json arguments = params.contains("arguments") ? params.at("arguments") : Json::object();
    if (!arguments.is_object()) { fail(bad_params("arguments必须是对象", "arguments")); return; }
    auto plan = application_->plan_module_changes(profile_id, action, arguments);
    if (!plan) fail(plan.error()); else succeed(module_change_plan_json(*plan));
    return;
  }
  if (method == methods::module_save_changes) {
    std::string profile_id;
    if (!read_string(params, "profile_id", profile_id) || !params.contains("plan")) {
      fail(bad_params("module.save_changes需要profile_id与plan")); return;
    }
    auto plan = parse_module_change_plan(params.at("plan"));
    if (!plan) { fail(plan.error()); return; }
    succeed(save_many_result_json(application_->save_module_changes(profile_id, *plan)));
    return;
  }
  if (method == methods::module_include) {
    std::string id, key;
    if (!read_string(params, "profile_id", id) || !read_string(params, "module", key) ||
        !params.contains("included") || !params["included"].is_boolean()) {
      fail(bad_params("module.include需要profile_id、module与included")); return;
    }
    auto changed = application_->profiles().include_module(id, key, params["included"].get<bool>());
    if (!changed) fail(changed.error()); else succeed(Json{{"saved_only", true}});
    return;
  }
  if (method == methods::module_rename_file) {
    std::string id, key, filename;
    if (!read_string(params, "profile_id", id) || !read_string(params, "module", key) ||
        !read_string(params, "filename", filename)) { fail(bad_params("module.rename_file缺少参数")); return; }
    auto changed = application_->profiles().rename_module_file(id, key, filename);
    if (!changed) fail(changed.error()); else succeed(Json{{"saved_only", true}});
    return;
  }
  if (method == methods::connection_select) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("connection.select需要profile_id")); return; }
    auto target = read_target(params);
    if (!target) fail(target.error());
    else {
      auto selected = application_->select(id, std::move(*target));
      if (!selected) fail(selected.error()); else succeed(Json{{"status", "saved_only"}});
    }
    return;
  }
  if (method == methods::group_list || method == methods::group_get) {
    std::string profile_id;
    if (!read_string(params, "profile_id", profile_id)) {
      fail(bad_params("group操作需要profile_id")); return;
    }
    auto groups = application_->profiles().groups(profile_id);
    if (!groups) { fail(groups.error()); return; }
    if (method == methods::group_list) { succeed(Json(*groups)); return; }
    std::string group_id;
    if (!read_string(params, "group_id", group_id)) { fail(bad_params("group.get需要group_id")); return; }
    const auto found = std::find_if(groups->begin(), groups->end(), [&](const GroupBinding& group) {
      return group.id == group_id;
    });
    if (found == groups->end()) { fail(AppError{ErrorCode::not_found, "找不到策略组", group_id, std::nullopt}); return; }
    succeed(Json(*found)); return;
  }
  if (method == methods::group_save || method == methods::group_remove) {
    std::string profile_id;
    if (!read_string(params, "profile_id", profile_id)) { fail(bad_params("group操作需要profile_id")); return; }
    auto groups = application_->profiles().groups(profile_id);
    if (!groups) { fail(groups.error()); return; }
    auto profile_path = application_->store().profile_path(profile_id);
    if (!profile_path) { fail(profile_path.error()); return; }
    auto original = read_text(*profile_path / "bindings" / "groups.json", 8 * 1024 * 1024);
    EditBase base;
    if (original) base.expected_text = *original;
    else if (original.error().code != ErrorCode::not_found) { fail(original.error()); return; }
    std::string saved_group_id;
    if (method == methods::group_save) {
      if (!params.contains("group") || !params["group"].is_object()) {
        fail(bad_params("group.save需要group对象", "group")); return;
      }
      try {
        auto encoded_group = params["group"];
        if (!encoded_group.contains("id") || !encoded_group["id"].is_string() || encoded_group["id"].get<std::string>().empty())
          encoded_group["id"] = new_source_id();
        auto group = encoded_group.get<GroupBinding>();
        saved_group_id = group.id;
        const auto found = std::find_if(groups->begin(), groups->end(), [&](const GroupBinding& existing) {
          return existing.id == group.id;
        });
        if (found == groups->end()) groups->push_back(std::move(group));
        else *found = std::move(group);
      } catch (const std::exception& error) { fail(bad_params(error.what(), "group")); return; }
    } else {
      std::string group_id;
      if (!read_string(params, "group_id", group_id)) { fail(bad_params("group.remove需要group_id")); return; }
      const auto before = groups->size();
      std::erase_if(*groups, [&](const GroupBinding& group) { return group.id == group_id; });
      if (groups->size() == before) { fail(AppError{ErrorCode::not_found, "找不到策略组", group_id, std::nullopt}); return; }
    }
    auto saved = application_->profiles().save_groups(profile_id, BindingsEdit{std::move(base), *groups});
    if (!saved) fail(saved.error());
    else if (method == methods::group_save) succeed(Json{{"saved", true}, {"id", saved_group_id}});
    else succeed(Json{{"removed", true}});
    return;
  }
  if (method == methods::group_select) {
    std::string profile_id, group_id, node_id;
    if (!read_string(params, "profile_id", profile_id) || !read_string(params, "group_id", group_id) ||
        !read_string(params, "node_id", node_id)) { fail(bad_params("group.select需要profile_id、group_id、node_id")); return; }
    auto selected = application_->profiles().select_group_node(profile_id, group_id, node_id);
    if (!selected) fail(selected.error()); else succeed(Json{{"saved_only", true}});
    return;
  }
  if (method == methods::group_override || method == methods::group_override_clear) {
    if (!params.contains("runtime") || !params["runtime"].is_boolean() || !params["runtime"].get<bool>()) {
      fail(bad_params("runtime override 必须显式提供 runtime=true", "runtime")); return;
    }
    std::string balancer;
    if (!read_string(params, "balancer", balancer)) { fail(bad_params("override需要balancer tag")); return; }
    std::string outbound;
    if (method == methods::group_override && !read_string(params, "outbound", outbound)) {
      fail(bad_params("override需要outbound tag")); return;
    }
    auto done = [reply = std::move(reply), id](Result<OverrideResult> response) mutable {
      if (!response) {
        reply(RpcResponse{1, id, false, std::nullopt, response.error()});
        return;
      }
      auto result = std::move(*response);
      reply(RpcResponse{1, id, true, Json{{"state", result.confirmed ? "confirmed" : "unknown"},
          {"actual_outbound", result.actual_outbound ? Json(*result.actual_outbound) : Json(nullptr)}}, std::nullopt});
    };
    auto accepted = method == methods::group_override
        ? application_->override_runtime_group(std::move(balancer), std::move(outbound), std::move(done))
        : application_->clear_runtime_group_override(std::move(balancer), std::move(done));
    if (!accepted) fail(accepted.error());
    return;
  }
  if (method == methods::connection_connect) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("connection.connect需要profile_id")); return; }
    auto target = read_target(params);
    if (!target) { fail(target.error()); return; }
    const auto operation = application_->operations().begin();
    application_->connect(id, std::move(*target), [application = application_, operation](auto result) {
      if (!result) application->operations().complete(operation, std::unexpected(result.error()));
      else application->operations().complete(operation, activation_json(*result));
    });
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::config_apply) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("config.apply需要profile_id")); return; }
    const auto operation = application_->operations().begin();
    auto done = [application = application_, operation](auto result) {
      if (!result) application->operations().complete(operation, std::unexpected(result.error()));
      else application->operations().complete(operation, activation_json(*result));
    };
    if (application_->core_manager().status().active)
      application_->connect(id, std::nullopt, std::move(done));
    else
      application_->check(id, std::move(done));
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::connection_disconnect || method == methods::core_stop) {
    const auto operation = application_->operations().begin();
    auto accepted = application_->core_manager().stop([application = application_, operation](auto result) {
      if (!result) application->operations().complete(operation, std::unexpected(result.error()));
      else application->operations().complete(operation, activation_json(*result));
    });
    if (!accepted) application_->operations().complete(operation, std::unexpected(accepted.error()));
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::config_render) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("config.render需要profile_id")); return; }
    auto rendered = application_->render(id);
    if (!rendered) { fail(rendered.error()); return; }
    Json files = Json::array();
    for (const auto& entry : std::filesystem::directory_iterator(rendered->confdir))
      files.push_back(entry.path().filename().string());
    succeed(Json{{"confdir", rendered->confdir.string()}, {"files", std::move(files)}});
    return;
  }
  if (method == methods::config_check) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("config.check需要profile_id")); return; }
    const auto operation = application_->operations().begin();
    application_->check(id, [application = application_, operation](auto result) {
      if (!result) application->operations().complete(operation, std::unexpected(result.error()));
      else application->operations().complete(operation, activation_json(*result));
    });
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::config_dump) {
    std::string id;
    if (!read_string(params, "profile_id", id)) { fail(bad_params("config.dump需要profile_id")); return; }
    const auto operation = application_->operations().begin();
    application_->dump(id, [application = application_, operation](auto result) {
      application->operations().complete(operation, std::move(result));
    });
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  if (method == methods::config_export) {
    std::string id, source, output;
    if (!read_string(params, "profile_id", id) || !read_string(params, "source", source) ||
        !read_string(params, "output", output)) { fail(bad_params("config.export缺少参数")); return; }
    auto result = application_->export_config(id, source, output);
    if (!result) fail(result.error()); else succeed(Json{{"exported", true}, {"output", output}});
    return;
  }
  if (method == methods::config_files) {
    std::string id, source;
    if (!read_string(params, "profile_id", id) || !read_string(params, "source", source)) {
      fail(bad_params("config.files需要profile_id与source")); return;
    }
    std::optional<std::string> module;
    if (params.contains("module") && params["module"].is_string()) module = params["module"].get<std::string>();
    auto result = application_->show_config(id, source, std::move(module));
    if (!result) fail(result.error()); else succeed(std::move(*result));
    return;
  }
  if (method == methods::runtime_status) {
    auto status = application_->runtime_status();
    if (!status) fail(status.error()); else succeed(std::move(*status));
    return;
  }
  if (method == methods::runtime_stats || method == methods::runtime_metrics) {
    bool watch = false;
    if (params.contains("watch")) {
      if (!params.at("watch").is_boolean()) { fail(bad_params("watch必须是布尔值", "watch")); return; }
      watch = params.at("watch").get<bool>();
    }
    succeed(application_->runtime_metrics(connection_id, watch));
    return;
  }
  if (method == methods::logs_read) {
    std::size_t limit = 64 * 1024;
    if (!read_size(params, "limit", limit, limit) || limit > 1024 * 1024) {
      fail(bad_params("日志limit必须是1MiB以内的非负整数")); return;
    }
    const auto page = application_->logs().read_after(log_cursor(params), limit);
    Json entries = Json::array();
    for (const auto& entry : page.entries)
      entries.push_back(Json{{"cursor", entry.cursor}, {"stream", log_stream_name(entry.stream)},
                             {"text", entry.text}});
    succeed(Json{{"entries", std::move(entries)}, {"next_cursor", page.next_cursor},
                 {"truncated", page.truncated}});
    return;
  }
  if (method == methods::operation_get) {
    std::string operation;
    if (!read_string(params, "operation_id", operation)) { fail(bad_params("operation.get需要operation_id")); return; }
    auto result = application_->operations().get(operation);
    if (!result) fail(result.error()); else succeed(std::move(*result));
    return;
  }
  if (method == methods::core_start || method == methods::core_restart) {
    std::string id;
    if (params.contains("profile_id") && params["profile_id"].is_string()) id = params["profile_id"].get<std::string>();
    if (id.empty()) {
      const auto status = application_->core_manager().status();
      if (status.active) id = status.active->profile_id;
    }
    if (id.empty()) { fail(AppError{ErrorCode::not_found, "需要profile_id或现有活动Profile", std::nullopt, std::nullopt}); return; }
    const auto operation = application_->operations().begin();
    application_->connect(id, std::nullopt, [application = application_, operation](auto result) {
      if (!result) application->operations().complete(operation, std::unexpected(result.error()));
      else application->operations().complete(operation, activation_json(*result));
    });
    succeed(Json{{"status", "accepted"}, {"operation_id", operation}});
    return;
  }
  fail(AppError{ErrorCode::unsupported, "不支持的RPC方法", method, std::nullopt});
}

}  // namespace xrm
