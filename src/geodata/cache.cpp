// XRM: Caches downloaded geodata resources for local use.

#include "geodata/cache.hpp"

#include "infra/process.hpp"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <unordered_map>

namespace xrm {

namespace {
AppError failure(ErrorCode code, std::string message,
                 std::string subject = {}) {
  return {code, std::move(message),
          subject.empty() ? std::nullopt
                          : std::optional<std::string>(std::move(subject)),
          std::nullopt};
}

bool safe_segment(const std::string &value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
         });
}

std::string sha256(std::string_view bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
         digest);
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(sizeof(digest) * 2);
  for (unsigned char ch : digest) {
    result += digits[ch >> 4];
    result += digits[ch & 15];
  }
  return result;
}

void write_file(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
    throw std::runtime_error("open failed");
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.close();
  if (!file.good())
    throw std::runtime_error("write failed");
}
} // namespace

struct GeodataCache::Impl {
  struct State;
  struct Job {
    uv_work_t work{};
    std::shared_ptr<State> owner;
    GeodataAcquireRequest request;
    Completion<GeodataAssetVersion> completion;
    Id id, http_id, command_id;
    std::atomic<bool> cancelled{false};
    std::string body;
    std::optional<AppError> stage_error;
    std::filesystem::path stage, final_dir;
    GeodataAssetVersion version;
    std::set<std::string> check_labels;
  };

  struct State : std::enable_shared_from_this<State> {
    State(HttpClient &h, ProcessService &p, uv_loop_t &l,
          std::filesystem::path r)
        : http(h), process(p), loop(l), root(std::move(r)) {}
    HttpClient &http;
    ProcessService &process;
    uv_loop_t &loop;
    std::filesystem::path root;
    std::uint64_t next_id = 1;
    std::unordered_map<Id, std::shared_ptr<Job>> jobs;
    bool closing = false;
    std::function<void()> drained;

    void finish(const std::shared_ptr<Job> &job,
                Result<GeodataAssetVersion> result) {
      if (!jobs.erase(job->id))
        return;
      if (!job->stage.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(job->stage, ignored);
      }
      auto callback = std::move(job->completion);
      job->owner.reset();
      try {
        if (callback)
          callback(std::move(result));
      } catch (...) { /* libuv callback */
      }
      if (closing && jobs.empty() && drained) {
        auto done = std::move(drained);
        done();
      }
    }

    static void stage_work(uv_work_t *work) {
      auto *job = static_cast<Job *>(work->data);
      struct CleanupOpenSslWorkerState {
        ~CleanupOpenSslWorkerState() { OPENSSL_thread_stop(); }
      } cleanup;
      try {
        if (job->cancelled)
          return;
        if (!job->request.local_source.empty()) {
          std::ifstream input(job->request.local_source, std::ios::binary);
          if (!input) {
            job->stage_error =
                failure(ErrorCode::io_error, "无法读取本地Geodata资源",
                        job->request.local_source.string());
            return;
          }
          std::array<char, 8192> buffer{};
          while (input) {
            input.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count > job->request.max_body_bytes - job->body.size()) {
              job->stage_error =
                  failure(ErrorCode::too_large, "本地Geodata资源超过大小上限",
                          job->request.local_source.string());
              return;
            }
            job->body.append(buffer.data(), count);
            if (job->cancelled)
              return;
          }
          if (input.bad()) {
            job->stage_error =
                failure(ErrorCode::io_error, "读取本地Geodata资源失败",
                        job->request.local_source.string());
            return;
          }
        }
        auto inspected = inspect_geodata_bytes(job->request.kind, job->body);
        if (!inspected) {
          job->stage_error = inspected.error();
          return;
        }
        for (const auto &label : job->request.required_labels)
          if (!inspected->labels.contains(label)) {
            job->stage_error = failure(ErrorCode::invalid_argument,
                                       "Geodata 文件缺少所需标签", label);
            return;
          }
        job->check_labels = job->request.required_labels;
        if (job->check_labels.empty())
          job->check_labels.insert(*inspected->labels.begin());
        auto files = geodata_validation_files(
            job->request.kind, job->request.file_name, job->check_labels);
        if (!files) {
          job->stage_error = files.error();
          return;
        }
        const auto digest = sha256(job->body);
        const auto kind =
            job->request.kind == GeoAssetKind::geoip ? "geoip" : "geosite";
        job->final_dir =
            job->owner->root / job->request.resource_id /
            (std::string(kind) + "-" + digest + "-" + job->request.file_name);
        job->version = {job->request.resource_id,
                        job->request.kind,
                        job->request.file_name,
                        digest,
                        job->final_dir / "assets",
                        job->final_dir / "assets" / job->request.file_name,
                        std::move(inspected->labels),
                        job->body.size()};
        std::filesystem::create_directories(job->owner->root);
        auto pattern =
            (job->owner->root / (".staging-" + job->id + "-XXXXXX")).string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char *created = ::mkdtemp(buffer.data());
        if (!created)
          throw std::runtime_error("mkdtemp failed");
        job->stage = created;
        const auto assets = job->stage / "assets";
        const auto conf = job->stage / "conf";
        std::filesystem::create_directory(assets);
        std::filesystem::create_directory(conf);
        write_file(assets / job->request.file_name, job->body);
        for (const auto &file : *files)
          write_file(conf / file.relative_name, file.bytes);
        job->body.clear();
      } catch (const std::exception &error) {
        job->stage_error =
            failure(ErrorCode::io_error,
                    "Geodata staging 失败：" + std::string(error.what()));
      }
    }

    static void staged(uv_work_t *work, int status) {
      auto *raw = static_cast<Job *>(work->data);
      auto self = raw->owner;
      auto found = self->jobs.find(raw->id);
      if (found == self->jobs.end())
        return;
      auto job = found->second;
      if (job->cancelled || status == UV_ECANCELED) {
        self->finish(job, std::unexpected(failure(ErrorCode::cancelled,
                                                  "Geodata 更新已取消")));
        return;
      }
      if (job->stage_error) {
        self->finish(job, std::unexpected(*job->stage_error));
        return;
      }
      ProcessSpec spec;
      spec.executable = job->request.validator_binary;
      spec.arguments = {"run", "-test", "-confdir",
                        (job->stage / "conf").string()};
      spec.cwd = job->stage;
      spec.environment = {
          {"XRAY_LOCATION_ASSET", (job->stage / "assets").string()}};
      auto command = self->process.execute(
          std::move(spec), job->request.timeout,
          [self, job](Result<ProcessResult> checked) {
            if (job->cancelled) {
              self->finish(job, std::unexpected(failure(ErrorCode::cancelled,
                                                        "Geodata 更新已取消")));
              return;
            }
            if (!checked) {
              self->finish(job, std::unexpected(checked.error()));
              return;
            }
            if (checked->exit.exit_code != 0 ||
                checked->exit.term_signal != 0) {
              self->finish(job,
                           std::unexpected(failure(ErrorCode::core_check_failed,
                                                   "Xray 未接受 Geodata：" +
                                                       checked->stderr_text)));
              return;
            }
            std::error_code ec;
            std::filesystem::create_directories(job->final_dir.parent_path(),
                                                ec);
            if (ec) {
              self->finish(job,
                           std::unexpected(failure(ErrorCode::io_error,
                                                   "无法创建 Geodata 版本目录",
                                                   ec.message())));
              return;
            }
            if (std::filesystem::exists(job->final_dir, ec)) {
              if (ec ||
                  !std::filesystem::is_regular_file(job->version.path, ec)) {
                self->finish(
                    job, std::unexpected(failure(ErrorCode::conflict,
                                                 "已有 Geodata 版本目录不完整",
                                                 job->final_dir.string())));
                return;
              }
            } else {
              std::filesystem::rename(job->stage, job->final_dir, ec);
              if (ec) {
                self->finish(
                    job, std::unexpected(failure(ErrorCode::io_error,
                                                 "无法原子发布 Geodata 版本",
                                                 ec.message())));
                return;
              }
              job->stage.clear();
            }
            self->finish(job, job->version);
          });
      if (!command)
        self->finish(job, std::unexpected(command.error()));
      else
        job->command_id = *command;
    }

    Result<Id> acquire(GeodataAcquireRequest request,
                       Completion<GeodataAssetVersion> callback) {
      if (closing)
        return std::unexpected(
            failure(ErrorCode::busy, "Geodata 缓存正在关闭"));
      if (jobs.size() >= 3)
        return std::unexpected(
            failure(ErrorCode::busy, "同时最多更新三个 Geodata 资源"));
      if (!callback || (request.url.empty() == request.local_source.empty()) ||
          !safe_segment(request.resource_id))
        return std::unexpected(
            failure(ErrorCode::invalid_argument,
                    "Geodata 需要安全的资源 ID、唯一来源和回调"));
      if (!request.local_source.empty()) {
        std::error_code ec;
        const auto status =
            std::filesystem::symlink_status(request.local_source, ec);
        if (!request.local_source.is_absolute() || ec ||
            !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
          return std::unexpected(
              failure(ErrorCode::invalid_argument,
                      "本地Geodata来源必须是绝对路径上的普通文件",
                      request.local_source.string()));
      }
      if (!request.validator_binary.is_absolute() ||
          !std::filesystem::exists(request.validator_binary))
        return std::unexpected(failure(ErrorCode::not_found,
                                       "未选择可用于资源验证的 Xray",
                                       request.validator_binary.string()));
      if (request.timeout.count() <= 0 || request.max_body_bytes == 0 ||
          request.max_body_bytes > 64 * 1024 * 1024)
        return std::unexpected(failure(ErrorCode::invalid_argument,
                                       "Geodata 下载大小或时限超出允许范围"));
      std::set<std::string> labels;
      for (auto label : request.required_labels) {
        std::transform(label.begin(), label.end(), label.begin(),
                       [](unsigned char ch) {
                         return static_cast<char>(std::tolower(ch));
                       });
        labels.insert(std::move(label));
      }
      auto filename = geodata_validation_files(
          request.kind, request.file_name,
          labels.empty() ? std::set<std::string>{"x"} : labels);
      if (!filename)
        return std::unexpected(filename.error());
      request.required_labels = std::move(labels);
      for (const auto &[_, active] : jobs)
        if (active->request.resource_id == request.resource_id)
          return std::unexpected(failure(ErrorCode::busy,
                                         "该 Geodata 资源已有更新任务",
                                         request.resource_id));
      const auto id = "geodata-" + std::to_string(next_id++);
      auto job = std::make_shared<Job>();
      job->owner = shared_from_this();
      job->id = id;
      job->request = std::move(request);
      job->completion = std::move(callback);
      jobs.emplace(id, job);
      if (!job->request.local_source.empty()) {
        job->work.data = job.get();
        if (uv_queue_work(&loop, &job->work, stage_work, staged) != 0) {
          jobs.erase(id);
          return std::unexpected(
              failure(ErrorCode::io_error, "无法安排本地Geodata检查任务"));
        }
        return id;
      }
      HttpRequest fetch;
      fetch.url = job->request.url;
      fetch.headers = job->request.headers;
      fetch.user_agent = "xrm-geodata";
      fetch.route = job->request.route;
      fetch.timeout = job->request.timeout;
      fetch.max_body_bytes = job->request.max_body_bytes;
      auto self = shared_from_this();
      auto started = http.fetch(
          std::move(fetch), [self, job](Result<HttpResponse> response) {
            if (job->cancelled) {
              self->finish(job, std::unexpected(failure(ErrorCode::cancelled,
                                                        "Geodata 更新已取消")));
              return;
            }
            if (!response) {
              self->finish(job, std::unexpected(response.error()));
              return;
            }
            if (response->status != 200) {
              self->finish(job, std::unexpected(failure(
                                    ErrorCode::network_error,
                                    "Geodata 下载返回 HTTP " +
                                        std::to_string(response->status))));
              return;
            }
            job->body = std::move(response->body);
            job->work.data = job.get();
            if (uv_queue_work(&self->loop, &job->work, stage_work, staged) != 0)
              self->finish(
                  job, std::unexpected(failure(ErrorCode::io_error,
                                               "无法安排 Geodata 检查任务")));
          });
      if (!started) {
        jobs.erase(id);
        return std::unexpected(started.error());
      }
      job->http_id = *started;
      return id;
    }

    Result<void> cancel(const Id &id) {
      const auto found = jobs.find(id);
      if (found == jobs.end())
        return std::unexpected(
            failure(ErrorCode::not_found, "Geodata 更新任务不存在", id));
      auto job = found->second;
      job->cancelled = true;
      if (!job->http_id.empty())
        (void)http.cancel(job->http_id);
      if (!job->command_id.empty())
        (void)process.cancel_command(job->command_id);
      return {};
    }

    void shutdown(std::function<void()> callback) {
      closing = true;
      if (callback)
        drained = std::move(callback);
      std::vector<Id> ids;
      for (const auto &[id, _] : jobs)
        ids.push_back(id);
      for (const auto &id : ids)
        (void)cancel(id);
      if (jobs.empty() && drained) {
        auto done = std::move(drained);
        done();
      }
    }
  };

  Impl(HttpClient &h, ProcessService &p, uv_loop_t &loop,
       std::filesystem::path root)
      : state(std::make_shared<State>(h, p, loop, std::move(root))) {}
  std::shared_ptr<State> state;
};

GeodataCache::GeodataCache(HttpClient &http, ProcessService &process,
                           uv_loop_s &loop, std::filesystem::path root)
    : impl_(std::make_unique<Impl>(http, process,
                                   reinterpret_cast<uv_loop_t &>(loop),
                                   std::move(root))) {}
GeodataCache::~GeodataCache() {
  if (impl_)
    impl_->state->shutdown({});
}

Result<Id> GeodataCache::acquire(GeodataAcquireRequest request,
                                 Completion<GeodataAssetVersion> completion) {
  return impl_->state->acquire(std::move(request), std::move(completion));
}

Result<void> GeodataCache::cancel(const Id &id) {
  return impl_->state->cancel(id);
}
void GeodataCache::shutdown(std::function<void()> drained) {
  impl_->state->shutdown(std::move(drained));
}

} // namespace xrm
