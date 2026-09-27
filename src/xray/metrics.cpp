/// @file
/// @brief Samples Xray traffic and observation data for the active run.
///
/// Missing endpoint/sample means unavailable or unknown, never fabricated zero
/// counters. Viewer demand controls polling; samples are tied to a run ID.

#include "xray/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <uv.h>

namespace xrm {
namespace {

std::optional<std::uint64_t> counter_value(const Json &value) {
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number >= 0)
      return static_cast<std::uint64_t>(number);
  }
  return std::nullopt;
}

std::optional<double> nonnegative_number(const Json &value) {
  if (!value.is_number())
    return std::nullopt;
  const auto number = value.get<double>();
  if (!std::isfinite(number) || number < 0)
    return std::nullopt;
  return number;
}

using CounterKey = std::pair<std::string, std::string>;

std::map<CounterKey, const TrafficCounter *>
counter_index(const MetricsSample &sample) {
  std::map<CounterKey, const TrafficCounter *> result;
  for (const auto &counter : sample.traffic)
    result[{counter.scope, counter.tag}] = &counter;
  return result;
}

} // namespace

Result<MetricsSample> parse_metrics(const Json &json, const Id &run_id,
                                    UnixMillis received_at) {
  // Preserve absent counters as optionals. A missing uplink/downlink field is
  // different from an observed numeric zero.
  if (!json.is_object()) {
    return std::unexpected(
        AppError{ErrorCode::protocol_error,
                 "Xray metrics response must be a JSON object", std::nullopt,
                 std::nullopt});
  }

  MetricsSample sample{run_id, received_at, {}, {}};
  if (const auto stats = json.find("stats");
      stats != json.end() && stats->is_object()) {
    for (auto scope = stats->begin(); scope != stats->end(); ++scope) {
      if (!scope.value().is_object())
        continue;
      for (auto tag = scope.value().begin(); tag != scope.value().end();
           ++tag) {
        if (!tag.value().is_object())
          continue;
        const auto uplink = tag.value().find("uplink");
        const auto downlink = tag.value().find("downlink");
        std::optional<std::uint64_t> up;
        std::optional<std::uint64_t> down;
        if (uplink != tag.value().end())
          up = counter_value(*uplink);
        if (downlink != tag.value().end())
          down = counter_value(*downlink);
        if (uplink != tag.value().end() || downlink != tag.value().end()) {
          sample.traffic.push_back({scope.key(), tag.key(), up, down});
        }
      }
    }
  }

  if (const auto observations = json.find("observatory");
      observations != json.end() && observations->is_object()) {
    for (auto item = observations->begin(); item != observations->end();
         ++item) {
      if (!item.value().is_object())
        continue;
      NodeObservation observation;
      observation.tag = item.key();
      if (const auto alive = item.value().find("alive");
          alive != item.value().end() && alive->is_boolean()) {
        observation.alive = alive->get<bool>();
      }
      if (const auto delay = item.value().find("delay");
          delay != item.value().end()) {
        observation.latency_ms = nonnegative_number(*delay);
      }
      sample.observations.push_back(std::move(observation));
    }
  }
  return sample;
}

std::vector<TrafficRate> compute_rates(const MetricsSample &previous,
                                       const MetricsSample &current,
                                       std::chrono::duration<double> elapsed) {
  const auto before = counter_index(previous);
  std::vector<TrafficRate> rates;
  rates.reserve(current.traffic.size());
  const bool same_run = previous.run_id == current.run_id;
  const double seconds = elapsed.count();
  const bool valid_elapsed = std::isfinite(seconds) && seconds > 0;
  for (const auto &counter : current.traffic) {
    TrafficRate rate{counter.scope, counter.tag, std::nullopt, std::nullopt};
    const auto prior = before.find({counter.scope, counter.tag});
    if (same_run && valid_elapsed && prior != before.end()) {
      const auto calculate =
          [seconds](const auto &old_value,
                    const auto &new_value) -> std::optional<double> {
        if (!old_value || !new_value || *new_value < *old_value)
          return std::nullopt;
        return static_cast<double>(*new_value - *old_value) / seconds;
      };
      rate.uplink_bytes_per_second =
          calculate(prior->second->uplink, counter.uplink);
      rate.downlink_bytes_per_second =
          calculate(prior->second->downlink, counter.downlink);
    }
    rates.push_back(std::move(rate));
  }
  return rates;
}

struct MetricsClient::Impl {
  struct Shared {
    std::optional<Id> run_id;
    std::optional<std::string> endpoint;
    std::size_t viewers = 0;
    std::optional<MetricsSample> latest;
    std::vector<TrafficRate> rates;
    std::optional<std::chrono::steady_clock::time_point> sampled_at;
    std::optional<Id> request_id;
    std::function<void(const MetricsSample &)> callback;
    bool shutting_down = false;
  };

  Impl(uv_loop_t &event_loop, HttpClient &http_client)
      : loop(&event_loop), http(http_client),
        shared(std::make_shared<Shared>()) {
    uv_timer_init(loop, &timer);
    timer.data = this;
  }

  static void tick(uv_timer_t *timer_handle) {
    auto *self = static_cast<Impl *>(timer_handle->data);
    auto state = self->shared;
    if (state->viewers == 0 || !state->run_id || !state->endpoint ||
        state->request_id)
      return;
    HttpRequest request;
    request.url = *state->endpoint;
    request.user_agent = "xrm";
    request.timeout = Milliseconds{1800};
    request.max_body_bytes = 4 * 1024 * 1024;
    std::weak_ptr<Shared> weak = state;
    const Id requested_run = *state->run_id;
    auto started = self->http.fetch(
        std::move(request), [weak, requested_run](auto response) {
          auto current = weak.lock();
          if (!current)
            return;
          current->request_id.reset();
          if (current->viewers == 0 || current->run_id != requested_run ||
              !response || response->status != 200)
            return;
          try {
            const auto body = Json::parse(response->body);
            const auto timestamp =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            auto parsed = parse_metrics(body, requested_run, timestamp);
            if (!parsed)
              return;
            const auto now = std::chrono::steady_clock::now();
            if (current->latest && current->sampled_at) {
              current->rates = compute_rates(*current->latest, *parsed,
                                             now - *current->sampled_at);
            } else {
              current->rates.clear();
            }
            current->latest = std::move(*parsed);
            current->sampled_at = now;
            if (current->callback)
              current->callback(*current->latest);
          } catch (const Json::exception &) {
            // A malformed or non-JSON response leaves the last sample intact;
            // its age is available to the caller for stale presentation.
          }
        });
    if (started)
      state->request_id = *started;
  }

  static void timer_close(uv_handle_t *handle) {
    auto *self = static_cast<Impl *>(handle->data);
    self->timer_closed = true;
    if (self->delete_on_close)
      delete self;
  }

  void close_timer() {
    auto &state = *shared;
    if (state.shutting_down)
      return;
    state.shutting_down = true;
    state.viewers = 0;
    uv_timer_stop(&timer);
    if (state.request_id)
      (void)http.cancel(*state.request_id);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&timer)))
      uv_close(reinterpret_cast<uv_handle_t *>(&timer), timer_close);
  }

  uv_loop_t *loop;
  HttpClient &http;
  uv_timer_t timer{};
  bool timer_closed = false;
  bool delete_on_close = false;
  std::shared_ptr<Shared> shared;
};

MetricsClient::MetricsClient(uv_loop_t &loop, HttpClient &http)
    : impl_(std::make_unique<Impl>(loop, http)) {}

MetricsClient::~MetricsClient() {
  if (!impl_)
    return;
  impl_->close_timer();
  if (impl_->timer_closed)
    return;
  impl_->delete_on_close = true;
  (void)impl_.release();
}

void MetricsClient::attach(const RunRecord &record,
                           std::optional<std::string> endpoint) {
  impl_->shared->run_id = record.run_id;
  impl_->shared->endpoint = std::move(endpoint);
  impl_->shared->latest.reset();
  impl_->shared->rates.clear();
  impl_->shared->sampled_at.reset();
}

void MetricsClient::set_viewer_count(std::size_t count) {
  // Do not poll a core solely because the daemon is running. One timer serves
  // all viewers, and the final viewer leaving cancels an outstanding sample.
  auto &state = *impl_->shared;
  if (state.shutting_down)
    return;
  const bool already_sampling = state.viewers > 0;
  state.viewers = count;
  if (count == 0) {
    uv_timer_stop(&impl_->timer);
    if (impl_->shared->request_id) {
      (void)impl_->http.cancel(*impl_->shared->request_id);
    }
    return;
  }
  if (!state.endpoint || !state.run_id || already_sampling)
    return;
  uv_timer_start(&impl_->timer, Impl::tick, 0, 2000);
}

std::optional<MetricsSample> MetricsClient::latest() const {
  return impl_->shared->latest;
}

MetricsView MetricsClient::view() const {
  const auto &state = *impl_->shared;
  MetricsView result;
  result.sample = state.latest;
  result.rates = state.rates;
  if (!state.run_id) {
    result.availability = MetricAvailability::unknown;
  } else if (!state.endpoint || !state.latest) {
    result.availability = MetricAvailability::unavailable;
  } else {
    const bool stale = !state.sampled_at ||
                       std::chrono::steady_clock::now() - *state.sampled_at >
                           std::chrono::seconds(6);
    result.availability =
        stale ? MetricAvailability::stale : MetricAvailability::available;
  }
  return result;
}

void MetricsClient::on_sample(
    std::function<void(const MetricsSample &)> callback) {
  impl_->shared->callback = std::move(callback);
}

void MetricsClient::stop() { set_viewer_count(0); }

void MetricsClient::shutdown() { impl_->close_timer(); }

} // namespace xrm
