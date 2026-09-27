/// @file
/// @brief Declares subscription refresh operations.

#pragma once

#include "infra/http.hpp"
#include "infra/store.hpp"

#include <memory>

namespace xrm {

struct BatchRefreshResult {
  // A batch can finish with both successful and failed sources.
  std::vector<RefreshReport> completed;
  std::vector<std::pair<Id, AppError>> failed;
};

/// @brief Refreshes independent node inventory without publishing Runtime.
///
/// Each source is reconciled against its saved revision. Failed or unusable
/// responses retain the previously usable nodes; callers receive the concrete
/// per-source failure through BatchRefreshResult.
class SubscriptionService {
public:
  SubscriptionService(Store &store, HttpClient &http);
  ~SubscriptionService();
  SubscriptionService(const SubscriptionService &) = delete;
  SubscriptionService &operator=(const SubscriptionService &) = delete;

  /// Starts a batch; the callback reports every source's final result.
  /// A successful admission is not a successful refresh.
  Result<Id> refresh(std::vector<Id> source_ids,
                     Completion<BatchRefreshResult> completion);
  /// Runs saved automatic refresh intervals on the supplied daemon loop.
  void start_schedule(uv_loop_s &loop);
  void stop_schedule();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace xrm
