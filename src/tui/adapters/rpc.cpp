// XRM: Adapts daemon RPC responses for the TUI workspace.

#include "tui/adapters/rpc.hpp"
#include "client/rpc_client.hpp"
#include "contract/methods.hpp"
#include "infra/pipe_write_scope.hpp"
#include <uv.h>
namespace xrm::tui {
Result<Json> request(const std::string &socket, std::string method,
                     Json params) {
  PipeWriteScope pipe_write;
  uv_loop_t loop{};
  int status = uv_loop_init(&loop);
  if (status != 0)
    return std::unexpected(AppError{ErrorCode::io_error, uv_strerror(status),
                                    std::nullopt, std::nullopt});
  RpcClient client(loop);
  std::optional<Result<Json>> result;
  auto opened = client.open(socket, [&](Result<void> open_result) {
    if (!open_result) {
      result.emplace(std::unexpected(open_result.error()));
      return;
    }
    auto call = client.call(std::move(method), std::move(params),
                            [&](Result<Json> response) {
                              result.emplace(std::move(response));
                              client.close();
                            });
    if (!call) {
      result.emplace(std::unexpected(call.error()));
      client.close();
    }
  });
  if (!opened)
    result.emplace(std::unexpected(opened.error()));
  else
    uv_run(&loop, UV_RUN_DEFAULT);
  client.close();
  uv_loop_close(&loop);
  if (!result)
    return std::unexpected(AppError{ErrorCode::result_unknown,
                                    "TUI RPC请求没有结果", std::nullopt,
                                    std::nullopt});
  return std::move(*result);
}

Result<Json> inventory(const std::string &socket) {
  Json nodes = Json::array();
  std::size_t offset = 0;
  for (;;) {
    auto response = request(socket, std::string(methods::node_list),
                            Json{{"offset", offset}, {"limit", 500}});
    if (!response)
      return std::unexpected(response.error());
    if (!response->contains("items") || !response->at("items").is_array())
      return std::unexpected(AppError{ErrorCode::io_error,
                                      "node.list returned no items",
                                      std::nullopt, std::nullopt});
    auto batch = response->at("items").get<std::vector<Json>>();
    for (auto &node : batch)
      nodes.push_back(std::move(node));
    if (!response->contains("next_offset") ||
        response->at("next_offset").is_null())
      break;
    const auto next = response->at("next_offset").get<std::size_t>();
    if (next <= offset)
      return std::unexpected(AppError{ErrorCode::io_error,
                                      "node.list pagination did not advance",
                                      std::nullopt, std::nullopt});
    offset = next;
  }
  return nodes;
}

} // namespace xrm::tui
