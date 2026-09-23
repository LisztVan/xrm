#include "server/rpc_server.hpp"

#include <uv.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace xrm {
namespace {

constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxClientQueuedBytes = 1024 * 1024;

AppError uv_error(int code, std::string action) {
  action += ": ";
  action += uv_strerror(code);
  return {ErrorCode::io_error, std::move(action), std::nullopt, std::nullopt};
}

Json error_response(std::uint64_t id, const AppError& error) {
  return Json{{"v", 1}, {"id", id}, {"ok", false}, {"error", error_to_json(error)}};
}

std::uint64_t request_id_hint(std::string_view line) {
  try {
    const auto json = Json::parse(line);
    if (json.is_object() && json.contains("id") && json.at("id").is_number_unsigned()) {
      return json.at("id").get<std::uint64_t>();
    }
  } catch (const Json::exception&) {
  }
  return 0;
}

}  // namespace

struct RpcServer::Impl {
  struct Client {
    Impl* owner = nullptr;
    Id id;
    uv_pipe_t pipe{};
    bool closing = false;
    std::string input;
    std::size_t queued_bytes = 0;
  };

  struct Write {
    uv_write_t request{};
    Impl* owner = nullptr;
    Id connection_id;
    std::string bytes;
    bool close_after = false;
  };

  Impl(uv_loop_t& event_loop, RpcHandler request_handler)
      : loop(&event_loop), handler(std::move(request_handler)) {}

  static void allocate(uv_handle_t*, std::size_t suggested, uv_buf_t* buffer) {
    const auto size = std::clamp<std::size_t>(suggested, 4096, 65536);
    buffer->base = static_cast<char*>(std::malloc(size));
    buffer->len = buffer->base ? size : 0;
  }

  static void on_listen(uv_stream_t* stream, int status) {
    auto* self = static_cast<Impl*>(stream->data);
    if (status < 0 || self->closing) return;
    auto client = std::make_unique<Client>();
    client->owner = self;
    client->id = "connection-" + std::to_string(self->next_connection++);
    const auto id = client->id;
    const int initialized = uv_pipe_init(self->loop, &client->pipe, 0);
    if (initialized != 0) return;
    client->pipe.data = client.get();
    self->clients.emplace(id, std::move(client));
    auto* raw = self->clients.at(id).get();
    const int accepted = uv_accept(stream, reinterpret_cast<uv_stream_t*>(&raw->pipe));
    if (accepted != 0) {
      self->close_client(id);
      return;
    }
    const int read_status = uv_read_start(reinterpret_cast<uv_stream_t*>(&raw->pipe),
                                          allocate, on_read);
    if (read_status != 0) self->close_client(id);
  }

  static void on_read(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
    auto* client = static_cast<Client*>(stream->data);
    auto* self = client->owner;
    if (count > 0) {
      client->input.append(buffer->base, static_cast<std::size_t>(count));
      std::free(buffer->base);
      self->consume_frames(client);
      return;
    }
    std::free(buffer->base);
    if (count < 0) self->close_client(client->id);
  }

  static void on_write(uv_write_t* request, int status) {
    std::unique_ptr<Write> write(static_cast<Write*>(request->data));
    auto* self = write->owner;
    if (self->pending_writes > 0) --self->pending_writes;
    const auto found = self->clients.find(write->connection_id);
    if (found != self->clients.end()) {
      auto& queued = found->second->queued_bytes;
      queued -= std::min(queued, write->bytes.size());
    }
    if (status != 0 || write->close_after) self->close_client(write->connection_id);
    self->notify_drained();
  }

  static void on_client_closed(uv_handle_t* handle) {
    auto* client = static_cast<Client*>(handle->data);
    auto* self = client->owner;
    const auto id = client->id;
    self->clients.erase(id);
    self->notify_drained();
  }

  static void on_listener_closed(uv_handle_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    self->listener_open = false;
    if (!self->socket_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(self->socket_path, ignored);
    }
    self->notify_drained();
  }

  void close_client(const Id& id) {
    const auto found = clients.find(id);
    if (found == clients.end() || found->second->closing) return;
    auto* client = found->second.get();
    client->closing = true;
    uv_read_stop(reinterpret_cast<uv_stream_t*>(&client->pipe));
    uv_close(reinterpret_cast<uv_handle_t*>(&client->pipe), on_client_closed);
  }

  void write(Client* client, std::string bytes, bool close_after) {
    if (!client || client->closing) return;
    if (bytes.size() > kMaxClientQueuedBytes ||
        client->queued_bytes > kMaxClientQueuedBytes - bytes.size()) {
      // RPC responses are correlated and cannot be silently discarded while
      // keeping the connection usable. Disconnect this slow consumer instead
      // of allowing libuv's per-stream write queue to grow without bound.
      close_client(client->id);
      return;
    }
    auto request = std::make_unique<Write>();
    request->owner = this;
    request->connection_id = client->id;
    request->bytes = std::move(bytes);
    request->close_after = close_after;
    uv_buf_t buffer = uv_buf_init(request->bytes.data(),
                                  static_cast<unsigned int>(request->bytes.size()));
    const int status = uv_write(&request->request,
                                reinterpret_cast<uv_stream_t*>(&client->pipe),
                                &buffer, 1, on_write);
    if (status != 0) {
      close_client(client->id);
      return;
    }
    ++pending_writes;
    client->queued_bytes += request->bytes.size();
    auto* pending_write = request.release();
    pending_write->request.data = pending_write;
  }

  void reply(const Id& connection_id, std::uint64_t request_id, RpcResponse response) {
    const auto found = clients.find(connection_id);
    if (found != clients.end() && !found->second->closing) {
      response.v = 1;
      response.id = request_id;
      auto bytes = encode_response(response);
      if (bytes.size() > kMaxFrameBytes) {
        bytes = encode_response(RpcResponse{
            1, request_id, false, std::nullopt,
            AppError{ErrorCode::too_large, "RPC响应超过8MiB上限",
                     std::nullopt, std::nullopt}});
      }
      write(found->second.get(), std::move(bytes), false);
    }
  }

  void dispatch(Client* client, RpcRequest request) {
    const auto connection_id = client->id;
    const auto request_id = request.id;
    ++pending_replies;
    auto answered = std::make_shared<bool>(false);
    auto callback = [this, connection_id, request_id, answered](RpcResponse response) mutable {
      if (*answered) return;
      *answered = true;
      reply(connection_id, request_id, std::move(response));
      --pending_replies;
      notify_drained();
    };
    try {
      handler(connection_id, std::move(request), callback);
    } catch (const std::exception& error) {
      callback(RpcResponse{1, request_id, false, std::nullopt,
                           AppError{ErrorCode::protocol_error,
                                    std::string("RPC handler异常: ") + error.what(),
                                    std::nullopt, std::nullopt}});
    } catch (...) {
      callback(RpcResponse{1, request_id, false, std::nullopt,
                           AppError{ErrorCode::protocol_error, "RPC handler异常",
                                    std::nullopt, std::nullopt}});
    }
  }

  void consume_frames(Client* client) {
    for (;;) {
      const auto newline = client->input.find('\n');
      if (newline == std::string::npos) {
        if (client->input.size() > kMaxFrameBytes) {
          write(client, encode_response(RpcResponse{
                            1, 0, false, std::nullopt,
                            AppError{ErrorCode::too_large, "RPC请求帧超过8MiB上限",
                                     std::nullopt, std::nullopt}}), true);
        }
        return;
      }
      if (newline > kMaxFrameBytes) {
        const auto id = request_id_hint(std::string_view(client->input).substr(0, newline));
        RpcResponse response{1, id, false, std::nullopt,
                             AppError{ErrorCode::too_large, "RPC请求帧超过8MiB上限",
                                      std::nullopt, std::nullopt}};
        write(client, encode_response(response), true);
        return;
      }
      auto line = client->input.substr(0, newline);
      client->input.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto request = decode_request(line);
      if (!request) {
        const auto id = request_id_hint(line);
        RpcResponse response{1, id, false, std::nullopt, request.error()};
        write(client, encode_response(response), true);
        return;
      }
      dispatch(client, std::move(*request));
      if (client->closing) return;
    }
  }

  void notify_drained() {
    if (!closing || listener_open || !clients.empty() || pending_replies != 0 ||
        pending_writes != 0 || !drained) return;
    auto callback = std::move(drained);
    callback();
  }

  uv_loop_t* loop;
  RpcHandler handler;
  uv_pipe_t listener{};
  bool listener_open = false;
  bool closing = false;
  std::uint64_t next_connection = 1;
  std::filesystem::path socket_path;
  std::map<Id, std::unique_ptr<Client>> clients;
  std::size_t pending_replies = 0;
  std::size_t pending_writes = 0;
  std::function<void()> drained;
};

RpcServer::RpcServer(uv_loop_t& loop, RpcHandler handler)
    : impl_(std::make_unique<Impl>(loop, std::move(handler))) {}
RpcServer::~RpcServer() = default;

Result<void> RpcServer::listen(const std::filesystem::path& socket) {
  if (socket.empty() || impl_->listener_open || !impl_->handler) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "RPC Server需要socket路径和handler，且只能listen一次",
                                    std::nullopt, std::nullopt});
  }
  int status = uv_pipe_init(impl_->loop, &impl_->listener, 0);
  if (status != 0) return std::unexpected(uv_error(status, "初始化RPC listener失败"));
  impl_->listener_open = true;
  impl_->listener.data = impl_.get();
  status = uv_pipe_bind(&impl_->listener, socket.string().c_str());
  if (status != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&impl_->listener), Impl::on_listener_closed);
    return std::unexpected(uv_error(status, "绑定Unix Socket失败"));
  }
  impl_->socket_path = socket;
#ifndef _WIN32
  if (::chmod(socket.c_str(), S_IRUSR | S_IWUSR) != 0) {
    const auto error = AppError{ErrorCode::io_error, "设置Unix Socket权限失败",
                                socket.string(), std::nullopt};
    uv_close(reinterpret_cast<uv_handle_t*>(&impl_->listener), Impl::on_listener_closed);
    return std::unexpected(error);
  }
#endif
  status = uv_listen(reinterpret_cast<uv_stream_t*>(&impl_->listener), 128,
                     Impl::on_listen);
  if (status != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&impl_->listener), Impl::on_listener_closed);
    return std::unexpected(uv_error(status, "监听Unix Socket失败"));
  }
  return {};
}

void RpcServer::close(std::function<void()> drained) {
  if (impl_->closing) return;
  impl_->closing = true;
  impl_->drained = std::move(drained);
  if (impl_->listener_open &&
      !uv_is_closing(reinterpret_cast<uv_handle_t*>(&impl_->listener))) {
    uv_close(reinterpret_cast<uv_handle_t*>(&impl_->listener), Impl::on_listener_closed);
  }
  std::vector<Id> clients;
  clients.reserve(impl_->clients.size());
  for (const auto& [id, _] : impl_->clients) clients.push_back(id);
  for (const auto& id : clients) impl_->close_client(id);
  impl_->notify_drained();
}

}  // namespace xrm
