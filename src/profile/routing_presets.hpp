#pragma once

#include "profile/profile_service.hpp"

#include <optional>
#include <set>
#include <string>

namespace xrm {

// Behavior was compared with v2rayN's builtin samples and InitBuiltinRouting
// at commit e1cb99cd6ec1c134ea8260814d2a27e8b0ab862e (GPL-3.0-or-later).
// This is an original, shorter rule set, not a copy of those sample files:
// public DNS exception lists are omitted, while UDP 443 block, private
// bypass, Google exception, mainland bypass, and final actions are explicit.
// "global" therefore still bypasses private destinations and blocks UDP 443.
enum class RoutingPreset { bypass_mainland, blacklist_proxy, global_proxy,
                           direct_only, custom_native };

struct RoutingProxyTarget {
  // "outboundTag" for a node/native outbound, "balancerTag" for a group.
  std::string native_key;
  std::string native_tag;
};

struct RoutingPresetRequest {
  RoutingPreset preset = RoutingPreset::custom_native;
  ModuleDocument routing;
  std::optional<RoutingProxyTarget> proxy;
  std::set<std::string> outbound_tags;
  std::set<std::string> balancer_tags;
  std::set<std::string> protected_inbound_tags;
  // These are labels verified from the selected resource version, not guesses
  // from a provider name or a successful HTTP response.
  std::set<std::string> geoip_labels;
  std::set<std::string> geosite_labels;
};

struct RoutingPresetPlan {
  ModuleEdit edit;
  Json native_before;
  Json native_after;
  std::set<std::string> required_geoip_labels;
  std::set<std::string> required_geosite_labels;
  bool changed = false;
};

// Pure plan. The caller displays the before/after JSON and saves edit through
// ProfileService only after explicit confirmation. This never edits a module,
// changes a selection, or applies a runtime config itself.
Result<RoutingPresetPlan> plan_routing_preset(const RoutingPresetRequest&);

}  // namespace xrm
