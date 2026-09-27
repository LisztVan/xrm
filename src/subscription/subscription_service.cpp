#include "subscription/subscription_service.hpp"

#include "subscription/parser.hpp"
#include "subscription/reconcile.hpp"

#include <uv.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string_view>
#include <utility>

namespace xrm {
namespace {

AppError service_error(ErrorCode code, std::string message, std::optional<std::string> subject = {}) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

UnixMillis now_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

UnixMillis interval_millis(std::chrono::seconds interval) {
  const auto value = interval.count();
  if (value <= 0) return 1;
  if (value > std::numeric_limits<UnixMillis>::max() / 1000)
    return std::numeric_limits<UnixMillis>::max();
  return value * 1000;
}

UnixMillis add_saturated(UnixMillis left, UnixMillis right) {
  if (right > 0 && left > std::numeric_limits<UnixMillis>::max() - right)
    return std::numeric_limits<UnixMillis>::max();
  return left + right;
}

std::string header_value(const HttpResponse& response, std::string_view name) {
  const auto found = response.headers.find(std::string(name));
  if (found == response.headers.end()) return {};
  std::string value = found->second;
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

bool source_information_name(const Subscription& source, std::string_view name) {
  if (name.empty() || name.size() > 512) return false;
  if (source.info_name_prefixes) {
    return std::any_of(source.info_name_prefixes->begin(), source.info_name_prefixes->end(),
        [&](const std::string& prefix) { return !prefix.empty() && name.starts_with(prefix); });
  }
  // Built-in classification is deliberately anchored and requires a label
  // separator: a real exit named “官网香港” must remain a connectable node.
  constexpr std::array<std::string_view, 4> prefixes{
      "剩余流量", "套餐到期", "官网", "距离重置"};
  for (const auto prefix : prefixes) {
    if (!name.starts_with(prefix)) continue;
    const auto suffix = name.substr(prefix.size());
    if (suffix.starts_with(":") || suffix.starts_with("：") ||
        suffix.starts_with(" ") || suffix.starts_with("=") ||
        suffix.starts_with("|") || suffix.starts_with("｜")) return true;
  }
  return false;
}

Id new_node_id() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char digits[] = "0123456789abcdef";
  Id id;
  id.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) id.push_back('-');
    id.push_back(digits[bytes[index] >> 4]);
    id.push_back(digits[bytes[index] & 0x0f]);
  }
  return id;
}

std::string issue_summary(const ParseIssue& issue) {
  return "item " + std::to_string(issue.item_index + 1) + " · " +
      issue.code + ": " + issue.message;
}

void redact_literal(std::string& message, std::string_view secret,
                    std::string_view replacement) {
  if (secret.empty()) return;
  std::size_t start = 0;
  while ((start = message.find(secret, start)) != std::string::npos) {
    message.replace(start, secret.size(), replacement);
    start += replacement.size();
  }
}

}  // namespace

struct SubscriptionService::Impl : std::enable_shared_from_this<Impl> {
  struct Batch {
    std::size_t pending = 0;
    BatchRefreshResult result;
    Completion<BatchRefreshResult> completion;
  };

  struct InFlight {
    Subscription source;
    SubscriptionState state;
    std::optional<Id> http_request;
    bool retried_without_cache_validator = false;
    std::vector<std::function<void(Result<RefreshReport>)>> waiters;
  };

  Impl(Store& data_store, HttpClient& client) : store(&data_store), http(&client) {}

  Store* store;
  HttpClient* http;
  std::uint64_t next_batch_id = 1;
  bool shutting_down = false;
  uv_loop_t* loop = nullptr;
  uv_timer_t timer{};
  bool timer_initialized = false;
  bool timer_closing = false;
  std::shared_ptr<Impl>* timer_keepalive = nullptr;
  std::map<Id, InFlight, std::less<>> in_flight;

  static void timer_closed(uv_handle_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    auto* retained = self->timer_keepalive;
    self->timer_keepalive = nullptr;
    delete retained;
  }

  void stop_timer() {
    if (!timer_initialized || timer_closing) return;
    timer_closing = true;
    uv_timer_stop(&timer);
    uv_close(reinterpret_cast<uv_handle_t*>(&timer), timer_closed);
  }

  void complete_batch(const std::shared_ptr<Batch>& batch,
                      const Id& source_id, Result<RefreshReport> result) {
    if (result) batch->result.completed.push_back(std::move(*result));
    else batch->result.failed.emplace_back(source_id, std::move(result.error()));
    if (batch->pending > 0) --batch->pending;
    if (batch->pending == 0 && batch->completion) {
      auto callback = std::move(batch->completion);
      try { callback(std::move(batch->result)); } catch (...) { /* async boundary */ }
    }
  }

  void complete_source(const Id& source_id, Result<RefreshReport> result) {
    const auto found = in_flight.find(source_id);
    if (found == in_flight.end()) return;
    auto waiters = std::move(found->second.waiters);
    in_flight.erase(found);
    for (auto& waiter : waiters) {
      try { waiter(result); } catch (...) { /* async boundary */ }
    }
    schedule_next();
  }

  void record_attempt_failure(const Id& source_id, ErrorCode code,
                              std::string message, std::string_view stage) {
    const auto found = in_flight.find(source_id);
    if (found == in_flight.end()) return;
    const auto source = found->second.source;
    redact_literal(message, source.url, "[subscription URL]");
    if (source.route.proxy_url)
      redact_literal(message, *source.route.proxy_url, "[proxy URL]");
    auto state = found->second.state;
    state.last_attempt = now_millis();
    state.last_result = "failed";
    state.last_error = message;
    state.last_error_stage = stage;
    RefreshReport report;
    report.source_id = source_id;
    report.malformed = state.last_malformed_count;
    report.unsupported = state.last_unsupported_count;
    SourceCommit commit{source.settings_revision, {}, state, report};
    auto saved = store->commit_source(source_id, commit);
    if (!saved) {
      complete_source(source_id, std::unexpected(saved.error()));
      return;
    }
    complete_source(source_id, std::unexpected(service_error(code, std::move(message), source_id)));
  }

  void persist_response(const Id& source_id, SubscriptionState state,
                        RefreshReport report, NodeChanges changes) {
    auto found = in_flight.find(source_id);
    if (found == in_flight.end()) return;
    const auto revision = found->second.source.settings_revision;
    SourceCommit commit{revision, std::move(changes), state, report};
    if (auto saved = store->commit_source(source_id, commit); !saved) {
      complete_source(source_id, std::unexpected(saved.error()));
      return;
    }
    complete_source(source_id, std::move(report));
  }

  Result<std::vector<Node>> source_nodes(const Id& source_id) const {
    std::vector<Node> nodes;
    std::size_t offset = 0;
    for (;;) {
      auto page = store->list_nodes(NodeQuery{.source_id = source_id, .include_missing = true},
                                    {offset, 1000});
      if (!page) return std::unexpected(page.error());
      nodes.insert(nodes.end(), std::make_move_iterator(page->items.begin()),
                   std::make_move_iterator(page->items.end()));
      if (!page->next_offset) break;
      offset = *page->next_offset;
    }
    return nodes;
  }

  bool has_success_cache(const Id& source_id, const SubscriptionState& state) const {
    if (!state.last_success) return false;
    auto nodes = store->list_nodes(NodeQuery{.source_id = source_id, .include_missing = true}, {0, 1});
    return nodes && nodes->total != 0;
  }

  void issue_request(const Id& source_id, bool conditional) {
    auto found = in_flight.find(source_id);
    if (found == in_flight.end()) return;
    HttpRequest request;
    request.url = found->second.source.url;
    request.headers = found->second.source.headers;
    request.user_agent = found->second.source.user_agent;
    request.route = found->second.source.route;
    if (conditional) {
      if (found->second.state.etag) request.headers.insert_or_assign("If-None-Match", *found->second.state.etag);
      if (found->second.state.last_modified)
        request.headers.insert_or_assign("If-Modified-Since", *found->second.state.last_modified);
    }
    std::weak_ptr<Impl> weak = shared_from_this();
    auto requested = http->fetch(std::move(request), [weak, source_id](Result<HttpResponse> response) mutable {
      if (auto self = weak.lock()) self->on_response(source_id, std::move(response));
    });
    if (!requested) {
      record_attempt_failure(source_id, requested.error().code, requested.error().message, "download");
      return;
    }
    found = in_flight.find(source_id);
    if (found != in_flight.end()) found->second.http_request = std::move(*requested);
  }

  void on_response(const Id& source_id, Result<HttpResponse> response) {
    auto found = in_flight.find(source_id);
    if (found == in_flight.end()) return;
    if (shutting_down) {
      complete_source(source_id, std::unexpected(service_error(ErrorCode::cancelled,
          "订阅服务正在关闭", source_id)));
      return;
    }
    const auto source = found->second.source;
    auto state = found->second.state;
    if (!response) {
      record_attempt_failure(source_id, response.error().code, response.error().message, "download");
      return;
    }
    if (response->status == 304) {
      if (has_success_cache(source_id, state)) {
        state.last_attempt = now_millis();
        state.last_result = "not_modified";
        state.last_error.reset();
        state.last_error_stage.reset();
        RefreshReport report;
        report.source_id = source_id;
        report.unsupported = state.last_unsupported_count;
        report.malformed = state.last_malformed_count;
        persist_response(source_id, std::move(state), std::move(report), {});
        return;
      }
      if (!found->second.retried_without_cache_validator) {
        found->second.retried_without_cache_validator = true;
        found->second.state.etag.reset();
        found->second.state.last_modified.reset();
        issue_request(source_id, false);
        return;
      }
      record_attempt_failure(source_id, ErrorCode::network_error,
                             "订阅返回304但本地没有有效成功缓存", "http");
      return;
    }
    if (response->status < 200 || response->status >= 300) {
      record_attempt_failure(source_id, ErrorCode::network_error,
                             "订阅服务器返回HTTP " + std::to_string(response->status), "http");
      return;
    }

    auto parsed = parse_subscription(response->body);
    if (!parsed) {
      record_attempt_failure(source_id, parsed.error().code, parsed.error().message, "parse");
      return;
    }
    std::vector<std::string> information;
    std::erase_if(parsed->nodes, [&](const ParsedNode& node) {
      if (!source_information_name(source, node.name)) return false;
      if (information.size() < 16 &&
          std::find(information.begin(), information.end(), node.name) == information.end())
        information.push_back(node.name);
      return true;
    });
    if (parsed->nodes.empty()) {
      auto state_after = state;
      state_after.last_attempt = now_millis();
      state_after.last_result = "failed";
      if (!information.empty()) state_after.reported_notices = std::move(information);
      RefreshReport report;
      report.source_id = source_id;
      for (const auto& issue : parsed->issues) {
        if (issue.kind == IssueKind::malformed) ++report.malformed;
        if (issue.kind == IssueKind::unsupported) ++report.unsupported;
      }
      report.partial = !parsed->issues.empty();
      state_after.last_malformed_count = report.malformed;
      state_after.last_unsupported_count = report.unsupported;
      std::string message = "订阅没有可用节点（malformed=" + std::to_string(report.malformed) +
          ", unsupported=" + std::to_string(report.unsupported) + "），旧库存已保留";
      if (!parsed->issues.empty()) message += "；首个问题：" + issue_summary(parsed->issues.front());
      redact_literal(message, source.url, "[subscription URL]");
      state_after.last_error = message;
      state_after.last_error_stage = "parse";
      SourceCommit commit{source.settings_revision, {}, state_after, report};
      auto saved = store->commit_source(source_id, commit);
      if (!saved) complete_source(source_id, std::unexpected(saved.error()));
      else complete_source(source_id, std::unexpected(service_error(
          ErrorCode::invalid_argument, std::move(message), source_id)));
      return;
    }

    const bool structurally_complete = parsed->structurally_complete;
    auto old_nodes = source_nodes(source_id);
    if (!old_nodes) {
      complete_source(source_id, std::unexpected(old_nodes.error()));
      return;
    }
    std::optional<std::string> first_issue;
    if (!parsed->issues.empty()) {
      first_issue = issue_summary(parsed->issues.front());
      redact_literal(*first_issue, source.url, "[subscription URL]");
    }
    auto reconciled = reconcile({source, std::move(*old_nodes), std::move(*parsed)}, new_node_id);
    if (!reconciled) {
      record_attempt_failure(source_id, reconciled.error().code, reconciled.error().message, "reconcile");
      return;
    }
    auto report = std::move(reconciled->report);
    report.partial = report.partial || !structurally_complete;
    report.first_issue = std::move(first_issue);
    SubscriptionState next_state = state;
    next_state.last_attempt = now_millis();
    next_state.last_error.reset();
    next_state.last_error_stage.reset();
    if (structurally_complete && !report.partial)
      next_state.reported_notices = std::move(information);
    else if (!information.empty())
      next_state.reported_notices = std::move(information);
    // Providers commonly publish usage as response metadata rather than as
    // synthetic proxy nodes. Keep the original bounded header for display;
    // no numbers are inferred when the provider did not send it.
    const auto usage = header_value(*response, "subscription-userinfo");
    if (usage.empty()) next_state.reported_usage.reset();
    else if (usage.size() <= 4096) next_state.reported_usage = usage;
    const bool complete = structurally_complete && !report.partial &&
                          report.malformed == 0 && report.unsupported == 0;
    if (complete) {
      next_state.last_success = next_state.last_attempt;
      const auto etag = header_value(*response, "etag");
      const auto modified = header_value(*response, "last-modified");
      if (etag.empty()) next_state.etag.reset(); else next_state.etag = etag;
      if (modified.empty()) next_state.last_modified.reset(); else next_state.last_modified = modified;
      next_state.last_result = "success";
    } else {
      next_state.last_result = "partial";
      if (report.first_issue) next_state.last_error = report.first_issue;
      else next_state.last_error = "The source document was incomplete; prior inventory may remain.";
      next_state.last_error_stage = "parse";
    }
    next_state.last_malformed_count = report.malformed;
    next_state.last_unsupported_count = report.unsupported;
    persist_response(source_id, std::move(next_state), report,
                     std::move(reconciled->changes));
  }

  Result<Id> refresh(std::vector<Id> source_ids,
                     Completion<BatchRefreshResult> completion) {
    if (shutting_down)
      return std::unexpected(service_error(ErrorCode::busy, "订阅服务正在关闭"));
    if (!completion)
      return std::unexpected(service_error(ErrorCode::invalid_argument, "刷新完成回调不能为空"));
    if (source_ids.empty()) {
      auto sources = store->list_sources();
      if (!sources) return std::unexpected(sources.error());
      for (const auto& source : *sources) source_ids.push_back(source.id);
    }
    std::set<Id> unique(source_ids.begin(), source_ids.end());
    auto batch = std::make_shared<Batch>();
    batch->pending = unique.size();
    batch->completion = std::move(completion);
    const Id batch_id = "subscription-refresh-" + std::to_string(next_batch_id++);
    if (unique.empty()) {
      auto callback = std::move(batch->completion);
      try { callback(std::move(batch->result)); } catch (...) { /* async boundary */ }
      return batch_id;
    }
    for (const auto& id : unique) {
      auto finish_one = [self = shared_from_this(), batch, id](Result<RefreshReport> result) mutable {
        self->complete_batch(batch, id, std::move(result));
      };
      if (auto found = in_flight.find(id); found != in_flight.end()) {
        found->second.waiters.push_back(std::move(finish_one));
        continue;
      }
      auto source = store->get_source(id);
      if (!source) {
        finish_one(std::unexpected(source.error()));
        continue;
      }
      auto state = store->get_source_state(id);
      if (!state) {
        finish_one(std::unexpected(state.error()));
        continue;
      }
      InFlight request{std::move(*source), std::move(*state), std::nullopt, false, {}};
      request.waiters.push_back(std::move(finish_one));
      in_flight.emplace(id, std::move(request));
      issue_request(id, true);
    }
    return batch_id;
  }

  static void timer_fired(uv_timer_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    if (!self || self->shutting_down) return;
    auto sources = self->store->list_sources();
    if (sources) {
      const auto now = now_millis();
      std::vector<Id> due;
      for (const auto& source : *sources) {
        if (!source.auto_refresh || self->in_flight.contains(source.id)) continue;
        auto state = self->store->get_source_state(source.id);
        if (!state) continue;
        const auto next = add_saturated(state->last_attempt.value_or(0), interval_millis(source.interval));
        if (!state->last_attempt || next <= now) due.push_back(source.id);
      }
      if (!due.empty()) (void)self->refresh(std::move(due), [](Result<BatchRefreshResult>) {});
    }
    self->schedule_next();
  }

  void schedule_next() {
    if (!timer_initialized || timer_closing || shutting_down || !loop) return;
    auto sources = store->list_sources();
    if (!sources) {
      uv_timer_start(&timer, timer_fired, 60000, 0);
      return;
    }
    const auto now = now_millis();
    std::optional<UnixMillis> earliest;
    for (const auto& source : *sources) {
      if (!source.auto_refresh || in_flight.contains(source.id)) continue;
      auto state = store->get_source_state(source.id);
      if (!state) continue;
      const UnixMillis due = state->last_attempt
          ? add_saturated(*state->last_attempt, interval_millis(source.interval))
          : now;
      if (!earliest || due < *earliest) earliest = due;
    }
    if (!earliest) {
      uv_timer_start(&timer, timer_fired, 60000, 0);
      return;
    }
    const auto delay = *earliest <= now ? 0 : static_cast<std::uint64_t>(*earliest - now);
    uv_timer_start(&timer, timer_fired, delay, 0);
  }
};

SubscriptionService::SubscriptionService(Store& store, HttpClient& http)
    : impl_(std::make_shared<Impl>(store, http)) {}

SubscriptionService::~SubscriptionService() {
  if (!impl_) return;
  impl_->shutting_down = true;
  impl_->stop_timer();
  std::vector<Id> requests;
  for (const auto& [source_id, request] : impl_->in_flight) {
    (void)source_id;
    if (request.http_request) requests.push_back(*request.http_request);
  }
  impl_->in_flight.clear();
  for (const auto& request : requests) (void)impl_->http->cancel(request);
}

Result<Id> SubscriptionService::refresh(std::vector<Id> source_ids,
                                        Completion<BatchRefreshResult> completion) {
  return impl_->refresh(std::move(source_ids), std::move(completion));
}

void SubscriptionService::start_schedule(uv_loop_s& loop) {
  if (!impl_ || impl_->shutting_down || impl_->timer_initialized) return;
  impl_->loop = reinterpret_cast<uv_loop_t*>(&loop);
  if (uv_timer_init(impl_->loop, &impl_->timer) != 0) return;
  impl_->timer_initialized = true;
  impl_->timer.data = impl_.get();
  impl_->timer_keepalive = new std::shared_ptr<Impl>(impl_);
  impl_->schedule_next();
}

void SubscriptionService::stop_schedule() {
  if (impl_) impl_->stop_timer();
}

}  // namespace xrm
