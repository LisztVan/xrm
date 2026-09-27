/// @file
/// @brief Declares Xray control operations.

#pragma once

#include "infra/process.hpp"

namespace xrm {

struct ControlTarget {
  RunSpec run;
  std::string endpoint;
  Milliseconds timeout{3000};
};
struct OverrideResult {
  // Only a read-back observation can confirm the requested override.
  bool confirmed = false;
  std::optional<std::string> actual_outbound;
};
/// @brief Uses bounded Xray API helper commands against an active endpoint.
///
/// It does not own the active core process. Writes read back the balancer
/// state so a timeout or ambiguous reply is not reported as confirmed.
class XrayControl {
public:
  explicit XrayControl(ProcessService &);
  Result<Id> balancer_info(ControlTarget, std::string balancer_tag,
                           Completion<Json>);
  Result<Id> override_outbound(ControlTarget, std::string balancer_tag,
                               std::string outbound_tag,
                               Completion<OverrideResult>);
  Result<Id> clear_override(ControlTarget, std::string balancer_tag,
                            Completion<OverrideResult>);

private:
  ProcessService &process_;
};

} // namespace xrm
