#include "server/node_test_service.hpp"

#include "infra/http.hpp"

#include <catch2/catch_test_macros.hpp>
#include <uv.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct Listener {
  int fd = -1;
  std::uint16_t port = 0;
  Listener() {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    port = ntohs(address.sin_port);
    REQUIRE(::listen(fd, 2) == 0);
  }
  ~Listener() { if (fd >= 0) ::close(fd); }
};

struct HttpFixture {
  Listener listener;
  std::atomic<int> requests{0};
  std::atomic<bool> stopped{false};
  std::thread thread;
  explicit HttpFixture(std::string body, std::chrono::milliseconds delay = 0ms) {
    thread = std::thread([this, body = std::move(body), delay] {
      while (!stopped.load()) {
        pollfd item{listener.fd, POLLIN, 0};
        if (::poll(&item, 1, 50) <= 0) continue;
        const int peer = ::accept(listener.fd, nullptr, nullptr);
        if (peer < 0) continue;
        char buffer[4096];
        (void)::recv(peer, buffer, sizeof(buffer), 0);
        ++requests;
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
          const auto size = ::send(peer, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
          if (size <= 0) break;
          sent += static_cast<std::size_t>(size);
        }
        ::close(peer);
      }
    });
  }
  ~HttpFixture() {
    stopped = true;
    if (thread.joinable()) thread.join();
  }
};

struct TempRoot {
  std::filesystem::path path;
  TempRoot() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "xrm-node-test-XXXXXX").string();
    char* name = ::mkdtemp(pattern.data());
    REQUIRE(name != nullptr);
    path = name;
  }
  ~TempRoot() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

std::uint16_t free_port() {
  Listener temporary;
  return temporary.port;
}

bool port_open(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return connected;
}

xrm::Node node(std::uint16_t port) {
  xrm::Node result;
  result.id = "node-test-loopback";
  result.original_name = "loopback";
  xrm::StreamNode stream;
  stream.endpoint = {"127.0.0.1", port};
  stream.protocol = xrm::VlessSettings{"00000000-0000-0000-0000-000000000001"};
  result.settings = stream;
  return result;
}

template<class Predicate>
void pump(uv_loop_t& loop, Predicate done) {
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(2ms);
  }
  REQUIRE(done());
}

}  // namespace

TEST_CASE("NODE-TEST TCP measures a loopback listener without launching a core", "[node-test]") {
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    Listener listener;
    bool finished = false;
    xrm::Result<xrm::NodeTestResult> result;
    xrm::NodeTestRequest request;
    request.node = node(listener.port);
    request.kind = xrm::NodeTestKind::tcp;
    request.timeout = 1s;
    auto id = tests.start(request, [&](auto value) { result = std::move(value); finished = true; });
    REQUIRE(id.has_value());
    pump(loop, [&] { return finished; });
    REQUIRE(result.has_value());
    CHECK(result->status == xrm::NodeTestStatus::success);
    CHECK(result->resolve_ms.has_value());
    CHECK(result->latency_ms.has_value());
    CHECK(result->target == "127.0.0.1:" + std::to_string(listener.port));
    CHECK(result->node_id == request.node.id);
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST pure UDP transport is not misreported as TCP failure", "[node-test]") {
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    auto target = node(443);
    auto& stream = std::get<xrm::StreamNode>(target.settings);
    stream.transport = xrm::HysteriaTransport{};
    bool finished = false;
    xrm::Result<xrm::NodeTestResult> result;
    auto id = tests.start({.node = target, .kind = xrm::NodeTestKind::tcp},
                          [&](auto value) { result = std::move(value); finished = true; });
    REQUIRE(id.has_value());
    pump(loop, [&] { return finished; });
    REQUIRE(result.has_value());
    CHECK(result->status == xrm::NodeTestStatus::not_applicable);
    CHECK_FALSE(result->latency_ms.has_value());
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST cancellation and concurrency cap are bounded", "[node-test]") {
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    Listener listener;
    std::vector<xrm::Id> ids;
    std::vector<xrm::NodeTestResult> results;
    for (int n = 0; n < 3; ++n) {
      auto id = tests.start({.node = node(listener.port), .kind = xrm::NodeTestKind::tcp},
                            [&](auto result) {
        REQUIRE(result.has_value());
        results.push_back(*result);
      });
      REQUIRE(id.has_value());
      ids.push_back(*id);
    }
    auto fourth = tests.start({.node = node(listener.port), .kind = xrm::NodeTestKind::tcp},
                              [](auto) {});
    REQUIRE_FALSE(fourth.has_value());
    CHECK(fourth.error().code == xrm::ErrorCode::busy);
    CHECK(tests.cancel(ids.front()).has_value());
    pump(loop, [&] { return results.size() == 3; });
    CHECK(std::count_if(results.begin(), results.end(), [](const auto& result) {
      return result.status == xrm::NodeTestStatus::cancelled;
    }) == 1);
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST ICMP uses a direct argv ping with distinct failure status", "[node-test]") {
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http, "/usr/bin/ping");
    bool finished = false;
    xrm::Result<xrm::NodeTestResult> result;
    auto id = tests.start({.node = node(9), .kind = xrm::NodeTestKind::icmp,
                           .timeout = 1500ms},
                          [&](auto value) { result = std::move(value); finished = true; });
    REQUIRE(id.has_value());
    pump(loop, [&] { return finished; });
    REQUIRE(result.has_value());
    if (std::filesystem::exists("/usr/bin/ping")) {
      CHECK(result->status == xrm::NodeTestStatus::success);
      CHECK(result->latency_ms.has_value());
    } else {
      CHECK(result->status == xrm::NodeTestStatus::not_applicable);
    }
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST URL never falls back to direct when isolated Xray is unavailable", "[node-test]") {
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    HttpFixture target("must-not-be-fetched");
    bool finished = false;
    xrm::Result<xrm::NodeTestResult> result;
    xrm::NodeTestRequest request;
    request.node = node(9);
    request.kind = xrm::NodeTestKind::url;
    request.target_url = "http://127.0.0.1:" + std::to_string(target.listener.port) + "/";
    auto id = tests.start(request, [&](auto value) { result = std::move(value); finished = true; });
    REQUIRE(id.has_value());
    pump(loop, [&] { return finished; });
    REQUIRE(result.has_value());
    CHECK(result->status == xrm::NodeTestStatus::not_applicable);
    CHECK(target.requests == 0);
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST independent Xray SOCKS route carries loopback URL and bounded download", "[node-test][real_core]") {
  const char* binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary)) SKIP("fixed real Xray is unavailable");
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    HttpFixture target(std::string(4096, 'm'));
    TempRoot temp;
    xrm::Node raw;
    raw.id = "freedom-test";
    raw.original_name = "isolated freedom";
    raw.settings = xrm::RawOutbound{xrm::Json{{"protocol", "freedom"}, {"settings", xrm::Json::object()}}};
    for (const auto kind : {xrm::NodeTestKind::url, xrm::NodeTestKind::download}) {
      bool finished = false;
      xrm::Result<xrm::NodeTestResult> result;
      xrm::NodeTestRequest request;
      request.node = raw;
      request.kind = kind;
      request.target_url = "http://127.0.0.1:" + std::to_string(target.listener.port) + "/marker";
      request.timeout = 3s;
      request.max_body_bytes = 4096;
      request.xray_binary = binary;
      request.temporary_root = temp.path;
      auto id = tests.start(request, [&](auto value) { result = std::move(value); finished = true; });
      REQUIRE(id.has_value());
      pump(loop, [&] { return finished; });
      REQUIRE(result.has_value());
      INFO(result->message);
      CHECK(result->status == xrm::NodeTestStatus::success);
      CHECK(result->received_bytes == 4096);
      CHECK(result->latency_ms.has_value());
      if (kind == xrm::NodeTestKind::download) CHECK(result->bytes_per_second.has_value());
      else CHECK_FALSE(result->bytes_per_second.has_value());
    }
    bool capped = false;
    xrm::Result<xrm::NodeTestResult> capped_result;
    xrm::NodeTestRequest limit_request;
    limit_request.node = raw;
    limit_request.kind = xrm::NodeTestKind::download;
    limit_request.target_url = "http://127.0.0.1:" + std::to_string(target.listener.port) + "/limit";
    limit_request.timeout = 3s;
    limit_request.max_body_bytes = 1024;
    limit_request.xray_binary = binary;
    limit_request.temporary_root = temp.path;
    auto limit_id = tests.start(limit_request, [&](auto value) {
      capped_result = std::move(value);
      capped = true;
    });
    REQUIRE(limit_id.has_value());
    pump(loop, [&] { return capped; });
    REQUIRE(capped_result.has_value());
    CHECK(capped_result->status == xrm::NodeTestStatus::failed);
    CHECK_FALSE(capped_result->bytes_per_second.has_value());
    CHECK(target.requests == 3);
    CHECK(std::filesystem::is_empty(temp.path / "runtime" / "preview"));
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST typed VLESS travels through an independent test Xray without changing production", "[node-test][real_core]") {
  const char* binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary)) SKIP("fixed real Xray is unavailable");
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    HttpFixture target("NODE-TEST-VLESS-MARKER");
    TempRoot temp;
    const auto vless_port = free_port();
    const std::string uuid = "00000000-0000-4000-8000-000000000001";
    xrm::Json peer = {
      {"log", {{"loglevel", "warning"}}},
      {"inbounds", xrm::Json::array({xrm::Json{
          {"listen", "127.0.0.1"}, {"port", vless_port}, {"protocol", "vless"},
          {"settings", {{"clients", xrm::Json::array({xrm::Json{{"id", uuid}}})},
                        {"decryption", "none"}}}}})},
      {"outbounds", xrm::Json::array({xrm::Json{
          {"protocol", "freedom"}, {"settings", {{"finalRules", xrm::Json::array({
              xrm::Json{{"action", "allow"}, {"network", "tcp"},
                        {"port", std::to_string(target.listener.port)},
                        {"ip", xrm::Json::array({"127.0.0.0/8"})}}
          })}}}}})}
    };
    const auto peer_config = temp.path / "peer.json";
    {
      std::ofstream file(peer_config);
      REQUIRE(file.good());
      file << peer.dump(2) << '\n';
    }
    bool peer_exited = false;
    auto peer_id = process.spawn({binary, {"run", "-c", peer_config.string()}, temp.path, {}},
                                 {.exited = [&](xrm::ProcessId, xrm::ProcessExit) { peer_exited = true; }});
    REQUIRE(peer_id.has_value());
    pump(loop, [&] { return port_open(vless_port) || peer_exited; });
    REQUIRE_FALSE(peer_exited);

    xrm::NodeTestRequest request;
    request.node = node(vless_port);
    request.node.id = "typed-vless-test";
    request.node.settings = xrm::StreamNode{.endpoint = {"127.0.0.1", vless_port},
        .protocol = xrm::VlessSettings{uuid}};
    request.kind = xrm::NodeTestKind::url;
    request.target_url = "http://127.0.0.1:" + std::to_string(target.listener.port) + "/vless";
    request.timeout = 3s;
    request.xray_binary = binary;
    request.temporary_root = temp.path;
    bool finished = false;
    xrm::Result<xrm::NodeTestResult> result;
    auto test_id = tests.start(request, [&](auto value) { result = std::move(value); finished = true; });
    REQUIRE(test_id.has_value());
    pump(loop, [&] { return finished; });
    REQUIRE(result.has_value());
    INFO(result->message);
    CHECK(result->status == xrm::NodeTestStatus::success);
    CHECK(result->received_bytes == std::string("NODE-TEST-VLESS-MARKER").size());
    CHECK(target.requests == 1);
    CHECK(std::filesystem::is_empty(temp.path / "runtime" / "preview"));
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    REQUIRE(process.terminate(*peer_id, 500ms).has_value());
    pump(loop, [&] { return peer_exited; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("NODE-TEST cancelling an active proxied download reaps the test core and files", "[node-test][real_core]") {
  const char* binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary)) SKIP("fixed real Xray is unavailable");
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    xrm::ProcessService process(loop);
    xrm::HttpClient http(loop);
    xrm::NodeTestService tests(loop, process, http);
    HttpFixture target(std::string(1024, 'x'), 500ms);
    TempRoot temp;
    xrm::Node raw;
    raw.id = "cancel-test";
    raw.settings = xrm::RawOutbound{xrm::Json{{"protocol", "freedom"},
                                             {"settings", xrm::Json::object()}}};
    xrm::NodeTestRequest request;
    request.node = raw;
    request.kind = xrm::NodeTestKind::download;
    request.target_url = "http://127.0.0.1:" + std::to_string(target.listener.port) + "/slow";
    request.timeout = 3s;
    request.xray_binary = binary;
    request.temporary_root = temp.path;
    bool finished = false;
    xrm::Result<xrm::NodeTestResult> result;
    auto id = tests.start(request, [&](auto value) { result = std::move(value); finished = true; });
    REQUIRE(id.has_value());
    pump(loop, [&] { return target.requests == 1; });
    REQUIRE(tests.cancel(*id).has_value());
    pump(loop, [&] { return finished; });
    REQUIRE(result.has_value());
    CHECK(result->status == xrm::NodeTestStatus::cancelled);
    CHECK(std::filesystem::is_empty(temp.path / "runtime" / "preview"));
    bool drained = false;
    tests.shutdown([&] { drained = true; });
    pump(loop, [&] { return drained; });
    http.shutdown([] {});
    process.shutdown([] {});
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  CHECK(uv_loop_close(&loop) == 0);
}
