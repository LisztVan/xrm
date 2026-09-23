#pragma once

#include "infra/http.hpp"
#include "infra/store.hpp"

namespace xrm {

struct BatchRefreshResult {
  std::vector<RefreshReport> completed;
  std::vector<std::pair<Id, AppError>> failed;
};

class SubscriptionService {
 public:
  SubscriptionService(Store& store, HttpClient& http);
  ~SubscriptionService();
  SubscriptionService(const SubscriptionService&) = delete;
  SubscriptionService& operator=(const SubscriptionService&) = delete;

  Result<Id> refresh(std::vector<Id> source_ids,
                     Completion<BatchRefreshResult> completion);
  void start_schedule(uv_loop_s& loop);
  void stop_schedule();

 private:
  Store* store_;
  HttpClient* http_;
};

}  // namespace xrm
