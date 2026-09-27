/// @file
/// @brief Checks release selection, verification, and managed core
/// installation.

#include <catch2/catch_test_macros.hpp>

#include "infra/http.hpp"
#include "infra/process.hpp"
#include "xray/installer.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <uv.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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
  ~Loop() { (void)uv_loop_close(&value); }
};

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-installer-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

class LocalReleaseServer {
public:
  LocalReleaseServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int reuse = 1;
    REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(fd_, 16) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0);
    base_url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    thread_ = std::thread([this] { run(); });
  }

  ~LocalReleaseServer() {
    stopping_ = true;
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    if (thread_.joinable())
      thread_.join();
  }

  std::string api_url() const { return base_url_ + "/releases"; }
  std::string asset_url() const { return base_url_ + "/xray.zip"; }
  void set(std::string releases, std::string archive, long api_status = 200) {
    std::lock_guard lock(mutex_);
    releases_ = std::move(releases);
    archive_ = std::move(archive);
    api_status_ = api_status;
  }
  std::size_t api_requests() const { return api_requests_; }
  std::size_t asset_requests() const { return asset_requests_; }

private:
  static void send_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::send(fd, bytes.data() + offset,
                                bytes.size() - offset, MSG_NOSIGNAL);
      if (count <= 0)
        return;
      offset += static_cast<std::size_t>(count);
    }
  }

  void run() {
    while (!stopping_) {
      pollfd event{fd_, POLLIN, 0};
      if (::poll(&event, 1, 100) <= 0)
        continue;
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0)
        continue;
      timeval timeout{2, 0};
      (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
      std::string request;
      std::array<char, 1024> buffer{};
      while (request.find("\r\n\r\n") == std::string::npos &&
             request.size() < 16384) {
        const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
        if (count <= 0)
          break;
        request.append(buffer.data(), static_cast<std::size_t>(count));
      }
      std::string body;
      long status = 200;
      {
        std::lock_guard lock(mutex_);
        if (request.starts_with("GET /releases")) {
          ++api_requests_;
          body = releases_;
          status = api_status_;
        } else if (request.starts_with("GET /xray.zip ")) {
          ++asset_requests_;
          body = archive_;
        } else {
          status = 404;
        }
      }
      const auto status_text = status == 200   ? "OK"
                               : status == 404 ? "Not Found"
                                               : "Server Error";
      const auto headers =
          "HTTP/1.1 " + std::to_string(status) + " " + status_text +
          "\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nConnection: close\r\n\r\n";
      send_all(client, headers);
      send_all(client, body);
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }

  int fd_ = -1;
  std::string base_url_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mutex_;
  std::string releases_;
  std::string archive_;
  long api_status_ = 200;
  std::atomic<std::size_t> api_requests_{0};
  std::atomic<std::size_t> asset_requests_{0};
  std::thread thread_;
};

std::string sha256(std::string_view bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  REQUIRE(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size,
                     EVP_sha256(), nullptr) == 1);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (unsigned int i = 0; i < size; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 0x0f]);
  }
  return result;
}

std::string
zip_with_core(std::string_view script,
              std::optional<std::string_view> extra_path = std::nullopt) {
  std::vector<char> storage(1024 * 1024);
  size_t used = 0;
  archive *writer = archive_write_new();
  REQUIRE(writer != nullptr);
  REQUIRE(archive_write_set_format_zip(writer) == ARCHIVE_OK);
  REQUIRE(archive_write_open_memory(writer, storage.data(), storage.size(),
                                    &used) == ARCHIVE_OK);
  auto append = [&](std::string_view name, std::string_view contents,
                    mode_t mode) {
    archive_entry *entry = archive_entry_new();
    REQUIRE(entry != nullptr);
    archive_entry_set_pathname(entry, std::string(name).c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, mode);
    archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
    REQUIRE(archive_write_header(writer, entry) == ARCHIVE_OK);
    REQUIRE(archive_write_data(writer, contents.data(), contents.size()) ==
            static_cast<la_ssize_t>(contents.size()));
    archive_entry_free(entry);
  };
  if (extra_path)
    append(*extra_path, "escape", 0644);
  append("xray", script, 0755);
  REQUIRE(archive_write_close(writer) == ARCHIVE_OK);
  REQUIRE(archive_write_free(writer) == ARCHIVE_OK);
  return std::string(storage.data(), used);
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string metadata(const LocalReleaseServer &server,
                     std::string_view checksum, bool include_digest = true,
                     bool add_arch_assets = true, bool external_asset = false) {
#ifndef XRM_RELEASE_FIXTURE_ROOT
#define XRM_RELEASE_FIXTURE_ROOT "tests/fixtures/releases"
#endif
  std::ifstream input(std::string(XRM_RELEASE_FIXTURE_ROOT) + "/releases.json");
  REQUIRE(input.good());
  std::string body{std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>()};
  const auto replace_all = [&](std::string_view needle,
                               std::string_view replacement) {
    std::size_t offset = 0;
    while ((offset = body.find(needle, offset)) != std::string::npos) {
      body.replace(offset, needle.size(), replacement);
      offset += replacement.size();
    }
  };
  replace_all("{asset_url}", server.asset_url());
  replace_all("{sha256}", checksum);
  if (external_asset) {
    const auto position = body.find(server.asset_url());
    REQUIRE(position != std::string::npos);
    body.replace(position, server.asset_url().size(),
                 "https://downloads.example.net/Xray-linux-64.zip");
  }
  if (!include_digest)
    replace_all("\"digest\": \"sha256:" + std::string(checksum) + "\"",
                "\"digest\": null");
  if (!add_arch_assets) {
    const auto first =
        body.find("      {\n        \"name\": \"Xray-linux-arm64-v8a.zip\"");
    REQUIRE(first != std::string::npos);
    const auto last = body.find("      }\n", first);
    REQUIRE(last != std::string::npos);
    body.erase(first, last + 8 - first);
  }
  return body;
}

struct Services {
  Loop loop;
  xrm::HttpClient http{loop.value};
  xrm::ProcessService process{loop.value};
  ~Services() {
    bool http_drained = false;
    bool process_drained = false;
    http.shutdown([&] { http_drained = true; });
    process.shutdown([&] { process_drained = true; });
    while (!http_drained || !process_drained)
      (void)uv_run(&loop.value, UV_RUN_ONCE);
  }
};

template <class T, class Start>
xrm::Result<T> wait_result(uv_loop_t &loop, Start &&start) {
  std::optional<xrm::Result<T>> result;
  const auto started =
      start([&](xrm::Result<T> value) { result.emplace(std::move(value)); });
  REQUIRE(started.has_value());
  while (!result)
    (void)uv_run(&loop, UV_RUN_ONCE);
  return std::move(*result);
}

xrm::CoreRelease find_release(const std::vector<xrm::CoreRelease> &releases,
                              std::string_view tag) {
  const auto found =
      std::find_if(releases.begin(), releases.end(),
                   [&](const auto &release) { return release.tag == tag; });
  REQUIRE(found != releases.end());
  return *found;
}

xrm::InstallRequest request_for(const xrm::CoreRelease &release,
                                std::string asset = "Xray-linux-64.zip") {
  return xrm::InstallRequest{release, std::move(asset), false};
}

std::string success_script() {
  return "#!/bin/sh\n[ \"$1\" = version ] || exit 9\nprintf 'Xray 26.9.9 "
         "fixture\\n'\n";
}

} // namespace

TEST_CASE(
    "INS-01 release metadata distinguishes stable and prerelease releases") {
  LocalReleaseServer server;
  const auto archive = zip_with_core(success_script());
  server.set(metadata(server, sha256(archive)), archive);
  Services services;
  xrm::CoreInstaller installer(services.http, services.process,
                               std::filesystem::temp_directory_path() /
                                   "unused-installer-test",
                               server.api_url());

  auto stable = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(stable.has_value());
  REQUIRE(stable->size() == 1);
  CHECK(stable->front().tag == "v26.9.9");
  CHECK_FALSE(stable->front().prerelease);
  CHECK(stable->front().published_at == 1788266096000LL);
  REQUIRE(stable->front().assets.size() == 3);
  CHECK(stable->front().assets[0].platform == "linux");
  CHECK(stable->front().assets[0].architecture == "x86_64");
  CHECK(stable->front().assets[0].sha256 == sha256(archive));
  CHECK(stable->front().assets[1].architecture == "aarch64");
  CHECK_FALSE(stable->front().assets[2].sha256.has_value());

  auto all = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(true, std::move(completion));
      });
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  CHECK(find_release(*all, "v26.10.0-rc.1").prerelease);
}

TEST_CASE("INS-02 verified selected release installs only xray and returns its "
          "real version") {
  TempDirectory temp;
  LocalReleaseServer server;
  const auto archive = zip_with_core(success_script());
  const auto expected_hash = sha256(archive);
  server.set(metadata(server, expected_hash), archive);
  Services services;
  const auto root = temp.path / "cores";
  xrm::CoreInstaller installer(services.http, services.process, root,
                               server.api_url());
  auto releases = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(releases.has_value());
  auto request = request_for(find_release(*releases, "v26.9.9"));

  auto installed =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(std::move(request), std::move(completion));
      });
  REQUIRE(installed.has_value());
  CHECK(installed->name == "v26.9.9");
  CHECK(installed->managed);
  CHECK(installed->path == root / "v26.9.9" / "xray");
  CHECK(installed->reported_version == "Xray 26.9.9 fixture");
  CHECK(read_file(installed->path) == success_script());
  CHECK(std::filesystem::exists(root / "v26.9.9" / "xray"));
  CHECK(std::filesystem::is_empty(root) == false);
  CHECK(server.asset_requests() == 1);
  REQUIRE(std::filesystem::is_regular_file(installed->path));
  CHECK((std::filesystem::status(installed->path).permissions() &
         std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
}

TEST_CASE("INS-02 checksum policy, architecture and exact selected asset are "
          "enforced") {
  TempDirectory temp;
  LocalReleaseServer server;
  const auto archive = zip_with_core(success_script());
  auto releases_body = metadata(server, sha256(archive), false);
  server.set(releases_body, archive);
  Services services;
  const auto root = temp.path / "cores";
  xrm::CoreInstaller installer(services.http, services.process, root,
                               server.api_url());
  auto releases = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(releases.has_value());
  const auto release = find_release(*releases, "v26.9.9");

  auto unverified =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(request_for(release), std::move(completion));
      });
  REQUIRE_FALSE(unverified.has_value());
  CHECK(server.asset_requests() == 0);

  auto allowed_request = request_for(release);
  allowed_request.allow_unverified_checksum = true;
  auto allowed =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(std::move(allowed_request),
                                 std::move(completion));
      });
  REQUIRE(allowed.has_value());
  CHECK(server.asset_requests() == 1);
}

TEST_CASE(
    "INS-02 release asset URL must be the selected official Xray download") {
  TempDirectory temp;
  LocalReleaseServer server;
  const auto archive = zip_with_core(success_script());
  server.set(metadata(server, sha256(archive), true, true, true), archive);
  Services services;
  xrm::CoreInstaller installer(services.http, services.process,
                               temp.path / "cores", server.api_url());
  auto releases = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(releases.has_value());

  auto result =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(
            request_for(find_release(*releases, "v26.9.9")),
            std::move(completion));
      });
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == xrm::ErrorCode::protocol_error);
  CHECK(server.asset_requests() == 0);
  CHECK_FALSE(std::filesystem::exists(temp.path / "cores"));
}

TEST_CASE("INS-02 bad digest, wrong architecture, missing asset and traversal "
          "clean staging") {
  TempDirectory temp;
  LocalReleaseServer server;
  const auto archive = zip_with_core(success_script());
  server.set(metadata(server, std::string(64, '0')), archive);
  Services services;
  const auto root = temp.path / "cores";
  xrm::CoreInstaller installer(services.http, services.process, root,
                               server.api_url());
  auto releases = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(releases.has_value());
  const auto release = find_release(*releases, "v26.9.9");

  auto wrong_hash =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(request_for(release), std::move(completion));
      });
  REQUIRE_FALSE(wrong_hash.has_value());
  CHECK(std::filesystem::is_empty(root));

  auto wrong_arch =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(
            request_for(release, "Xray-linux-arm64-v8a.zip"),
            std::move(completion));
      });
  REQUIRE_FALSE(wrong_arch.has_value());
  CHECK(server.asset_requests() == 1);

  auto missing =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(request_for(release, "Xray-linux-64.tar.gz"),
                                 std::move(completion));
      });
  REQUIRE_FALSE(missing.has_value());
  CHECK(server.asset_requests() == 1);

  const auto traversal_zip = zip_with_core(success_script(), "../escaped");
  server.set(metadata(server, sha256(traversal_zip)), traversal_zip);
  auto traversal_releases = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(traversal_releases.has_value());
  auto traversal =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(
            request_for(find_release(*traversal_releases, "v26.9.9")),
            std::move(completion));
      });
  REQUIRE_FALSE(traversal.has_value());
  CHECK(std::filesystem::is_empty(root));
  CHECK_FALSE(std::filesystem::exists(temp.path / "escaped"));
}

TEST_CASE("INS-03 invalid core version output fails and removes staging") {
  TempDirectory temp;
  LocalReleaseServer server;
  const auto invalid = zip_with_core("#!/bin/sh\nexit 12\n");
  server.set(metadata(server, sha256(invalid)), invalid);
  Services services;
  const auto root = temp.path / "cores";
  xrm::CoreInstaller installer(services.http, services.process, root,
                               server.api_url());
  auto releases = wait_result<std::vector<xrm::CoreRelease>>(
      services.loop.value, [&](auto completion) {
        return installer.releases(false, std::move(completion));
      });
  REQUIRE(releases.has_value());
  auto failed =
      wait_result<xrm::CoreInstall>(services.loop.value, [&](auto completion) {
        return installer.install(
            request_for(find_release(*releases, "v26.9.9")),
            std::move(completion));
      });
  REQUIRE_FALSE(failed.has_value());
  CHECK(std::filesystem::is_empty(root));
}
