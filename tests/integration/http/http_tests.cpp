/// @file
/// @brief Checks HTTP routing, headers, limits, cancellation, and shutdown.

#include <catch2/catch_test_macros.hpp>

#include "infra/http.hpp"

#include <uv.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Loop {
  uv_loop_t value{};
  Loop() { REQUIRE(uv_loop_init(&value) == 0); }
  ~Loop() { uv_loop_close(&value); }
};

class HttpFixture {
public:
  HttpFixture() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener_ >= 0);
    int reuse = 1;
    REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(listener_, 16) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0);
    port_ = ntohs(address.sin_port);
    accept_thread_ = std::thread([this] { accept_loop(); });
  }

  ~HttpFixture() {
    stopping_ = true;
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    if (accept_thread_.joinable())
      accept_thread_.join();
    std::vector<std::thread> handlers;
    {
      std::lock_guard lock(mutex_);
      handlers.swap(handlers_);
    }
    for (auto &handler : handlers)
      if (handler.joinable())
        handler.join();
  }

  std::string url(std::string_view path = "/") const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }
  int port() const { return port_; }
  int max_active() const { return max_active_; }
  int active() const { return active_; }
  bool saw_user_agent() const { return saw_user_agent_; }
  bool saw_custom_header() const { return saw_custom_header_; }
  bool saw_proxy_absolute_uri() const { return saw_proxy_absolute_uri_; }
  bool got_bad_proxy_direct_request() const {
    return got_bad_proxy_direct_request_;
  }

private:
  static void send_all(int fd, const std::string &data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto n =
          ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
      if (n <= 0)
        return;
      offset += static_cast<std::size_t>(n);
    }
  }

  void accept_loop() {
    while (!stopping_) {
      const int fd = ::accept(listener_, nullptr, nullptr);
      if (fd < 0)
        break;
      std::lock_guard lock(mutex_);
      handlers_.emplace_back([this, fd] { handle(fd); });
    }
  }

  void handle(int fd) {
    const int active = ++active_;
    int high = max_active_.load();
    while (active > high && !max_active_.compare_exchange_weak(high, active)) {
    }
    timeval timeout{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < 16384) {
      const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0)
        break;
      request.append(buffer, static_cast<std::size_t>(n));
    }
    if (request.find("User-Agent: xrm-http-test/1\r\n") != std::string::npos)
      saw_user_agent_ = true;
    if (request.find("X-Test-Header: source-value\r\n") != std::string::npos)
      saw_custom_header_ = true;
    if (request.find("GET http://127.0.0.1:") == 0)
      saw_proxy_absolute_uri_ = true;
    if (request.find("GET /should-not-direct ") == 0)
      got_bad_proxy_direct_request_ = true;
    std::string status = "200 OK";
    std::string body = "fixture-body";
    std::string extra_headers;
    auto delay = 0ms;
    if (request.find(" /slow ") != std::string::npos)
      delay = 350ms;
    if (request.find(" /big ") != std::string::npos)
      body = std::string(4096, 'x');
    if (request.find(" /missing ") != std::string::npos)
      status = "404 Not Found";
    if (request.find(" /server-error ") != std::string::npos)
      status = "503 Service Unavailable";
    if (request.find(" /not-modified ") != std::string::npos) {
      status = "304 Not Modified";
      body.clear();
    }
    if (request.find(" /redirect ") != std::string::npos) {
      status = "302 Found";
      extra_headers = "Location: " + url("/final") + "\r\n";
      body.clear();
    }
    if (request.find(" /final ") != std::string::npos)
      body = "redirect-final";
    std::this_thread::sleep_for(delay);
    const std::string response =
        "HTTP/1.1 " + status + "\r\n" + extra_headers +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "X-Fixture: kept-Value\r\nConnection: close\r\n\r\n" + body;
    send_all(fd, response);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    --active_;
  }

  int listener_ = -1;
  int port_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<int> active_{0};
  std::atomic<int> max_active_{0};
  std::atomic<bool> saw_user_agent_{false};
  std::atomic<bool> saw_custom_header_{false};
  std::atomic<bool> saw_proxy_absolute_uri_{false};
  std::atomic<bool> got_bad_proxy_direct_request_{false};
  std::mutex mutex_;
  std::thread accept_thread_;
  std::vector<std::thread> handlers_;
};

class SelfSignedTlsFixture {
public:
  SelfSignedTlsFixture() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("xrm-http-tls-" + std::to_string(stamp));
    std::filesystem::create_directories(directory_);
    const auto certificate = directory_ / "cert.pem";
    const auto key = directory_ / "key.pem";
    const pid_t generator = ::fork();
    REQUIRE(generator >= 0);
    if (generator == 0) {
      const int null_fd = ::open("/dev/null", O_WRONLY);
      if (null_fd >= 0) {
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
      }
      ::execlp("openssl", "openssl", "req", "-x509", "-newkey", "rsa:2048",
               "-nodes", "-batch", "-keyout", key.c_str(), "-out",
               certificate.c_str(), "-subj", "/CN=localhost", "-days", "1",
               nullptr);
      _exit(127);
    }
    int result = 0;
    REQUIRE(::waitpid(generator, &result, 0) == generator);
    REQUIRE(WIFEXITED(result));
    REQUIRE(WEXITSTATUS(result) == 0);
    port_ = reserve_port();
    server_ = ::fork();
    REQUIRE(server_ >= 0);
    if (server_ == 0) {
      const int null_fd = ::open("/dev/null", O_WRONLY);
      if (null_fd >= 0) {
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
      }
      const auto address = "127.0.0.1:" + std::to_string(port_);
      ::execlp("openssl", "openssl", "s_server", "-quiet", "-www", "-accept",
               address.c_str(), "-cert", certificate.c_str(), "-key",
               key.c_str(), nullptr);
      _exit(127);
    }
    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!ready && std::chrono::steady_clock::now() < deadline) {
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(port_);
      if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0)
        ready = true;
      ::close(fd);
      if (!ready)
        std::this_thread::sleep_for(10ms);
    }
    if (!ready) {
      ::kill(server_, SIGTERM);
      int status = 0;
      ::waitpid(server_, &status, 0);
      server_ = -1;
    }
    REQUIRE(ready);
  }

  ~SelfSignedTlsFixture() {
    if (server_ > 0) {
      ::kill(server_, SIGTERM);
      int status = 0;
      ::waitpid(server_, &status, 0);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  std::string url() const {
    return "https://127.0.0.1:" + std::to_string(port_) + "/";
  }

private:
  static int reserve_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0);
    const int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
  }

  std::filesystem::path directory_;
  int port_ = 0;
  pid_t server_ = -1;
};

class Socks5Fixture {
public:
  Socks5Fixture() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener_ >= 0);
    int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(listener_, 4) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { serve_one(); });
  }

  ~Socks5Fixture() {
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    if (thread_.joinable())
      thread_.join();
  }
  int port() const { return port_; }
  bool connected() const { return connected_; }

private:
  static bool read_exact(int fd, void *output, std::size_t length) {
    auto *bytes = static_cast<char *>(output);
    std::size_t consumed = 0;
    while (consumed < length) {
      const auto n = ::recv(fd, bytes + consumed, length - consumed, 0);
      if (n <= 0)
        return false;
      consumed += static_cast<std::size_t>(n);
    }
    return true;
  }

  static bool write_all(int fd, const void *input, std::size_t length) {
    const auto *bytes = static_cast<const char *>(input);
    std::size_t consumed = 0;
    while (consumed < length) {
      const auto n =
          ::send(fd, bytes + consumed, length - consumed, MSG_NOSIGNAL);
      if (n <= 0)
        return false;
      consumed += static_cast<std::size_t>(n);
    }
    return true;
  }

  void serve_one() {
    const int client = ::accept(listener_, nullptr, nullptr);
    if (client < 0)
      return;
    unsigned char greeting[2]{};
    if (!read_exact(client, greeting, sizeof(greeting)) || greeting[0] != 5) {
      ::close(client);
      return;
    }
    std::vector<unsigned char> methods(greeting[1]);
    if (!read_exact(client, methods.data(), methods.size())) {
      ::close(client);
      return;
    }
    const unsigned char method_selection[]{5, 0};
    if (!write_all(client, method_selection, sizeof(method_selection))) {
      ::close(client);
      return;
    }
    unsigned char request[4]{};
    if (!read_exact(client, request, sizeof(request)) || request[0] != 5 ||
        request[1] != 1) {
      ::close(client);
      return;
    }
    std::string host;
    if (request[3] == 1) {
      unsigned char address[4]{};
      if (!read_exact(client, address, sizeof(address))) {
        ::close(client);
        return;
      }
      char text[INET_ADDRSTRLEN]{};
      ::inet_ntop(AF_INET, address, text, sizeof(text));
      host = text;
    } else if (request[3] == 3) {
      unsigned char count = 0;
      if (!read_exact(client, &count, 1)) {
        ::close(client);
        return;
      }
      host.resize(count);
      if (!read_exact(client, host.data(), host.size())) {
        ::close(client);
        return;
      }
    } else if (request[3] == 4) {
      unsigned char address[16]{};
      if (!read_exact(client, address, sizeof(address))) {
        ::close(client);
        return;
      }
      char text[INET6_ADDRSTRLEN]{};
      ::inet_ntop(AF_INET6, address, text, sizeof(text));
      host = text;
    } else {
      ::close(client);
      return;
    }
    unsigned char port_bytes[2]{};
    if (!read_exact(client, port_bytes, sizeof(port_bytes))) {
      ::close(client);
      return;
    }
    const auto target_port =
        static_cast<unsigned>((port_bytes[0] << 8) | port_bytes[1]);
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *resolved = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(target_port).c_str(), &hints,
                      &resolved) != 0) {
      ::close(client);
      return;
    }
    int target = -1;
    for (auto *address = resolved; address; address = address->ai_next) {
      target = ::socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);
      if (target >= 0 &&
          ::connect(target, address->ai_addr, address->ai_addrlen) == 0)
        break;
      if (target >= 0)
        ::close(target);
      target = -1;
    }
    ::freeaddrinfo(resolved);
    if (target < 0) {
      const unsigned char failure[]{5, 5, 0, 1, 0, 0, 0, 0, 0, 0};
      write_all(client, failure, sizeof(failure));
      ::close(client);
      return;
    }
    const unsigned char success[]{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    if (!write_all(client, success, sizeof(success))) {
      ::close(target);
      ::close(client);
      return;
    }
    connected_ = true;
    pollfd sockets[2]{{client, POLLIN, 0}, {target, POLLIN, 0}};
    bool open = true;
    while (open) {
      const int ready = ::poll(sockets, 2, 2000);
      if (ready <= 0)
        break;
      for (int i = 0; i < 2; ++i) {
        if (!(sockets[i].revents & (POLLIN | POLLHUP | POLLERR)))
          continue;
        char bytes[4096];
        const auto count = ::recv(sockets[i].fd, bytes, sizeof(bytes), 0);
        if (count <= 0) {
          open = false;
          break;
        }
        const int other = sockets[1 - i].fd;
        if (!write_all(other, bytes, static_cast<std::size_t>(count))) {
          open = false;
          break;
        }
      }
    }
    ::shutdown(client, SHUT_RDWR);
    ::shutdown(target, SHUT_RDWR);
    ::close(client);
    ::close(target);
  }

  int listener_ = -1;
  int port_ = 0;
  std::atomic<bool> connected_{false};
  std::thread thread_;
};

template <class T>
xrm::Result<T> fetch_sync(xrm::HttpClient &client, uv_loop_t &loop,
                          xrm::HttpRequest request) {
  std::optional<xrm::Result<T>> result;
  auto accepted = client.fetch(std::move(request), [&](auto value) {
    result.emplace(std::move(value));
  });
  REQUIRE(accepted);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!result && std::chrono::steady_clock::now() < deadline) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(result.has_value());
  return std::move(*result);
}

} // namespace

TEST_CASE("HTTP-01 direct requests, headers, and explicit proxy",
          "[HTTP-01][real_http]") {
  HttpFixture fixture;
  Loop loop;
  xrm::HttpClient client(loop.value);
  xrm::HttpRequest request;
  request.url = fixture.url("/headers");
  request.user_agent = "xrm-http-test/1";
  request.headers.emplace("X-Test-Header", "source-value");
  auto response =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(request));
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(response->body == "fixture-body");
  CHECK(response->headers.contains("x-fixture"));
  CHECK(response->headers.at("x-fixture") == " kept-Value");
  CHECK(fixture.saw_user_agent());
  CHECK(fixture.saw_custom_header());

  // The local fixture acts as an explicit HTTP proxy. A failed explicit proxy
  // must remain a failure and must never fall back to a direct request.
  xrm::HttpRequest proxied;
  proxied.url = fixture.url("/via-proxy");
  proxied.route.proxy_url = fixture.url();
  auto proxy_response =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(proxied));
  REQUIRE(proxy_response);
  CHECK(proxy_response->status == 200);
  CHECK(fixture.saw_proxy_absolute_uri());

  Socks5Fixture socks_proxy;
  xrm::HttpRequest socks_request;
  socks_request.url = fixture.url("/through-socks");
  socks_request.route.proxy_url =
      "socks5h://127.0.0.1:" + std::to_string(socks_proxy.port());
  auto socks_response = fetch_sync<xrm::HttpResponse>(client, loop.value,
                                                      std::move(socks_request));
  REQUIRE(socks_response);
  CHECK(socks_response->status == 200);
  CHECK(socks_proxy.connected());

  xrm::HttpRequest bad_proxy;
  bad_proxy.url = fixture.url("/should-not-direct");
  bad_proxy.route.proxy_url = "http://127.0.0.1:1";
  const auto failed =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(bad_proxy));
  CHECK_FALSE(failed);
  if (!failed) {
    CHECK(failed.error().message.find('\0') == std::string::npos);
    CHECK(failed.error().message.size() < 512);
  }
  CHECK_FALSE(fixture.got_bad_proxy_direct_request());
  xrm::HttpRequest unsupported_proxy;
  unsupported_proxy.url = fixture.url();
  unsupported_proxy.route.proxy_url = "ftp://127.0.0.1:1";
  const auto rejected_proxy =
      client.fetch(std::move(unsupported_proxy), [](auto) {});
  REQUIRE_FALSE(rejected_proxy);
  CHECK(rejected_proxy.error().code == xrm::ErrorCode::invalid_argument);
  xrm::HttpRequest invalid_header;
  invalid_header.url = fixture.url();
  invalid_header.headers.emplace("X-Test", "value\r\nInjected: yes");
  const auto rejected_header =
      client.fetch(std::move(invalid_header), [](auto) {});
  REQUIRE_FALSE(rejected_header);
  CHECK(rejected_header.error().code == xrm::ErrorCode::invalid_argument);
  client.shutdown([] {});
  uv_run(&loop.value, UV_RUN_DEFAULT);

  SelfSignedTlsFixture tls;
  Loop tls_loop;
  xrm::HttpClient tls_client(tls_loop.value);
  xrm::HttpRequest tls_request;
  tls_request.url = tls.url();
  auto rejected_certificate = fetch_sync<xrm::HttpResponse>(
      tls_client, tls_loop.value, std::move(tls_request));
  CHECK_FALSE(rejected_certificate);
  CHECK(rejected_certificate.error().code == xrm::ErrorCode::network_error);
  tls_client.shutdown([] {});
  uv_run(&tls_loop.value, UV_RUN_DEFAULT);
}

TEST_CASE(
    "HTTP-02 enforces timeout, size, cancellation, concurrency, and shutdown",
    "[HTTP-02][real_http]") {
  HttpFixture fixture;
  Loop loop;
  xrm::HttpClient client(loop.value);
  xrm::HttpRequest slow;
  slow.url = fixture.url("/slow");
  slow.timeout = 40ms;
  auto timed_out =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(slow));
  REQUIRE_FALSE(timed_out);
  CHECK(timed_out.error().code == xrm::ErrorCode::timed_out);
  const auto idle_deadline = std::chrono::steady_clock::now() + 1s;
  while (fixture.active() != 0 &&
         std::chrono::steady_clock::now() < idle_deadline)
    std::this_thread::sleep_for(1ms);
  REQUIRE(fixture.active() == 0);

  xrm::HttpRequest big;
  big.url = fixture.url("/big");
  big.max_body_bytes = 32;
  auto too_large =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(big));
  REQUIRE_FALSE(too_large);
  CHECK(too_large.error().code == xrm::ErrorCode::too_large);
  const auto idle_after_big = std::chrono::steady_clock::now() + 1s;
  while (fixture.active() != 0 &&
         std::chrono::steady_clock::now() < idle_after_big)
    std::this_thread::sleep_for(1ms);
  REQUIRE(fixture.active() == 0);

  std::vector<xrm::Id> ids;
  int completed = 0;
  int cancelled_callbacks = 0;
  for (int i = 0; i < 4; ++i) {
    xrm::HttpRequest request;
    request.url = fixture.url("/slow");
    auto accepted = client.fetch(std::move(request), [&, i](auto result) {
      ++completed;
      if (i == 0 && !result && result.error().code == xrm::ErrorCode::cancelled)
        ++cancelled_callbacks;
    });
    REQUIRE(accepted);
    ids.push_back(*accepted);
  }
  bool loop_responded = false;
  uv_timer_t loop_probe{};
  REQUIRE(uv_timer_init(&loop.value, &loop_probe) == 0);
  loop_probe.data = &loop_responded;
  REQUIRE(uv_timer_start(
              &loop_probe,
              [](uv_timer_t *timer) {
                *static_cast<bool *>(timer->data) = true;
                uv_timer_stop(timer);
                uv_close(reinterpret_cast<uv_handle_t *>(timer), nullptr);
              },
              1, 0) == 0);
  const auto start_deadline = std::chrono::steady_clock::now() + 1s;
  while (fixture.max_active() < 3 &&
         std::chrono::steady_clock::now() < start_deadline) {
    uv_run(&loop.value, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(1ms);
  }
  CHECK(fixture.max_active() == 3);
  CHECK(loop_responded);
  auto cancelled = client.cancel(ids.front());
  REQUIRE(cancelled);
  bool drained = false;
  client.shutdown([&] { drained = true; });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!drained && std::chrono::steady_clock::now() < deadline) {
    uv_run(&loop.value, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(1ms);
  }
  CHECK(drained);
  CHECK(completed == 4);
  CHECK(cancelled_callbacks == 1);
  CHECK(loop_responded);
  CHECK(fixture.max_active() == 3);
  CHECK_FALSE(client.fetch({}, [](auto) {}));
}

TEST_CASE("HTTP-03 preserves HTTP status and handles redirects",
          "[HTTP-03][real_http]") {
  HttpFixture fixture;
  Loop loop;
  xrm::HttpClient client(loop.value);
  for (const auto &[path, expected] : std::vector<std::pair<std::string, long>>{
           {"/missing", 404}, {"/server-error", 503}, {"/not-modified", 304}}) {
    xrm::HttpRequest request;
    request.url = fixture.url(path);
    auto response =
        fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(request));
    REQUIRE(response);
    CHECK(response->status == expected);
  }
  xrm::HttpRequest empty_only;
  empty_only.url = fixture.url("/not-modified");
  empty_only.max_body_bytes = 0;
  auto empty_response =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(empty_only));
  REQUIRE(empty_response);
  CHECK(empty_response->body.empty());
  xrm::HttpRequest redirect;
  redirect.url = fixture.url("/redirect");
  auto response =
      fetch_sync<xrm::HttpResponse>(client, loop.value, std::move(redirect));
  REQUIRE(response);
  CHECK(response->status == 200);
  CHECK(response->body == "redirect-final");
  client.shutdown([] {});
  uv_run(&loop.value, UV_RUN_DEFAULT);
}
