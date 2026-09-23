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
  fail(AppError{ErrorCode::unsupported, "不支持的RPC方法", method, std::nullopt});
}

}  // namespace xrm
