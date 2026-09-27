/// @file
/// @brief Declares native-module form descriptors and draft binding.

#pragma once

#include "tui/components/widgets.hpp"
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace xrm::tui::forms {
/// @brief Binds Visual forms to one caller-owned native wrapper draft.
///
/// The host owns Profile identity, Edit permission, overlay lifetime, Source
/// persistence, and Runtime publication. These forms change only *draft.
struct NativeFormOptions {
  std::string module;
  std::string *draft = nullptr;
  bool readonly = true;
  std::vector<ui::Choice> outbound_tags, balancer_tags, inbound_tags;
  std::function<void(ftxui::Component)> open_dialog;
  std::function<void()> close_dialog;
  std::function<void()> changed;
  std::vector<ui::Choice> bound_groups;
  std::function<void()> open_groups;
};
ftxui::Component MakeNativeForm(NativeFormOptions options);

// Concrete native object forms are also usable by node's streamSettings editor.
// kind: dns-server, routing-rule, balancer, inbound, outbound, sniffing, mux,
// stream, tls, reality, sockopt, dns, log, api, policy-system, policy-level,
// metrics, observatory, burstObservatory, ping, fakedns, geodata, asset,
// version.
std::shared_ptr<ui::Form> MakeObjectForm(
    const std::string &kind, const nlohmann::json &value,
    const NativeFormOptions &options,
    std::function<std::optional<std::string>(const nlohmann::json &)> accept,
    std::function<void()> cancel, bool view_only = false);

// Inspectable fixed native mappings; not a schema/validation framework.
std::vector<std::string> NativeFormKinds();
} // namespace xrm::tui::forms
