// XRM: Declares subscription refresh operations.

#pragma once

#include "infra/http.hpp"
#include "infra/store.hpp"

#include <memory>

namespace xrm {

struct BatchRefreshResult {
  std::vector<RefreshReport> completed;
  std::vector<std::pair<Id, AppError>> failed;
};

class SubscriptionService {
public:
  SubscriptionService(Store &store, HttpClient &http);
  ~SubscriptionService();
  SubscriptionService(const SubscriptionService &) = delete;
  SubscriptionService &operator=(const SubscriptionService &) = delete;

  // Refresh only reconciles independent node inventory and source status; it
  // does not publish runtime files or restart the owned core.
  Result<Id> refresh(std::vector<Id> source_ids,
                     Completion<BatchRefreshResult> completion);
  void start_schedule(uv_loop_s &loop);
  void stop_schedule();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace xrm
