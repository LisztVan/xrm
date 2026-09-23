#pragma once

#include "infra/process.hpp"

namespace xrm {

struct ControlTarget { RunSpec run; std::string endpoint; Milliseconds timeout{3000}; };
struct OverrideResult { bool confirmed = false; std::optional<std::string> actual_outbound; };
class XrayControl {
 public:
  explicit XrayControl(ProcessService&);
  Result<Id> balancer_info(ControlTarget, std::string balancer_tag, Completion<Json>);
  Result<Id> override_outbound(ControlTarget, std::string balancer_tag,
                               std::string outbound_tag, Completion<OverrideResult>);
  Result<Id> clear_override(ControlTarget, std::string balancer_tag,
                            Completion<OverrideResult>);
 private:
  ProcessService& process_;
};

}  // namespace xrm
