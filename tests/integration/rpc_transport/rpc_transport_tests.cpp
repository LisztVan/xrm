#include <catch2/catch_test_macros.hpp>

#include "client/rpc_client.hpp"
#include "server/rpc_server.hpp"

#include <uv.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

struct Loop {
  uv_loop_t value{};
  bool closed = false;
  Loop() { REQUIRE(uv_loop_init(&value) == 0); }
  ~Loop() { if (!closed) uv_loop_close(&value); }
  int close() { closed = true; return uv_loop_close(&value); }
};

std::filesystem::path socket_path() {
  return std::filesystem::temp_directory_path() /
         ("xrm-rpc-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) + ".sock");
}

#ifndef _WIN32
int connect_socket(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto value = path.string();
  std::copy(value.begin(), value.end(), address.sun_path);
  address.sun_path[value.size()] = '\0';
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool send_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto written = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (written <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool receive_line(int fd, std::string& pending, std::string& line) {
  while (true) {
    const auto newline = pending.find('\n');
    if (newline != std::string::npos) {
      line = pending.substr(0, newline);
      pending.erase(0, newline + 1);
      return true;
    }
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, 3000) <= 0) return false;
    char buffer[4096];
    const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) return false;
    pending.append(buffer, static_cast<std::size_t>(count));
  }
}

struct LoopStopper {
  uv_async_t signal{};
  xrm::RpcServer* server = nullptr;
  bool drained = false;
  bool sent = false;
  std::thread loop_thread;
  static void request_stop(uv_async_t* handle) {
    auto* self = static_cast<LoopStopper*>(handle->data);
    self->server->close([self] { self->drained = true; });
    uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
  }
  void start(uv_loop_t& loop) { loop_thread = std::thread([&loop] { uv_run(&loop, UV_RUN_DEFAULT); }); }
  void stop() {
    if (loop_thread.joinable()) {
      if (!sent) {
        sent = true;
        uv_async_send(&signal);
      }
      loop_thread.join();
    }
  }
  ~LoopStopper() { stop(); }
};
#endif

}  // namespace

TEST_CASE("RPC-01 client and server exchange independent correlated replies", "[RPC-01]") {
  Loop loop;
  xrm::RpcServer server(loop.value, [](const xrm::Id&, xrm::RpcRequest request,
                                       xrm::RpcReply reply) {
    reply(xrm::RpcResponse{1, request.id, true,
                           xrm::Json{{"method", request.method}, {"params", request.params}},
                           std::nullopt});
  });
  const auto socket = socket_path();
  REQUIRE(server.listen(socket));
  xrm::RpcClient client(loop.value);
  bool open_done = false;
  bool open_succeeded = false;
  bool request_failed = false;
  bool drained = false;
  std::vector<xrm::Result<xrm::Json>> replies;
  REQUIRE_FALSE(client.call("echo", xrm::Json::object(), [](auto) {}));
  REQUIRE(client.open(socket, [&](auto opened) {
    open_done = true;
    open_succeeded = static_cast<bool>(opened);
    if (!opened) return;
    auto first = client.call("echo", {{"text", "line one\nline two"}},
                             [&](auto result) {
                               if (!result) request_failed = true;
                               replies.push_back(std::move(result));
                               if (replies.size() == 2) {
                                 client.close();
                                 server.close([&] { drained = true; });
                               }
                             });
    auto second = client.call("node.list", {{"limit", 5}},
                              [&](auto result) {
                                if (!result) request_failed = true;
                                replies.push_back(std::move(result));
                                if (replies.size() == 2) {
                                  client.close();
                                  server.close([&] { drained = true; });
                                }
                              });
    request_failed = !first || !second;
  }));
  uv_run(&loop.value, UV_RUN_DEFAULT);
  REQUIRE(open_done);
  REQUIRE(open_succeeded);
  REQUIRE_FALSE(request_failed);
  REQUIRE(replies.size() == 2);
  REQUIRE(replies[0]);
  REQUIRE(replies[1]);
  REQUIRE(replies[0]->at("params").at("text") == "line one\nline two");
  REQUIRE(replies[1]->at("method") == "node.list");
  REQUIRE(drained);
}

TEST_CASE("RPC-02 unsupported wire versions have structured errors", "[RPC-02]") {
  const auto invalid = xrm::decode_request(
      R"({"v":9,"id":42,"method":"node.list","params":{}})");
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code == xrm::ErrorCode::unsupported);
}

TEST_CASE("RPC-02 unavailable UDS is reported through the open callback", "[RPC-02]") {
  Loop loop;
  xrm::RpcClient client(loop.value);
  bool completed = false;
  bool callback_ok = false;
  xrm::AppError error{xrm::ErrorCode::io_error, "unset", std::nullopt, std::nullopt};
  const auto socket = socket_path();
  REQUIRE(client.open(socket, [&](auto result) {
    completed = true;
    callback_ok = static_cast<bool>(result);
    if (!result) error = result.error();
  }));
  uv_run(&loop.value, UV_RUN_DEFAULT);
  REQUIRE(completed);
  REQUIRE_FALSE(callback_ok);
  REQUIRE(error.code == xrm::ErrorCode::io_error);
}

#ifndef _WIN32
TEST_CASE("RPC-01 stream framing accepts fragments and combined frames and bounds size", "[RPC-01]") {
  Loop loop;
  xrm::RpcServer server(loop.value, [](const xrm::Id&, xrm::RpcRequest request,
                                       xrm::RpcReply reply) {
    reply(xrm::RpcResponse{1, request.id, true,
                           xrm::Json{{"method", request.method}, {"params", request.params}},
                           std::nullopt});
  });
  const auto socket = socket_path();
  REQUIRE(server.listen(socket));
  LoopStopper stopper;
  stopper.server = &server;
  REQUIRE(uv_async_init(&loop.value, &stopper.signal, LoopStopper::request_stop) == 0);
  stopper.signal.data = &stopper;
  stopper.start(loop.value);

  std::string receive_buffer;
  const int fd = connect_socket(socket);
  REQUIRE(fd >= 0);
  const std::string first = R"({"v":1,"id":1,"method":"echo","params":{"text":"split"}})" "\n";
  const std::string second = R"({"v":1,"id":2,"method":"echo","params":{"n":2}})" "\n";
  REQUIRE(send_all(fd, std::string_view(first).substr(0, 9)));
  REQUIRE(send_all(fd, first.substr(9) + second));
  std::string line;
  REQUIRE(receive_line(fd, receive_buffer, line));
  const auto first_response = xrm::Json::parse(line);
  REQUIRE(first_response.at("id") == 1);
  REQUIRE(first_response.at("result").at("params").at("text") == "split");
  REQUIRE(receive_line(fd, receive_buffer, line));
  const auto second_response = xrm::Json::parse(line);
  REQUIRE(second_response.at("id") == 2);
  REQUIRE(second_response.at("result").at("params").at("n") == 2);
  ::close(fd);

  const int unsupported_fd = connect_socket(socket);
  REQUIRE(unsupported_fd >= 0);
  REQUIRE(send_all(unsupported_fd,
                   R"({"v":9,"id":42,"method":"node.list","params":{}})" "\n"));
  REQUIRE(receive_line(unsupported_fd, receive_buffer, line));
  const auto unsupported = xrm::Json::parse(line);
  REQUIRE(unsupported.at("id") == 42);
  REQUIRE(unsupported.at("error").at("code") == "unsupported");
  ::close(unsupported_fd);

  const int partial_fd = connect_socket(socket);
  REQUIRE(partial_fd >= 0);
  REQUIRE(send_all(partial_fd, R"({"v":1,"id":8)"));
  ::shutdown(partial_fd, SHUT_WR);
  char byte = 0;
  pollfd partial_descriptor{partial_fd, POLLIN | POLLHUP, 0};
  REQUIRE(::poll(&partial_descriptor, 1, 3000) > 0);
  REQUIRE(::recv(partial_fd, &byte, 1, 0) == 0);
  ::close(partial_fd);

  const int oversized_fd = connect_socket(socket);
  REQUIRE(oversized_fd >= 0);
  REQUIRE(send_all(oversized_fd, std::string(8 * 1024 * 1024 + 1, 'x')));
  REQUIRE(receive_line(oversized_fd, receive_buffer, line));
  const auto oversized = xrm::Json::parse(line);
  REQUIRE(oversized.at("error").at("code") == "too_large");
  ::close(oversized_fd);

  stopper.stop();
  REQUIRE(stopper.drained);
  REQUIRE(loop.close() == 0);
}
#endif
