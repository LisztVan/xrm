// XRM: Manages a reusable RPC session and its connection lifetime.

#pragma once

#include "client/rpc_client.hpp"
#include "infra/pipe_write_scope.hpp"

#include <uv.h>

#include <memory>
#include <optional>

namespace xrm {

// A small synchronous facade over the existing asynchronous RpcClient. Unlike
// the one-shot CLI helper, this keeps one UDS connection open so server-side
// viewer ownership remains tied to a live connection for watch operations.
class RpcSession {
public:
  explicit RpcSession(const std::string &socket) {
    const int initialized = uv_loop_init(&loop_);
    if (initialized != 0) {
      error_ = AppError{ErrorCode::io_error, uv_strerror(initialized),
                        std::nullopt, std::nullopt};
      return;
    }
    loop_initialized_ = true;
    client_ = std::make_unique<RpcClient>(loop_);
    std::optional<Result<void>> opened;
    auto result = client_->open(
        socket, [&](Result<void> value) { opened.emplace(std::move(value)); });
    if (!result) {
      error_ = result.error();
      return;
    }
    while (!opened)
      uv_run(&loop_, UV_RUN_ONCE);
    if (!*opened)
      error_ = opened->error();
  }

  RpcSession(const RpcSession &) = delete;
  RpcSession &operator=(const RpcSession &) = delete;
  ~RpcSession() { close(); }

  Result<Json> call(std::string method, Json params = Json::object()) {
    if (error_)
      return std::unexpected(*error_);
    if (!client_)
      return std::unexpected(AppError{ErrorCode::cancelled, "RPC会话已关闭",
                                      std::nullopt, std::nullopt});
    PipeWriteScope pipe_write;
    std::optional<Result<Json>> response;
    auto sent = client_->call(
        std::move(method), std::move(params),
        [&](Result<Json> value) { response.emplace(std::move(value)); });
    if (!sent)
      return std::unexpected(sent.error());
    while (!response)
      uv_run(&loop_, UV_RUN_ONCE);
    return std::move(*response);
  }

  void close() {
    if (client_) {
      client_->close();
      uv_run(&loop_, UV_RUN_DEFAULT);
      client_.reset();
    }
    if (loop_initialized_) {
      (void)uv_loop_close(&loop_);
      loop_initialized_ = false;
    }
  }

private:
  uv_loop_t loop_{};
  std::unique_ptr<RpcClient> client_;
  std::optional<AppError> error_;
  bool loop_initialized_ = false;
};

} // namespace xrm
