/// @file
/// @brief Checks last-good geodata cache retention and failed/cancelled
/// updates.

#include "geodata/cache.hpp"
#include "infra/http.hpp"
#include "infra/process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <uv.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace xrm;

namespace {

std::string field(int number, std::string_view bytes) {
  REQUIRE(bytes.size() < 128);
  return std::string{static_cast<char>((number << 3) | 2),
                     static_cast<char>(bytes.size())} +
         std::string(bytes);
}

std::string geoip(std::string_view ip) {
  const auto cidr = field(1, ip) + std::string{"\x10\x08", 2};
  return field(1, field(1, "XRM-TEST") + field(2, cidr));
}

class HttpFixture {
public:
  explicit HttpFixture(std::string body) : body_(std::move(body)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener_ >= 0);
    int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(listener_, 8) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0);
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] {
      while (!stopped_) {
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0)
          break;
        char buffer[1024];
        (void)::recv(client, buffer, sizeof(buffer), 0);
        std::string body;
        int status;
        {
          std::lock_guard lock(mutex_);
          body = body_;
          status = status_;
        }
        const auto text = "HTTP/1.1 " + std::to_string(status) +
                          (status == 200 ? " OK\r\n" : " Error\r\n") +
                          "Content-Length: " + std::to_string(body.size()) +
                          "\r\nConnection: close\r\n\r\n" + body;
        (void)::send(client, text.data(), text.size(), MSG_NOSIGNAL);
        ::close(client);
      }
    });
  }
  ~HttpFixture() {
    stopped_ = true;
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    worker_.join();
  }
  void respond(int status, std::string body) {
    std::lock_guard lock(mutex_);
    status_ = status;
    body_ = std::move(body);
  }
  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/asset.dat";
  }

private:
  int listener_ = -1;
  int port_ = 0;
  std::atomic<bool> stopped_{false};
  std::thread worker_;
  std::mutex mutex_;
  int status_ = 200;
  std::string body_;
};

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto base =
        std::filesystem::temp_directory_path() / "xrm-cache-XXXXXX";
    auto text = base.string();
    std::vector<char> writable(text.begin(), text.end());
    writable.push_back('\0');
    if (char *created = ::mkdtemp(writable.data()))
      path = created;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

template <typename T>
Result<T> wait_for(uv_loop_t &loop,
                   const std::function<Result<Id>(Completion<T>)> &start) {
  std::optional<Result<T>> result;
  auto started =
      start([&](Result<T> response) { result = std::move(response); });
  if (!started)
    return std::unexpected(started.error());
  const auto until = std::chrono::steady_clock::now() + 8s;
  while (!result && std::chrono::steady_clock::now() < until) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(result);
  return std::move(*result);
}

} // namespace

TEST_CASE("P5 shared cache keeps immutable last-good on bad content and HTTP "
          "failure") {
  const char *core = std::getenv("XRM_TEST_XRAY");
  if (!core || !std::filesystem::exists(core))
    SKIP("XRM_TEST_XRAY is unavailable");
  TemporaryDirectory temp;
  REQUIRE_FALSE(temp.path.empty());
  HttpFixture fixture(geoip(std::string{"\x7f\0\0\0", 4}));
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    GeodataCache cache(http, processes, loop, temp.path / "cache");
    GeodataAcquireRequest request;
    request.resource_id = "asset-one";
    request.kind = GeoAssetKind::geoip;
    request.file_name = "geoip.dat";
    request.url = fixture.url();
    request.required_labels = {"xrm-test"};
    request.validator_binary = core;

    auto fetch = [&] {
      return wait_for<GeodataAssetVersion>(loop, [&](auto completion) {
        return cache.acquire(request, std::move(completion));
      });
    };
    auto first = fetch();
    REQUIRE(first);
    CHECK(std::filesystem::exists(first->path));
    CHECK(first->labels.contains("xrm-test"));
    const auto first_path = first->path;

    fixture.respond(200, "not a protobuf dat file");
    auto bad = fetch();
    CHECK_FALSE(bad);
    CHECK(std::filesystem::exists(first_path));

    fixture.respond(503, "failure");
    auto unavailable = fetch();
    CHECK_FALSE(unavailable);
    CHECK(std::filesystem::exists(first_path));

    fixture.respond(200, geoip(std::string{"\x0a\0\0\0", 4}));
    auto second = fetch();
    REQUIRE(second);
    CHECK(second->path != first_path);
    CHECK(std::filesystem::exists(first_path));
    CHECK(std::filesystem::exists(second->path));
    auto repeated = fetch();
    REQUIRE(repeated);
    CHECK(repeated->path == second->path);
    for (const auto &file :
         std::filesystem::directory_iterator(temp.path / "cache"))
      CHECK_FALSE(file.path().filename().string().starts_with(".staging-"));
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE(
    "P5 geodata cache cancellation and body limit never publish a version") {
  const char *core = std::getenv("XRM_TEST_XRAY");
  if (!core || !std::filesystem::exists(core))
    SKIP("XRM_TEST_XRAY is unavailable");
  TemporaryDirectory temp;
  REQUIRE_FALSE(temp.path.empty());
  HttpFixture fixture(geoip(std::string{"\x7f\0\0\0", 4}));
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    GeodataCache cache(http, processes, loop, temp.path / "cache");
    GeodataAcquireRequest request;
    request.resource_id = "asset-cancel";
    request.kind = GeoAssetKind::geoip;
    request.file_name = "geoip.dat";
    request.url = fixture.url();
    request.validator_binary = core;
    std::optional<Result<GeodataAssetVersion>> result;
    auto operation =
        cache.acquire(request, [&](auto value) { result = std::move(value); });
    REQUIRE(operation);
    CHECK_FALSE(
        cache.acquire(request, [](auto) {})); // one request per resource
    REQUIRE(cache.cancel(*operation));
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!result && std::chrono::steady_clock::now() < deadline) {
      uv_run(&loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(result);
    CHECK_FALSE(*result);
    CHECK(result->error().code == ErrorCode::cancelled);
    CHECK_FALSE(std::filesystem::exists(temp.path / "cache" / "asset-cancel"));

    request.max_body_bytes = 4;
    auto too_large = wait_for<GeodataAssetVersion>(loop, [&](auto completion) {
      return cache.acquire(request, std::move(completion));
    });
    REQUIRE_FALSE(too_large);
    CHECK(too_large.error().code == ErrorCode::too_large);
    CHECK_FALSE(std::filesystem::exists(temp.path / "cache" / "asset-cancel"));
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("P5 geodata cache bounds concurrent queued acquisitions") {
  const char *core = std::getenv("XRM_TEST_XRAY");
  if (!core || !std::filesystem::exists(core))
    SKIP("XRM_TEST_XRAY is unavailable");
  TemporaryDirectory temp;
  REQUIRE_FALSE(temp.path.empty());
  HttpFixture fixture(geoip(std::string{"\x7f\0\0\0", 4}));
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    GeodataCache cache(http, processes, loop, temp.path / "cache");
    GeodataAcquireRequest request;
    request.kind = GeoAssetKind::geoip;
    request.file_name = "geoip.dat";
    request.url = fixture.url();
    request.validator_binary = core;
    std::vector<Id> started;
    int completed = 0;
    for (int index = 0; index < 3; ++index) {
      request.resource_id = "asset-" + std::to_string(index);
      auto result = cache.acquire(request, [&](auto) { ++completed; });
      REQUIRE(result);
      started.push_back(*result);
    }
    request.resource_id = "asset-over-limit";
    auto fourth = cache.acquire(request, [&](auto) { ++completed; });
    REQUIRE_FALSE(fourth);
    CHECK(fourth.error().code == ErrorCode::busy);
    for (const auto &id : started)
      REQUIRE(cache.cancel(id));
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (completed != 3 && std::chrono::steady_clock::now() < deadline) {
      uv_run(&loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(1ms);
    }
    CHECK(completed == 3);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}
