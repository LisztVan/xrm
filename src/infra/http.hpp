// XRM: Declares the HTTP download interface.

#pragma once

#include "model/subscription.hpp"

#include <cstddef>
#include <map>
#include <string>

struct uv_loop_s;

namespace xrm {

struct HttpRequest {
  std::string url;
  std::map<std::string, std::string> headers;
  std::string user_agent;
  DownloadRoute route;
  Milliseconds timeout{30000};
  std::size_t max_body_bytes = 16 * 1024 * 1024;
};

struct HttpResponse {
  long status = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};

class HttpClient {
public:
  static constexpr std::size_t max_concurrent_requests = 3;
  explicit HttpClient(uv_loop_s &loop);
  ~HttpClient();
  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  Result<Id> fetch(HttpRequest request, Completion<HttpResponse> completion);
  Result<void> cancel(const Id &request_id);
  void shutdown(std::function<void()> drained);

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace xrm
