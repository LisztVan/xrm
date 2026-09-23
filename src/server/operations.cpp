#include "server/operations.hpp"

#include "contract/wire.hpp"

namespace xrm {

Operations::Operations(Id boot_id) : boot_id_(std::move(boot_id)) {}

Id Operations::begin() {
  const auto id = boot_id_ + ":" + std::to_string(sequence_++);
  records_.emplace(id, Record{});
  return id;
}

void Operations::complete(const Id& id, Result<Json> result) {
  const auto found = records_.find(id);
  if (found == records_.end() || found->second.status != "running") return;
  if (result) {
    found->second.status = "succeeded";
    found->second.result = std::move(*result);
  } else {
    found->second.status = "failed";
    found->second.error = std::move(result.error());
  }
  terminal_.push_back(id);
  prune();
}

Result<Json> Operations::get(const Id& id) const {
  const auto found = records_.find(id);
  if (found == records_.end())
    return std::unexpected(AppError{ErrorCode::not_found, "operation未知或属于此前Server进程", id, std::nullopt});
  Json error = nullptr;
  if (found->second.error) error = error_to_json(*found->second.error);
  return Json{{"operation_id", id}, {"status", found->second.status},
              {"result", found->second.result ? *found->second.result : Json(nullptr)},
              {"error", std::move(error)}};
}

void Operations::prune() {
  while (terminal_.size() > 64) {
    records_.erase(terminal_.front());
    terminal_.pop_front();
  }
}

}  // namespace xrm
