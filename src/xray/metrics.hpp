#pragma once

#include "infra/http.hpp"
#include "model/runtime.hpp"

#include <memory>

namespace xrm {

struct TrafficCounter {
  std::string scope, tag;
  std::optional<std::uint64_t> uplink, downlink;
};
struct NodeObservation {
  std::string tag;
  std::optional<bool> alive;
  std::optional<double> latency_ms;
};
struct MetricsSample {
  Id run_id;
  UnixMillis received_at = 0;
  std::vector<TrafficCounter> traffic;
  std::vector<NodeObservation> observations;
};
struct TrafficRate {
  std::string scope, tag;
  std::optional<double> uplink_bytes_per_second, downlink_bytes_per_second;
};
enum class MetricAvailability { available, unavailable, stale, unknown };
struct MetricsView {
  MetricAvailability availability = MetricAvailability::unknown;
  std::optional<MetricsSample> sample;
  std::vector<TrafficRate> rates;
};
Result<MetricsSample> parse_metrics(const Json&, const Id& run_id, UnixMillis);
std::vector<TrafficRate> compute_rates(const MetricsSample&, const MetricsSample&,
                                      std::chrono::duration<double> elapsed);
class MetricsClient {
 public:
  MetricsClient(uv_loop_t&, HttpClient&);
  ~MetricsClient();
  MetricsClient(const MetricsClient&) = delete;
  MetricsClient& operator=(const MetricsClient&) = delete;
  void attach(const RunRecord&, std::optional<std::string> endpoint);
  void set_viewer_count(std::size_t);
  std::optional<MetricsSample> latest() const;
  MetricsView view() const;
  void on_sample(std::function<void(const MetricsSample&)>);
  void stop();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm
