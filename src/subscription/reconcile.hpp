// XRM: Declares non-destructive subscription node reconciliation.

#pragma once

#include "model/subscription.hpp"

#include <functional>

namespace xrm {

struct ReconcileInput {
  Subscription source;
  std::vector<Node> old_nodes;
  ParseReport parsed;
};

struct ReconcileResult {
  NodeChanges changes;
  RefreshReport report;
};

Result<ReconcileResult> reconcile(const ReconcileInput &input,
                                  const std::function<Id()> &make_id);

} // namespace xrm
