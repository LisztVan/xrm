// XRM: Implements bounded HTTP downloads for subscriptions and resources.

#include "infra/http.hpp"

#include <curl/curl.h>
#include <uv.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <deque>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace xrm {
namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = {}) {
  return {code, std::move(message), std::move(subject), std::nullopt};
}

struct BodyTarget {
  std::string *body;
  std::size_t maximum;
  bool *too_large;
};

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(
      result.begin(), result.end(), result.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

bool valid_header_name(std::string_view name) {
  if (name.empty())
    return false;
  constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
  return std::all_of(name.begin(), name.end(), [&](unsigned char c) {
    const bool alpha_numeric = (c >= '0' && c <= '9') ||
                               (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return alpha_numeric ||
           punctuation.find(static_cast<char>(c)) != std::string_view::npos;
  });
}

bool has_header_break(std::string_view value) {
  return value.find('\r') != std::string_view::npos ||
         value.find('\n') != std::string_view::npos;
}

bool supported_proxy_scheme(std::string_view url) {
  const auto colon = url.find("://");
  if (colon == std::string_view::npos)
    return false;
  const auto scheme = lower(url.substr(0, colon));
  return scheme == "http" || scheme == "https" || scheme == "socks4" ||
         scheme == "socks4a" || scheme == "socks5" || scheme == "socks5h";
}

std::size_t append_body(char *data, std::size_t size, std::size_t count,
                        void *opaque) {
  auto *target = static_cast<BodyTarget *>(opaque);
  if (count != 0 && size > static_cast<std::size_t>(-1) / count)
    return 0;
  const std::size_t bytes = size * count;
  if (bytes >
      target->maximum - std::min(target->maximum, target->body->size())) {
    *target->too_large = true;
    return 0;
  }
  target->body->append(data, bytes);
  return bytes;
}

std::size_t receive_header(char *data, std::size_t size, std::size_t count,
                           void *opaque) {
  auto *headers = static_cast<std::map<std::string, std::string> *>(opaque);
  if (count != 0 && size > static_cast<std::size_t>(-1) / count)
    return 0;
  const std::size_t bytes = size * count;
  std::string_view line(data, bytes);
  if (line.starts_with("HTTP/")) {
    headers->clear();
    return bytes;
  }
  const auto colon = line.find(':');
  if (colon == std::string_view::npos)
    return bytes;
  const std::string name = lower(line.substr(0, colon));
  if (name.empty())
    return bytes;
  std::size_t value_end = line.size();
  while (value_end &&
         (line[value_end - 1] == '\r' || line[value_end - 1] == '\n'))
    --value_end;
  headers->insert_or_assign(
      name, std::string(line.substr(colon + 1, value_end - colon - 1)));
  return bytes;
}

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl_global() {
  static CurlGlobal global;
  (void)global;
}

} // namespace

struct HttpClient::Impl {
  struct State;
  struct Request {
    uv_work_t work{};
    std::shared_ptr<State> keep_state_alive;
    Id id;
    HttpRequest input;
    Completion<HttpResponse> completion;
    std::atomic<bool> cancelled{false};
    HttpResponse response;
    CURLcode curl_result = CURLE_OK;
    bool body_too_large = false;
    bool cancel_before_work = false;
    curl_slist *curl_headers = nullptr;
    std::string curl_error;
  };

  struct State : std::enable_shared_from_this<State> {
    explicit State(uv_loop_t &event_loop) : loop(&event_loop) {}

    uv_loop_t *loop;
    std::uint64_t next_id = 1;
    std::size_t active = 0;
    bool shutting_down = false;
    std::function<void()> drained;
    std::deque<std::shared_ptr<Request>> pending;
    std::unordered_map<Id, std::shared_ptr<Request>> requests;

    static int transfer_progress(void *opaque, curl_off_t, curl_off_t,
                                 curl_off_t, curl_off_t) {
      auto *request = static_cast<Request *>(opaque);
      return request->cancelled.load(std::memory_order_relaxed) ? 1 : 0;
    }

    static void work(uv_work_t *work_item) {
      auto *request = static_cast<Request *>(work_item->data);
      if (request->cancelled.load(std::memory_order_relaxed)) {
        request->cancel_before_work = true;
        return;
      }
      ensure_curl_global();
      CURL *curl = curl_easy_init();
      if (!curl) {
        request->curl_result = CURLE_FAILED_INIT;
        return;
      }
      BodyTarget body_target{&request->response.body,
                             request->input.max_body_bytes,
                             &request->body_too_large};
      request->curl_error.resize(CURL_ERROR_SIZE);
      curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, request->curl_error.data());
      curl_easy_setopt(curl, CURLOPT_URL, request->input.url.c_str());
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_target);
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
      curl_easy_setopt(curl, CURLOPT_HEADERDATA, &request->response.headers);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      const auto timeout_ms = request->input.timeout.count();
      curl_easy_setopt(
          curl, CURLOPT_TIMEOUT_MS,
          static_cast<long>(std::clamp<std::int64_t>(timeout_ms, 1, LONG_MAX)));
      curl_easy_setopt(
          curl, CURLOPT_CONNECTTIMEOUT_MS,
          static_cast<long>(std::clamp<std::int64_t>(timeout_ms, 1, LONG_MAX)));
      if (!request->input.user_agent.empty())
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         request->input.user_agent.c_str());

      for (const auto &[name, value] : request->input.headers) {
        const std::string line = name + ": " + value;
        request->curl_headers =
            curl_slist_append(request->curl_headers, line.c_str());
      }
      if (request->curl_headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request->curl_headers);

      if (request->input.route.proxy_url) {
        curl_easy_setopt(curl, CURLOPT_PROXY,
                         request->input.route.proxy_url->c_str());
        // An explicitly selected route is authoritative.  In particular,
        // do not let NO_PROXY turn proxy failure into a silent direct request.
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
      } else {
        // Direct is also explicit and should not be altered by ambient proxy
        // env.
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
      }

      request->curl_result = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
                        &request->response.status);
      if (request->curl_headers) {
        curl_slist_free_all(request->curl_headers);
        request->curl_headers = nullptr;
      }
      curl_easy_cleanup(curl);
    }

    static void after_work(uv_work_t *work_item, int status) {
      auto *request = static_cast<Request *>(work_item->data);
      auto state = request->keep_state_alive;
      if (request->curl_headers) {
        curl_slist_free_all(request->curl_headers);
        request->curl_headers = nullptr;
      }
      if (state->active)
        --state->active;
      Result<HttpResponse> result =
          std::unexpected(error(ErrorCode::network_error, "HTTP下载失败"));
      if (request->cancelled.load(std::memory_order_relaxed) ||
          status == UV_ECANCELED ||
          request->curl_result == CURLE_ABORTED_BY_CALLBACK ||
          request->cancel_before_work) {
        result = std::unexpected(error(ErrorCode::cancelled, "HTTP下载已取消"));
      } else if (request->body_too_large) {
        result = std::unexpected(
            error(ErrorCode::too_large, "HTTP响应正文超过大小限制"));
      } else if (request->curl_result == CURLE_OPERATION_TIMEDOUT) {
        result =
            std::unexpected(error(ErrorCode::timed_out, "HTTP下载超过时限"));
      } else if (request->curl_result == CURLE_OK) {
        result = std::move(request->response);
      } else {
        // CURLOPT_ERRORBUFFER is a NUL-terminated C string inside a fixed-size
        // buffer.  Copying the whole buffer leaks padding into RPC/JSON errors.
        const std::string detail =
            request->curl_error.empty() || request->curl_error.front() == '\0'
                ? curl_easy_strerror(request->curl_result)
                : request->curl_error.c_str();
        result = std::unexpected(
            error(ErrorCode::network_error, "HTTP下载失败: " + detail));
      }
      const auto found = state->requests.find(request->id);
      if (found != state->requests.end())
        state->finish(found->second, std::move(result));
      state->schedule();
    }

    Result<Id> fetch(HttpRequest input, Completion<HttpResponse> completion) {
      if (shutting_down)
        return std::unexpected(error(ErrorCode::busy, "HTTP客户端正在关闭"));
      if (input.url.empty() || !completion)
        return std::unexpected(
            error(ErrorCode::invalid_argument, "HTTP URL和完成回调不能为空"));
      if (input.timeout.count() <= 0)
        return std::unexpected(
            error(ErrorCode::invalid_argument, "HTTP超时必须大于零"));
      if (has_header_break(input.user_agent))
        return std::unexpected(
            error(ErrorCode::invalid_argument, "User-Agent含非法换行"));
      for (const auto &[name, value] : input.headers) {
        if (!valid_header_name(name) || has_header_break(value))
          return std::unexpected(
              error(ErrorCode::invalid_argument, "HTTP请求头格式无效"));
      }
      if (input.route.proxy_url &&
          !supported_proxy_scheme(*input.route.proxy_url))
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "下载代理必须使用HTTP或SOCKS协议"));
      const Id id = "http-" + std::to_string(next_id++);
      auto request = std::make_shared<Request>();
      request->work.data = request.get();
      request->id = id;
      request->keep_state_alive = shared_from_this();
      request->input = std::move(input);
      request->completion = std::move(completion);
      requests.emplace(id, request);
      pending.push_back(request);
      schedule();
      return id;
    }

    Result<void> cancel(const Id &id) {
      auto keep_alive = shared_from_this();
      const auto found = requests.find(id);
      if (found == requests.end())
        return std::unexpected(
            error(ErrorCode::not_found, "HTTP请求不存在", id));
      auto request = found->second;
      request->cancelled.store(true, std::memory_order_relaxed);
      const auto queued = std::find(pending.begin(), pending.end(), request);
      if (queued != pending.end()) {
        pending.erase(queued);
        finish(request, std::unexpected(
                            error(ErrorCode::cancelled, "HTTP下载已取消", id)));
        notify_drained();
      }
      return {};
    }

    void shutdown(std::function<void()> callback) {
      auto keep_alive = shared_from_this();
      if (callback && !drained)
        drained = std::move(callback);
      shutting_down = true;
      while (!pending.empty()) {
        auto request = pending.front();
        pending.pop_front();
        request->cancelled.store(true, std::memory_order_relaxed);
        finish(request,
               std::unexpected(error(ErrorCode::cancelled, "HTTP客户端关闭")));
      }
      for (auto &[id, request] : requests) {
        (void)id;
        request->cancelled.store(true, std::memory_order_relaxed);
      }
      notify_drained();
    }

    void schedule() {
      auto keep_alive = shared_from_this();
      while (!shutting_down && active < HttpClient::max_concurrent_requests &&
             !pending.empty()) {
        auto request = pending.front();
        pending.pop_front();
        ++active;
        const int status =
            uv_queue_work(loop, &request->work, work, after_work);
        if (status != 0) {
          --active;
          finish(request, std::unexpected(error(ErrorCode::io_error,
                                                "无法排入HTTP后台工作")));
        }
      }
      notify_drained();
    }

    void finish(const std::shared_ptr<Request> &request,
                Result<HttpResponse> result) {
      auto keep_alive = shared_from_this();
      auto found =
          std::find_if(requests.begin(), requests.end(), [&](const auto &item) {
            return item.second == request;
          });
      if (found == requests.end())
        return;
      auto completion = std::move(request->completion);
      request->keep_state_alive.reset();
      requests.erase(found);
      if (completion) {
        try {
          completion(std::move(result));
        } catch (...) { /* Never unwind through libuv. */
        }
      }
      notify_drained();
    }

    void notify_drained() {
      if (!shutting_down || !requests.empty() || !drained)
        return;
      auto callback = std::move(drained);
      try {
        callback();
      } catch (...) { /* Never unwind through libuv. */
      }
    }
  };

  explicit Impl(uv_loop_t &loop) : state(std::make_shared<State>(loop)) {}
  std::shared_ptr<State> state;
};

HttpClient::HttpClient(uv_loop_s &loop)
    : impl_(new Impl(reinterpret_cast<uv_loop_t &>(loop))) {}

HttpClient::~HttpClient() {
  if (!impl_)
    return;
  impl_->state->shutdown({});
  delete impl_;
  impl_ = nullptr;
}

Result<Id> HttpClient::fetch(HttpRequest request,
                             Completion<HttpResponse> completion) {
  return impl_->state->fetch(std::move(request), std::move(completion));
}

Result<void> HttpClient::cancel(const Id &request_id) {
  return impl_->state->cancel(request_id);
}

void HttpClient::shutdown(std::function<void()> drained) {
  impl_->state->shutdown(std::move(drained));
}

} // namespace xrm
