#include "profile/routing_presets.hpp"

#include <algorithm>
#include <string_view>

namespace xrm {
namespace {

AppError invalid(std::string message, std::string subject = "routing") {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject), std::nullopt};
}

Json rule(std::string_view key, Json condition, std::string_view destination_key,
          std::string_view destination_tag) {
  return Json{{"type", "field"}, {std::string(key), std::move(condition)},
              {std::string(destination_key), std::string(destination_tag)}};
}

Json route_all(std::string_view destination_key, std::string_view destination_tag) {
  return rule("network", "tcp,udp", destination_key, destination_tag);
}

bool is_protected(const Json& item, const std::set<std::string>& inbound_tags) {
  if (!item.is_object()) return false;
  auto found = item.find("inboundTag");
  if (found == item.end() || !found->is_array()) return false;
  for (const auto& tag : *found)
    if (tag.is_string() && inbound_tags.contains(tag.get<std::string>())) return true;
  return false;
}

Result<void> require_target(const RoutingPresetRequest& input) {
  if (!input.proxy || input.proxy->native_tag.empty())
    return std::unexpected(invalid("分流预设需要先选择有效节点或策略组", "selected target"));
  if (input.proxy->native_key == "outboundTag") {
    if (!input.outbound_tags.contains(input.proxy->native_tag))
      return std::unexpected(invalid("代理出站不在当前可用范围", input.proxy->native_tag));
  } else if (input.proxy->native_key == "balancerTag") {
    if (!input.balancer_tags.contains(input.proxy->native_tag))
      return std::unexpected(invalid("策略组不在当前 Profile 中", input.proxy->native_tag));
  } else {
    return std::unexpected(invalid("分流目标必须是原生 outboundTag 或 balancerTag",
                                   input.proxy->native_key));
  }
  return {};
}

Result<void> require_labels(const RoutingPresetRequest& input,
                            const RoutingPresetPlan& plan) {
  for (const auto& tag : plan.required_geoip_labels)
    if (!input.geoip_labels.contains(tag))
      return std::unexpected(invalid("所选 GeoIP 资源缺少分流所需标签", tag));
  for (const auto& tag : plan.required_geosite_labels)
    if (!input.geosite_labels.contains(tag))
      return std::unexpected(invalid("所选 GeoSite 资源缺少分流所需标签", tag));
  return {};
}

}  // namespace

Result<RoutingPresetPlan> plan_routing_preset(const RoutingPresetRequest& input) {
  if (input.routing.key != "routing")
    return std::unexpected(invalid("分流预设只修改 routing 原生模块", input.routing.key));
  Json source;
  try { source = Json::parse(input.routing.source_text); }
  catch (const Json::exception& error) {
    return std::unexpected(invalid("routing 原文不是有效 JSON：" + std::string(error.what())));
  }
  if (!source.is_object() || !source.contains("routing") || !source["routing"].is_object())
    return std::unexpected(invalid("routing 文档缺少原生对象根键"));
  auto& native = source["routing"];
  if (native.contains("rules") && !native["rules"].is_array())
    return std::unexpected(invalid("routing.rules 必须是数组"));
  if (native.contains("balancers") && !native["balancers"].is_array())
    return std::unexpected(invalid("routing.balancers 必须是数组"));

  RoutingPresetPlan plan;
  plan.native_before = source;
  if (input.preset == RoutingPreset::custom_native) {
    plan.native_after = source;
    plan.edit = ModuleEdit{"routing", {input.routing.source_text}, input.routing.source_text};
    return plan;
  }

  const bool needs_proxy = input.preset != RoutingPreset::direct_only;
  if (needs_proxy) {
    auto target = require_target(input);
    if (!target) return std::unexpected(target.error());
  }
  const bool needs_block = input.preset != RoutingPreset::direct_only;
  if (!input.outbound_tags.contains("direct") ||
      (needs_block && !input.outbound_tags.contains("block")))
    return std::unexpected(invalid("分流预设需要已有的 direct 与必要的 block 原生出站"));

  Json next_rules = Json::array();
  if (native.contains("rules"))
    for (const auto& item : native["rules"])
      if (is_protected(item, input.protected_inbound_tags)) next_rules.push_back(item);

  auto add_proxy = [&](std::string_view key, Json condition) {
    next_rules.push_back(rule(key, std::move(condition), input.proxy->native_key,
                              input.proxy->native_tag));
  };
  auto add_direct = [&](std::string_view key, Json condition) {
    next_rules.push_back(rule(key, std::move(condition), "outboundTag", "direct"));
  };
  if (needs_block)
    next_rules.push_back(Json{{"type", "field"}, {"network", "udp"},
                              {"port", "443"}, {"outboundTag", "block"}});

  if (input.preset == RoutingPreset::bypass_mainland ||
      input.preset == RoutingPreset::blacklist_proxy) {
    add_proxy("domain", Json::array({"geosite:google"}));
    plan.required_geosite_labels.insert("google");
  }
  if (input.preset != RoutingPreset::direct_only) {
    add_direct("ip", Json::array({"geoip:private"}));
    add_direct("domain", Json::array({"geosite:private"}));
    plan.required_geoip_labels.insert("private");
    plan.required_geosite_labels.insert("private");
  }
  if (input.preset == RoutingPreset::bypass_mainland) {
    add_direct("ip", Json::array({"geoip:cn"}));
    add_direct("domain", Json::array({"geosite:cn"}));
    plan.required_geoip_labels.insert("cn");
    plan.required_geosite_labels.insert("cn");
  } else if (input.preset == RoutingPreset::blacklist_proxy) {
    add_direct("protocol", Json::array({"bittorrent"}));
    add_proxy("ip", Json::array({"geoip:facebook", "geoip:fastly", "geoip:google",
                                  "geoip:netflix", "geoip:telegram", "geoip:twitter"}));
    add_proxy("domain", Json::array({"geosite:gfw", "geosite:greatfire"}));
    plan.required_geoip_labels.insert({"facebook", "fastly", "google", "netflix",
                                       "telegram", "twitter"});
    plan.required_geosite_labels.insert({"gfw", "greatfire"});
  }

  if (input.preset == RoutingPreset::direct_only ||
      input.preset == RoutingPreset::blacklist_proxy)
    next_rules.push_back(route_all("outboundTag", "direct"));
  else
    next_rules.push_back(route_all(input.proxy->native_key, input.proxy->native_tag));

  auto requirements = require_labels(input, plan);
  if (!requirements) return std::unexpected(requirements.error());
  native["rules"] = std::move(next_rules);
  plan.native_after = source;
  plan.changed = plan.native_before != plan.native_after;
  plan.edit = ModuleEdit{"routing", {input.routing.source_text},
                         plan.changed ? source.dump(2) + "\n" : input.routing.source_text};
  return plan;
}

}  // namespace xrm
