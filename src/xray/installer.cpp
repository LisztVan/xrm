#include "xray/installer.hpp"

#include "infra/http.hpp"
#include "infra/process.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xrm {
namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

bool local_http_url(std::string_view value) {
  return value.starts_with("http://127.0.0.1:") || value.starts_with("http://localhost:") ||
         value.starts_with("http://[::1]:");
}

bool official_api_url(std::string_view value) {
  return value == "https://api.github.com/repos/XTLS/Xray-core/releases" || local_http_url(value);
}

bool official_asset_url(std::string_view value, std::string_view tag, std::string_view name) {
  if (local_http_url(value)) return true;
  return value == "https://github.com/XTLS/Xray-core/releases/download/" +
                    std::string(tag) + "/" + std::string(name);
}

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool valid_sha256(std::string_view digest) {
  return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

Result<UnixMillis> parse_timestamp(std::string_view text) {
  auto number = [&](std::size_t offset, std::size_t count, std::string_view field) -> Result<int> {
    if (offset + count > text.size())
      return std::unexpected(error(ErrorCode::protocol_error, "invalid release publication time", std::string(field)));
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data() + offset, text.data() + offset + count, value);
    if (ec != std::errc{} || end != text.data() + offset + count)
      return std::unexpected(error(ErrorCode::protocol_error, "invalid release publication time", std::string(field)));
    return value;
  };
  if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':')
    return std::unexpected(error(ErrorCode::protocol_error, "expected RFC3339 release publication time", "published_at"));
  auto year = number(0, 4, "year");
  auto month = number(5, 2, "month");
  auto day = number(8, 2, "day");
  auto hour = number(11, 2, "hour");
  auto minute = number(14, 2, "minute");
  auto second = number(17, 2, "second");
  if (!year || !month || !day || !hour || !minute || !second)
    return std::unexpected(error(ErrorCode::protocol_error, "invalid release publication time", "published_at"));
  const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                         std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60)
    return std::unexpected(error(ErrorCode::protocol_error, "out-of-range release publication time", "published_at"));

  std::size_t pos = 19;
  int millis = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    const std::size_t begin = pos;
    unsigned int fraction_digits = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
      if (fraction_digits < 3) millis = millis * 10 + (text[pos] - '0');
      ++fraction_digits;
      ++pos;
    }
    if (pos == begin)
      return std::unexpected(error(ErrorCode::protocol_error, "empty fractional timestamp", "published_at"));
    if (fraction_digits == 1) millis *= 100;
    else if (fraction_digits == 2) millis *= 10;
  }
  int offset_minutes = 0;
  if (pos < text.size() && text[pos] == 'Z') {
    ++pos;
  } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const int sign = text[pos] == '+' ? 1 : -1;
    if (pos + 6 != text.size() || text[pos + 3] != ':')
      return std::unexpected(error(ErrorCode::protocol_error, "invalid RFC3339 timezone", "published_at"));
    auto offset_hour = number(pos + 1, 2, "timezone");
    auto offset_minute = number(pos + 4, 2, "timezone");
    if (!offset_hour || !offset_minute || *offset_hour > 23 || *offset_minute > 59)
      return std::unexpected(error(ErrorCode::protocol_error, "invalid RFC3339 timezone", "published_at"));
    offset_minutes = sign * (*offset_hour * 60 + *offset_minute);
    pos += 6;
  } else {
    return std::unexpected(error(ErrorCode::protocol_error, "release publication time has no timezone", "published_at"));
  }
  if (pos != text.size())
    return std::unexpected(error(ErrorCode::protocol_error, "trailing text in release publication time", "published_at"));

  const auto day_point = std::chrono::sys_days{date};
  const auto time = std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
                    std::chrono::seconds{*second} - std::chrono::minutes{offset_minutes} +
                    std::chrono::milliseconds{millis};
  return std::chrono::duration_cast<std::chrono::milliseconds>((day_point + time).time_since_epoch()).count();
}

std::pair<std::string, std::string> asset_platform_arch(std::string_view name) {
  if (!name.starts_with("Xray-") || !name.ends_with(".zip")) return {};
  const auto value = name.substr(5, name.size() - 9);
  const auto separator = value.find('-');
  if (separator == std::string_view::npos) return {};
  const std::string platform = lower(value.substr(0, separator));
  const auto arch = value.substr(separator + 1);
  std::string architecture;
  if (arch == "64") architecture = "x86_64";
  else if (arch == "32") architecture = "x86";
  else if (arch == "arm64-v8a") architecture = "aarch64";
  else if (arch == "arm32-v7a") architecture = "armv7";
  else if (arch == "riscv64") architecture = "riscv64";
  else if (arch == "loong64") architecture = "loong64";
  else if (arch == "mips64le") architecture = "mips64le";
  else if (arch == "mips32le") architecture = "mips32le";
  else if (arch == "ppc64le") architecture = "ppc64le";
  else if (arch == "s390x") architecture = "s390x";
  else return {};
  static const std::set<std::string, std::less<>> platforms{
      "linux", "windows", "macos", "android", "freebsd", "openbsd", "netbsd", "dragonfly"};
  if (!platforms.contains(platform)) return {};
  return {platform, architecture};
}

std::pair<std::string, std::string> current_platform_arch() {
#if defined(__ANDROID__)
  constexpr std::string_view platform = "android";
#elif defined(__linux__)
  constexpr std::string_view platform = "linux";
#elif defined(__APPLE__)
  constexpr std::string_view platform = "macos";
#elif defined(_WIN32)
  constexpr std::string_view platform = "windows";
#elif defined(__FreeBSD__)
  constexpr std::string_view platform = "freebsd";
#elif defined(__OpenBSD__)
  constexpr std::string_view platform = "openbsd";
#elif defined(__NetBSD__)
  constexpr std::string_view platform = "netbsd";
#elif defined(__DragonFly__)
  constexpr std::string_view platform = "dragonfly";
#else
  constexpr std::string_view platform = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
  constexpr std::string_view arch = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  constexpr std::string_view arch = "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  constexpr std::string_view arch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  constexpr std::string_view arch = "armv7";
#elif defined(__riscv) && __riscv_xlen == 64
  constexpr std::string_view arch = "riscv64";
#elif defined(__loongarch64)
  constexpr std::string_view arch = "loong64";
#elif defined(__mips64)
  constexpr std::string_view arch = "mips64le";
#elif defined(__mips__)
  constexpr std::string_view arch = "mips32le";
#elif defined(__powerpc64__)
  constexpr std::string_view arch = "ppc64le";
#elif defined(__s390x__)
  constexpr std::string_view arch = "s390x";
#else
  constexpr std::string_view arch = "unknown";
#endif
  return {std::string(platform), std::string(arch)};
}

bool safe_release_tag(std::string_view tag) {
  if (tag.empty() || tag.size() > 128 || tag == "." || tag == "..") return false;
  return std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_';
  });
}

Result<std::vector<CoreRelease>> parse_releases(std::string_view body, bool include_prerelease) {
  Json document;
  try {
    document = Json::parse(body);
  } catch (const Json::exception&) {
    return std::unexpected(error(ErrorCode::protocol_error, "official release response is not valid JSON"));
  }
  if (!document.is_array())
    return std::unexpected(error(ErrorCode::protocol_error, "official release response must be an array"));

  std::vector<CoreRelease> result;
  for (const auto& item : document) {
    if (!item.is_object() || !item.contains("tag_name") || !item["tag_name"].is_string() ||
        !item.contains("published_at") || !item["published_at"].is_string() ||
        !item.contains("prerelease") || !item["prerelease"].is_boolean() ||
        !item.contains("assets") || !item["assets"].is_array())
      return std::unexpected(error(ErrorCode::protocol_error, "release response is missing required fields"));
    CoreRelease release;
    release.tag = item["tag_name"].get<std::string>();
    auto published = parse_timestamp(item["published_at"].get<std::string>());
    if (!published) return std::unexpected(published.error());
    release.published_at = *published;
    release.prerelease = item["prerelease"].get<bool>();
    if (release.tag.empty())
      return std::unexpected(error(ErrorCode::protocol_error, "release tag is empty", "tag_name"));

    for (const auto& asset_json : item["assets"]) {
      if (!asset_json.is_object() || !asset_json.contains("name") || !asset_json["name"].is_string() ||
          !asset_json.contains("browser_download_url") || !asset_json["browser_download_url"].is_string())
        return std::unexpected(error(ErrorCode::protocol_error, "release asset is missing name or download URL"));
      ReleaseAsset asset;
      asset.name = asset_json["name"].get<std::string>();
      asset.url = asset_json["browser_download_url"].get<std::string>();
      auto [platform, arch] = asset_platform_arch(asset.name);
      asset.platform = std::move(platform);
      asset.architecture = std::move(arch);
      if (!asset_json.contains("digest") || asset_json["digest"].is_null()) {
        asset.sha256.reset();
      } else if (asset_json["digest"].is_string()) {
        const auto digest = asset_json["digest"].get<std::string>();
        if (digest.starts_with("sha256:")) {
          const auto hash = std::string_view(digest).substr(7);
          if (!valid_sha256(hash))
            return std::unexpected(error(ErrorCode::protocol_error, "release asset has an invalid sha256 digest", asset.name));
          asset.sha256 = lower(hash);
        }
      } else {
        return std::unexpected(error(ErrorCode::protocol_error, "release asset digest must be a string or null", asset.name));
      }
      release.assets.push_back(std::move(asset));
    }
    if (include_prerelease || !release.prerelease) result.push_back(std::move(release));
  }
  return result;
}

std::string trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
  return std::string(value);
}

std::string sha256(std::string_view bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 0x0f]);
  }
  return result;
}

bool safe_archive_path(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find('\\') != std::string_view::npos)
    return false;
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') return false;
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto slash = path.find('/', begin);
    const auto part = path.substr(begin, slash == std::string_view::npos ? path.size() - begin : slash - begin);
    if (part == ".." || part == ".") return false;
    if (slash == std::string_view::npos) break;
    begin = slash + 1;
  }
  return true;
}

Result<std::filesystem::path> extract_xray(std::string_view bytes,
                                           const std::filesystem::path& staging) {
  archive* reader = archive_read_new();
  if (!reader) return std::unexpected(error(ErrorCode::io_error, "cannot create archive reader"));
  archive_read_support_format_zip(reader);
  if (archive_read_open_memory(reader, bytes.data(), bytes.size()) != ARCHIVE_OK) {
    const std::string message = archive_error_string(reader) ? archive_error_string(reader) : "invalid release archive";
    archive_read_free(reader);
    return std::unexpected(error(ErrorCode::protocol_error, message, "archive"));
  }

#if defined(_WIN32)
  constexpr std::string_view core_name = "xray.exe";
#else
  constexpr std::string_view core_name = "xray";
#endif
  const auto core_path = staging / core_name;
  bool found = false;
  std::size_t entries = 0;
  AppError failure{ErrorCode::protocol_error, "release archive does not contain the Xray binary", "archive", std::nullopt};
  archive_entry* entry = nullptr;
  for (;;) {
    const int status = archive_read_next_header(reader, &entry);
    if (status == ARCHIVE_EOF) break;
    if (status != ARCHIVE_OK) {
      failure.message = archive_error_string(reader) ? archive_error_string(reader) : "cannot read release archive";
      break;
    }
    if (++entries > 10000) {
      failure.message = "release archive contains too many entries";
      break;
    }
    const char* raw_path = archive_entry_pathname(entry);
    if (!raw_path || !safe_archive_path(raw_path)) {
      failure.message = "release archive contains a path outside its staging directory";
      failure.subject = "archive.path";
      break;
    }
    const auto kind = archive_entry_filetype(entry);
    if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
        (kind != AE_IFREG && kind != AE_IFDIR)) {
      failure.message = "release archive contains a link or unsupported file type";
      failure.subject = raw_path;
      break;
    }
    if (kind == AE_IFDIR || std::string_view(raw_path) != core_name) {
      if (archive_read_data_skip(reader) != ARCHIVE_OK) {
        failure.message = "cannot skip non-core release asset";
        break;
      }
      continue;
    }
    if (found) {
      failure.message = "release archive contains more than one Xray binary";
      failure.subject = core_name;
      break;
    }
    const auto size = archive_entry_size(entry);
    if (size <= 0 || size > 256 * 1024 * 1024) {
      failure.message = "Xray archive member has an invalid or excessive size";
      failure.subject = core_name;
      break;
    }
    const int fd = ::open(core_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0700);
    if (fd < 0) {
      failure = error(ErrorCode::io_error, "cannot create staged Xray binary", core_path.string());
      break;
    }
    std::array<char, 64 * 1024> buffer{};
    bool write_ok = true;
    la_int64_t total = 0;
    for (;;) {
      const auto count = archive_read_data(reader, buffer.data(), buffer.size());
      if (count == 0) break;
      if (count < 0) {
        failure.message = archive_error_string(reader) ? archive_error_string(reader) : "cannot extract Xray binary";
        write_ok = false;
        break;
      }
      std::size_t offset = 0;
      while (offset < static_cast<std::size_t>(count)) {
        const auto written = ::write(fd, buffer.data() + offset, static_cast<std::size_t>(count) - offset);
        if (written <= 0) {
          failure = error(ErrorCode::io_error, "cannot write staged Xray binary", core_path.string());
          write_ok = false;
          break;
        }
        offset += static_cast<std::size_t>(written);
      }
      total += count;
      if (!write_ok) break;
      if (total > size || total > 256 * 1024 * 1024) {
        failure.message = "Xray archive member exceeds its declared or allowed size";
        write_ok = false;
        break;
      }
    }
    if (::fchmod(fd, 0755) != 0) {
      failure = error(ErrorCode::io_error, "cannot mark staged Xray binary executable", core_path.string());
      write_ok = false;
    }
    if (::close(fd) != 0) {
      failure = error(ErrorCode::io_error, "cannot close staged Xray binary", core_path.string());
      write_ok = false;
    }
    if (!write_ok || total != size) {
      if (write_ok) failure.message = "Xray archive member ended before its declared size";
      break;
    }
    found = true;
  }
  archive_read_close(reader);
  archive_read_free(reader);
  if (!found) return std::unexpected(failure);
  return core_path;
}

}  // namespace

struct CoreInstaller::Impl {
  struct State : std::enable_shared_from_this<State> {
    State(HttpClient& http_client, ProcessService& process_service,
          std::filesystem::path root_path, std::string api_url)
        : http(&http_client), process(&process_service), cores_root(std::move(root_path)),
          releases_url(std::move(api_url)) {}

    Result<Id> fetch_releases(bool include_prerelease,
                              Completion<std::vector<CoreRelease>> completion) {
      if (!official_api_url(releases_url))
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "release metadata URL must use HTTPS (only loopback HTTP is allowed for fixtures)",
                                     "releases_api_url"));
      HttpRequest request;
      request.url = releases_url + (releases_url.find('?') == std::string::npos ? "?per_page=100" : "&per_page=100");
      request.user_agent = "xrm-core-installer";
      request.headers.emplace("Accept", "application/vnd.github+json");
      request.headers.emplace("X-GitHub-Api-Version", "2022-11-28");
      request.timeout = Milliseconds{30000};
      request.max_body_bytes = 16 * 1024 * 1024;
      auto self = shared_from_this();
      return http->fetch(std::move(request), [self, include_prerelease,
                                              completion = std::move(completion)](Result<HttpResponse> response) mutable {
        if (!response) {
          completion(std::unexpected(response.error()));
          return;
        }
        if (response->status != 200) {
          completion(std::unexpected(error(ErrorCode::network_error,
                                           "official release metadata request returned HTTP " + std::to_string(response->status))));
          return;
        }
        completion(parse_releases(response->body, include_prerelease));
      });
    }

    Result<Id> start_install(InstallRequest request, Completion<CoreInstall> completion) {
      if (!safe_release_tag(request.release.tag))
        return std::unexpected(error(ErrorCode::invalid_argument, "release tag is unsafe for a managed core path", "release.tag"));
      if (request.asset_name.empty())
        return std::unexpected(error(ErrorCode::invalid_argument, "an exact release asset must be selected", "asset_name"));
      const auto operation = "install-" + std::to_string(next_operation++);
      const auto tag = request.release.tag;
      if (!installing.insert(tag).second)
        return std::unexpected(error(ErrorCode::busy, "this release is already being installed", tag));
      auto done = std::make_shared<Completion<CoreInstall>>(std::move(completion));
      auto self = shared_from_this();
      auto metadata_fetch = fetch_releases(true, [self, tag, request = std::move(request), done](Result<std::vector<CoreRelease>> releases) mutable {
        if (!releases) {
          self->finish(tag, done, std::unexpected(releases.error()));
          return;
        }
        const auto release_it = std::find_if(releases->begin(), releases->end(), [&](const CoreRelease& release) {
          return release.tag == request.release.tag;
        });
        if (release_it == releases->end()) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::not_found,
                                                        "selected tag is not an official Xray release",
                                                        request.release.tag)));
          return;
        }
        const auto asset_it = std::find_if(release_it->assets.begin(), release_it->assets.end(), [&](const ReleaseAsset& asset) {
          return asset.name == request.asset_name;
        });
        if (asset_it == release_it->assets.end()) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::not_found,
                                                        "selected asset is not part of the official release",
                                                        request.asset_name)));
          return;
        }
        const auto [platform, architecture] = current_platform_arch();
        if (asset_it->platform != platform || asset_it->architecture != architecture) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::unsupported,
                                                        "selected release asset does not match this platform and architecture",
                                                        asset_it->name)));
          return;
        }
        if (!official_asset_url(asset_it->url, release_it->tag, asset_it->name)) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::protocol_error,
                                                        "release asset URL is not the selected XTLS/Xray-core official asset",
                                                        asset_it->name)));
          return;
        }
        if (!asset_it->sha256 && !request.allow_unverified_checksum) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::unsupported,
                                                        "official asset has no SHA-256 digest; explicit unverified install is required",
                                                        asset_it->name)));
          return;
        }

        const auto final_dir = self->cores_root / release_it->tag;
        std::error_code ec;
        if (std::filesystem::exists(final_dir, ec)) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::conflict,
                                                        "managed release directory already exists",
                                                        final_dir.string())));
          return;
        }
        std::filesystem::create_directories(self->cores_root, ec);
        if (ec) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::io_error,
                                                        "cannot create managed core directory",
                                                        ec.message())));
          return;
        }
        const auto stage = self->cores_root / (".staging-" + release_it->tag + "-" + std::to_string(self->next_stage++));
        if (!std::filesystem::create_directory(stage, ec) || ec) {
          self->finish(tag, done, std::unexpected(error(ErrorCode::io_error,
                                                        "cannot create core staging directory",
                                                        ec.message())));
          return;
        }
        std::filesystem::permissions(stage, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) {
          std::error_code ignored;
          std::filesystem::remove_all(stage, ignored);
          self->finish(tag, done, std::unexpected(error(ErrorCode::io_error,
                                                        "cannot restrict core staging directory permissions",
                                                        ec.message())));
          return;
        }
        auto asset = *asset_it;
        auto download = HttpRequest{};
        download.url = asset.url;
        download.user_agent = "xrm-core-installer";
        download.headers.emplace("Accept", "application/octet-stream");
        download.timeout = Milliseconds{120000};
        download.max_body_bytes = 512 * 1024 * 1024;
        auto download_started = self->http->fetch(std::move(download),
            [self, tag, request = std::move(request), release = *release_it, asset = std::move(asset),
             stage, final_dir, done](Result<HttpResponse> response) mutable {
          auto fail = [&](AppError failure) mutable {
            std::error_code ignored;
            std::filesystem::remove_all(stage, ignored);
            self->finish(tag, done, std::unexpected(std::move(failure)));
          };
          if (!response) { fail(response.error()); return; }
          if (response->status != 200) {
            fail(error(ErrorCode::network_error,
                       "official release asset request returned HTTP " + std::to_string(response->status), asset.name));
            return;
          }
          if (asset.sha256 && sha256(response->body) != *asset.sha256) {
            fail(error(ErrorCode::protocol_error, "downloaded release asset SHA-256 does not match GitHub digest", asset.name));
            return;
          }
          auto extracted = extract_xray(response->body, stage);
          if (!extracted) { fail(extracted.error()); return; }
          ProcessSpec command;
          command.executable = *extracted;
          command.arguments = {"version"};
          command.cwd = stage;
          auto version = self->process->execute(std::move(command), Milliseconds{15000},
              [self, tag, release = std::move(release), stage,
               final_dir, binary = *extracted, done](Result<ProcessResult> result) mutable {
            auto fail_version = [&](AppError failure) mutable {
              std::error_code ignored;
              std::filesystem::remove_all(stage, ignored);
              self->finish(tag, done, std::unexpected(std::move(failure)));
            };
            if (!result) { fail_version(result.error()); return; }
            if (result->exit.exit_code != 0 || result->exit.term_signal != 0 || result->output_truncated) {
              fail_version(error(ErrorCode::protocol_error, "installed core version command failed or was truncated",
                                 std::string(release.tag)));
              return;
            }
            const auto output = trim(result->stdout_text);
            if (output.empty() || !output.starts_with("Xray ")) {
              fail_version(error(ErrorCode::protocol_error, "installed binary did not identify itself as Xray",
                                 std::string(release.tag)));
              return;
            }
            std::string expected = release.tag;
            if (expected.starts_with('v')) expected.erase(expected.begin());
            const auto version_begin = std::string_view(output).find_first_not_of(' ', 5);
            if (version_begin == std::string_view::npos) {
              fail_version(error(ErrorCode::protocol_error, "Xray version output did not include a version", release.tag));
              return;
            }
            const auto version_end = output.find_first_of(" (\r\n", version_begin);
            const auto reported_tag = output.substr(version_begin,
                version_end == std::string::npos ? std::string::npos : version_end - version_begin);
            if (reported_tag != expected) {
              fail_version(error(ErrorCode::protocol_error,
                                 "installed Xray version does not match the selected release tag", reported_tag));
              return;
            }
            std::error_code ec;
            std::filesystem::rename(stage, final_dir, ec);
            if (ec) {
              fail_version(error(ErrorCode::io_error, "cannot publish installed core directory", ec.message()));
              return;
            }
            CoreInstall installed;
            installed.id = "managed-" + release.tag;
            installed.name = release.tag;
            installed.path = final_dir / binary.filename();
            installed.reported_version = output;
            installed.managed = true;
            self->finish(tag, done, std::move(installed));
          });
          if (!version) fail(version.error());
        });
        if (!download_started) {
          std::error_code ignored;
          std::filesystem::remove_all(stage, ignored);
          self->finish(tag, done, std::unexpected(download_started.error()));
        }
      });
      if (!metadata_fetch) {
        installing.erase(tag);
        return std::unexpected(metadata_fetch.error());
      }
      return operation;
    }

    void finish(const std::string& tag, const std::shared_ptr<Completion<CoreInstall>>& completion,
                Result<CoreInstall> result) {
      installing.erase(tag);
      if (*completion) {
        auto callback = std::move(*completion);
        callback(std::move(result));
      }
    }

    HttpClient* http;
    ProcessService* process;
    std::filesystem::path cores_root;
    std::string releases_url;
    std::uint64_t next_operation = 1;
    std::uint64_t next_stage = 1;
    std::unordered_set<std::string> installing;
  };

  Impl(HttpClient& http, ProcessService& process, std::filesystem::path cores_root,
       std::string releases_api_url)
      : state(std::make_shared<State>(http, process, std::move(cores_root), std::move(releases_api_url))) {}

  std::shared_ptr<State> state;
};

CoreInstaller::CoreInstaller(HttpClient& http, ProcessService& process,
                             std::filesystem::path cores_root, std::string releases_api_url)
    : impl_(std::make_unique<Impl>(http, process, std::move(cores_root), std::move(releases_api_url))) {}
CoreInstaller::~CoreInstaller() = default;

Result<Id> CoreInstaller::releases(bool include_prerelease,
                                   Completion<std::vector<CoreRelease>> completion) {
  if (!completion)
    return std::unexpected(error(ErrorCode::invalid_argument, "release completion callback is required", "completion"));
  return impl_->state->fetch_releases(include_prerelease, std::move(completion));
}

Result<Id> CoreInstaller::install(InstallRequest request,
                                  Completion<CoreInstall> completion) {
  if (!completion)
    return std::unexpected(error(ErrorCode::invalid_argument, "install completion callback is required", "completion"));
  return impl_->state->start_install(std::move(request), std::move(completion));
}

}  // namespace xrm
