/// @file
/// @brief Implements the client side of the daemon RPC transport.

#include "client/rpc_client.hpp"

#include <uv.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace xrm {
namespace {

constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;

AppError uv_error(int code, std::string action) {
  action += ": ";
  action += uv_strerror(code);
  return {ErrorCode::io_error, std::move(action), std::nullopt, std::nullopt};
}

AppError protocol_error(std::string message) {
  return {ErrorCode::protocol_error, std::move(message), std::nullopt,
          std::nullopt};
}

} // namespace

struct RpcClient::Impl {
  struct Write {
    uv_write_t request{};
    Impl *owner = nullptr;
    std::string bytes;
  };

  explicit Impl(uv_loop_t &event_loop) : loop(&event_loop) {}

  static void allocate(uv_handle_t *, std::size_t suggested, uv_buf_t *buffer) {
    const auto size = std::clamp<std::size_t>(suggested, 4096, 65536);
    buffer->base = static_cast<char *>(std::malloc(size));
    buffer->len = buffer->base ? size : 0;
  }

  static void on_connect(uv_connect_t *request, int status) {
    auto *self = static_cast<Impl *>(request->data);
    if (status != 0) {
      self->complete_open(std::unexpected(uv_error(status, "连接xrmd失败")));
      self->close_handle();
      return;
    }
    if (self->closing) {
      self->complete_open(std::unexpected(AppError{
          ErrorCode::cancelled, "IPC连接已关闭", std::nullopt, std::nullopt}));
      return;
    }
    const int read_status = uv_read_start(
        reinterpret_cast<uv_stream_t *>(&self->pipe), allocate, on_read);
    if (read_status != 0) {
      self->complete_open(
          std::unexpected(uv_error(read_status, "启动IPC读取失败")));
      self->fail_pending(uv_error(read_status, "启动IPC读取失败"));
      self->close_handle();
      return;
    }
    self->connected = true;
    self->complete_open(Result<void>{});
  }

  static void on_read(uv_stream_t *stream, ssize_t count,
                      const uv_buf_t *buffer) {
    auto *self = static_cast<Impl *>(stream->data);
    if (count > 0) {
      self->input.append(buffer->base, static_cast<std::size_t>(count));
      std::free(buffer->base);
      self->consume_frames();
      return;
    }
    std::free(buffer->base);
    if (count < 0) {
      self->fail_pending(AppError{ErrorCode::io_error, "与xrmd的IPC连接已断开",
                                  std::nullopt, std::nullopt});
      self->close_handle();
    }
  }

  static void on_write(uv_write_t *request, int status) {
    std::unique_ptr<Write> write(static_cast<Write *>(request->data));
    if (status != 0) {
      auto error = uv_error(status, "发送IPC请求失败");
      write->owner->fail_pending(error);
      write->owner->close_handle();
    }
  }

  void complete_open(Result<void> result) {
    if (open_completed)
      return;
    open_completed = true;
    auto callback = std::move(open_completion);
    if (callback)
      callback(std::move(result));
  }

  void fail_pending(const AppError &error) {
    auto outstanding = std::move(pending);
    pending.clear();
    for (auto &[_, callback] : outstanding) {
      if (callback)
        callback(std::unexpected(error));
    }
  }

  void close_handle() {
    if (!initialized || uv_is_closing(reinterpret_cast<uv_handle_t *>(&pipe)))
      return;
    closing = true;
    connected = false;
    uv_read_stop(reinterpret_cast<uv_stream_t *>(&pipe));
    uv_close(reinterpret_cast<uv_handle_t *>(&pipe), nullptr);
  }

  void consume_frames() {
    for (;;) {
      const auto newline = input.find('\n');
      if (newline == std::string::npos) {
        if (input.size() > kMaxFrameBytes) {
          fail_pending(AppError{ErrorCode::too_large, "RPC响应帧超过8MiB上限",
                                std::nullopt, std::nullopt});
          close_handle();
        }
        return;
      }
      if (newline > kMaxFrameBytes) {
        fail_pending(AppError{ErrorCode::too_large, "RPC响应帧超过8MiB上限",
                              std::nullopt, std::nullopt});
        close_handle();
        return;
      }
      auto line = input.substr(0, newline);
      input.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      Json response;
      try {
        response = Json::parse(line);
      } catch (const Json::exception &) {
        fail_pending(protocol_error("xrmd返回的RPC帧不是有效JSON"));
        close_handle();
        return;
      }
      if (!response.is_object() || !response.contains("v") ||
          !response.at("v").is_number_integer() || response.at("v") != 1 ||
          !response.contains("id") || !response.at("id").is_number_unsigned() ||
          !response.contains("ok") || !response.at("ok").is_boolean()) {
        fail_pending(protocol_error("xrmd返回的RPC响应缺少有效字段"));
        close_handle();
        return;
      }
      const auto id = response.at("id").get<std::uint64_t>();
      const auto found = pending.find(id);
      if (found == pending.end())
        continue;
      auto callback = std::move(found->second);
      pending.erase(found);
      if (response.at("ok").get<bool>()) {
        if (!response.contains("result")) {
          callback(std::unexpected(protocol_error("成功响应缺少result")));
        } else {
          callback(response.at("result"));
        }
      } else if (!response.contains("error")) {
        callback(std::unexpected(protocol_error("失败响应缺少error")));
      } else {
        auto error = error_from_json(response.at("error"));
        if (error)
          callback(std::unexpected(std::move(*error)));
        else
          callback(std::unexpected(error.error()));
      }
    }
  }

  uv_loop_t *loop;
  uv_pipe_t pipe{};
  uv_connect_t connect_request{};
  bool initialized = false;
  bool connected = false;
  bool closing = false;
  bool open_completed = false;
  std::uint64_t next_request = 1;
  std::uint64_t next_connection = 1;
  std::filesystem::path socket_path;
  std::string input;
  Completion<void> open_completion;
  std::map<std::uint64_t, Completion<Json>> pending;
};

RpcClient::RpcClient(uv_loop_t &loop) : impl_(std::make_unique<Impl>(loop)) {}
RpcClient::~RpcClient() = default;

Result<Id> RpcClient::open(std::filesystem::path socket,
                           Completion<void> completion) {
  if (impl_->initialized || socket.empty()) {
    return std::unexpected(
        AppError{ErrorCode::invalid_argument,
                 impl_->initialized ? "IPC客户端已打开" : "Socket路径不能为空",
                 std::nullopt, std::nullopt});
  }
  const int status = uv_pipe_init(impl_->loop, &impl_->pipe, 0);
  if (status != 0)
    return std::unexpected(uv_error(status, "初始化IPC客户端失败"));
  impl_->initialized = true;
  impl_->pipe.data = impl_.get();
  impl_->socket_path = std::move(socket);
  impl_->open_completion = std::move(completion);
  impl_->connect_request.data = impl_.get();
  uv_pipe_connect(&impl_->connect_request, &impl_->pipe,
                  impl_->socket_path.string().c_str(), Impl::on_connect);
  return "connection-" + std::to_string(impl_->next_connection++);
}

Result<std::uint64_t> RpcClient::call(std::string method, Json params,
                                      Completion<Json> completion) {
  if (!impl_->connected || impl_->closing) {
    return std::unexpected(AppError{ErrorCode::busy, "IPC尚未连接，未发送请求",
                                    std::nullopt, std::nullopt});
  }
  if (method.empty() || !params.is_object()) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "RPC method不能为空且params必须是对象",
                                    std::nullopt, std::nullopt});
  }
  const auto id = impl_->next_request++;
  const auto frame =
      Json{{"v", 1}, {"id", id}, {"method", method}, {"params", params}}
          .dump() +
      '\n';
  if (frame.size() > kMaxFrameBytes) {
    return std::unexpected(AppError{ErrorCode::too_large, "RPC请求超过8MiB上限",
                                    std::nullopt, std::nullopt});
  }
  auto write = std::make_unique<Impl::Write>();
  write->owner = impl_.get();
  write->bytes = frame;
  uv_buf_t buffer = uv_buf_init(write->bytes.data(),
                                static_cast<unsigned int>(write->bytes.size()));
  impl_->pending.emplace(id, std::move(completion));
  const int status =
      uv_write(&write->request, reinterpret_cast<uv_stream_t *>(&impl_->pipe),
               &buffer, 1, Impl::on_write);
  if (status != 0) {
    impl_->pending.erase(id);
    return std::unexpected(uv_error(status, "发送IPC请求失败"));
  }
  auto *pending_write = write.release();
  pending_write->request.data = pending_write;
  return id;
}

void RpcClient::close() {
  if (!impl_->closing) {
    impl_->closing = true;
    impl_->fail_pending(AppError{ErrorCode::cancelled, "IPC客户端已关闭",
                                 std::nullopt, std::nullopt});
    impl_->complete_open(std::unexpected(AppError{
        ErrorCode::cancelled, "IPC客户端已关闭", std::nullopt, std::nullopt}));
    impl_->close_handle();
  }
}

} // namespace xrm
