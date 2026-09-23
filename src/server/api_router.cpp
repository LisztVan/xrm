#include "server/api_router.hpp"

#include "contract/methods.hpp"

#include <limits>

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

Json module_view_json(const ModuleView& view) {
  return Json{{"key", view.descriptor.key}, {"label", view.descriptor.label},
      {"source_file", (std::filesystem::path("modules") / view.setting.filename).generic_string()},
      {"runtime_file", view.setting.filename.generic_string()}, {"included", view.setting.included},
      {"text", view.document ? Json(view.document->source_text) : Json(nullptr)},
      {"editor_support", view.editor_support}};
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

void ApiRouter::dispatch(const Id&, RpcRequest request, RpcReply reply) {
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
  if (method == methods::node_add) {
    std::string name;
    if (!read_string(params, "name", name)) {
      fail(bad_params("node.add需要非空name", "name"));
      return;
    }
    if (!params.contains("outbound") || !params.at("outbound").is_object()) {
      fail(bad_params("node.add需要原生outbound对象", "outbound"));
      return;
    }
    auto added = application_->add_manual_node(std::move(name), params.at("outbound"));
    if (!added) fail(added.error());
    else succeed(Json(*added));
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
    auto page = application_->list_nodes(query, {offset, limit});
    if (!page) fail(page.error());
    else succeed(page_to_json(*page));
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
    if (!read_string(params, "name", name) || !read_string(params, "core_id", core_id)) {
      fail(bad_params("profile.create需要name和core_id")); return;
    }
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
    EditBase base;
    if (params.contains("expected_text") && !params["expected_text"].is_null()) {
      if (!params["expected_text"].is_string()) { fail(bad_params("expected_text必须为字符串或null")); return; }
      base.expected_text = params["expected_text"].get<std::string>();
    } else {
      auto current = application_->profiles().module(id, key);
      if (!current) { fail(current.error()); return; }
      base.expected_text = current->document ? current->document->source_text : std::string{};
    }
    auto saved = application_->profiles().save_module(id, ModuleEdit{key, std::move(base), std::move(text)});
    if (!saved) fail(saved.error()); else succeed(Json{{"status", "saved_only"}});
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
