#pragma once

#include "model/node.hpp"
#include "model/runtime.hpp"
#include "model/subscription.hpp"
#include "xray/metrics.hpp"
#include "xray/installer.hpp"

#include <nlohmann/json.hpp>

namespace xrm {

struct SubscriptionSummary {
  Id id;
  std::string name;
  bool enabled = true;
  std::string last_result;
  std::optional<UnixMillis> last_attempt;
  std::optional<UnixMillis> last_success;
  std::size_t node_count = 0;
  std::size_t unsupported_count = 0;
  std::size_t malformed_count = 0;
};

inline void to_json(Json& json, const SubscriptionSummary& value) {
  json = Json{{"id", value.id}, {"name", value.name}, {"enabled", value.enabled},
              {"last_result", value.last_result}, {"last_attempt", value.last_attempt},
              {"last_success", value.last_success}, {"node_count", value.node_count},
              {"unsupported_count", value.unsupported_count},
              {"malformed_count", value.malformed_count}};
}

inline void from_json(const Json& json, SubscriptionSummary& value) {
  value.id = json.at("id").get<Id>();
  value.name = json.at("name").get<std::string>();
  value.enabled = json.at("enabled").get<bool>();
  value.last_result = json.at("last_result").get<std::string>();
  if (!json.at("last_attempt").is_null()) value.last_attempt = json.at("last_attempt").get<UnixMillis>();
  if (!json.at("last_success").is_null()) value.last_success = json.at("last_success").get<UnixMillis>();
  value.node_count = json.at("node_count").get<std::size_t>();
  value.unsupported_count = json.at("unsupported_count").get<std::size_t>();
  value.malformed_count = json.at("malformed_count").get<std::size_t>();
}

struct NodeSummary {
  Id id;
  std::string name;
  std::vector<std::string> labels;
  bool enabled = true;
  bool source_missing = false;
  std::string protocol;
  std::string support;
};

struct NodeFormEditDto { Id id; std::string field; Json value; };
inline void to_json(Json& json, const NodeFormEditDto& value) {
  json = Json{{"id", value.id}, {"field", value.field}, {"value", value.value}};
}
inline void from_json(const Json& json, NodeFormEditDto& value) {
  value.id = json.at("id").get<Id>();
  value.field = json.at("field").get<std::string>();
  value.value = json.at("value");
}

inline void to_json(Json& json, const NodeSummary& value) {
  json = Json{{"id", value.id},
              {"name", value.name},
              {"labels", value.labels},
              {"enabled", value.enabled},
              {"source_missing", value.source_missing},
              {"protocol", value.protocol},
              {"support", value.support}};
}

inline void from_json(const Json& json, NodeSummary& value) {
  value.id = json.at("id").get<std::string>();
  value.name = json.at("name").get<std::string>();
  value.labels = json.at("labels").get<std::vector<std::string>>();
  value.enabled = json.at("enabled").get<bool>();
  value.source_missing = json.at("source_missing").get<bool>();
  value.protocol = json.at("protocol").get<std::string>();
  value.support = json.value("support", "raw-only");
}

inline NodeSummary summarize_node(const Node& node) {
  const auto name = node.alias.value_or(node.original_name);
  std::string protocol = "unknown";
  std::string support = "raw-only";
  if (const auto* raw = std::get_if<RawOutbound>(&node.settings)) {
    if (raw->value.is_object() && raw->value.contains("protocol") &&
        raw->value.at("protocol").is_string()) {
      protocol = raw->value.at("protocol").get<std::string>();
    }
  } else if (const auto* stream = std::get_if<StreamNode>(&node.settings)) {
    protocol = std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, VlessSettings>) return "vless";
      if constexpr (std::is_same_v<T, VmessSettings>) return "vmess";
      if constexpr (std::is_same_v<T, TrojanSettings>) return "trojan";
      if constexpr (std::is_same_v<T, ShadowsocksSettings>) return "shadowsocks";
      if constexpr (std::is_same_v<T, Hysteria2Settings>) return "hysteria2";
    }, stream->protocol);
    support = "typed-uri";
  }
  return {node.id, name, node.labels, node.enabled, node.source_missing,
          std::move(protocol), std::move(support)};
}

struct ProfileDto { Id id; std::string name; Id core_id; };
inline void to_json(Json& json, const ProfileDto& value) {
  json = Json{{"id", value.id}, {"name", value.name}, {"core_id", value.core_id}};
}

struct ModuleDto {
  std::string key;
  std::string filename;
  bool included = false;
  std::optional<std::string> text;
};
inline void to_json(Json& json, const ModuleDto& value) {
  json = Json{{"module", value.key}, {"filename", value.filename},
              {"included", value.included}, {"text", value.text}};
}

struct OperationDto { std::string status; std::optional<std::string> operation_id; };
inline void to_json(Json& json, const OperationDto& value) {
  json = Json{{"status", value.status}, {"operation_id", value.operation_id}};
}

struct GroupDto {
  GroupBinding binding;
  std::vector<NodeSummary> candidates;
};
inline void to_json(Json& json, const GroupDto& value) {
  json = Json{{"binding", value.binding}, {"candidates", value.candidates}};
}
struct RuntimeMetricsDto {
  MetricAvailability availability = MetricAvailability::unknown;
  std::optional<MetricsSample> sample;
  std::vector<TrafficRate> rates;
};
inline void to_json(Json& json, const RuntimeMetricsDto& value) {
  const auto availability = [](MetricAvailability state) {
    switch (state) {
      case MetricAvailability::available: return "available";
      case MetricAvailability::unavailable: return "unavailable";
      case MetricAvailability::stale: return "stale";
      case MetricAvailability::unknown: return "unknown";
    }
    return "unknown";
  };
  json = Json{{"availability", availability(value.availability)},
              {"run_id", value.sample ? Json(value.sample->run_id) : Json(nullptr)},
              {"received_at", value.sample ? Json(value.sample->received_at) : Json(nullptr)},
              {"rates", Json::array()}};
  for (const auto& rate : value.rates)
    json["rates"].push_back(Json{{"scope", rate.scope}, {"tag", rate.tag},
        {"uplink_bytes_per_second", rate.uplink_bytes_per_second ? Json(*rate.uplink_bytes_per_second) : Json(nullptr)},
        {"downlink_bytes_per_second", rate.downlink_bytes_per_second ? Json(*rate.downlink_bytes_per_second) : Json(nullptr)}});
}
struct GroupOverrideDto { std::string group_tag; std::optional<std::string> outbound_tag; bool clear = false; };
inline void to_json(Json& json, const GroupOverrideDto& value) {
  json = Json{{"group_tag", value.group_tag}, {"outbound_tag", value.outbound_tag}, {"clear", value.clear}};
}
struct GroupOverrideResultDto { std::string status; std::optional<std::string> actual_outbound; };
inline void to_json(Json& json, const GroupOverrideResultDto& value) {
  json = Json{{"status", value.status}, {"actual_outbound", value.actual_outbound}};
}

struct CoreReleasesRequestDto { bool include_prerelease = false; };
inline void to_json(Json& json, const CoreReleasesRequestDto& value) {
  json = Json{{"include_prerelease", value.include_prerelease}};
}
inline void from_json(const Json& json, CoreReleasesRequestDto& value) {
  value.include_prerelease = json.value("include_prerelease", false);
}
struct CoreReleaseListDto { std::vector<CoreRelease> items; };
inline void to_json(Json& json, const CoreReleaseListDto& value) {
  json = Json{{"items", value.items}};
}
struct CoreInstallRequestDto { InstallRequest request; };
inline void to_json(Json& json, const CoreInstallRequestDto& value) {
  json = Json{{"request", value.request}};
}
inline void from_json(const Json& json, CoreInstallRequestDto& value) {
  value.request = json.at("request").get<InstallRequest>();
}
struct CoreInstallResultDto { CoreInstall install; std::string status; };
inline void to_json(Json& json, const CoreInstallResultDto& value) {
  json = Json{{"install", value.install}, {"status", value.status}};
}
struct CoreUseRequestDto { Id profile_id; Id core_id; };
inline void to_json(Json& json, const CoreUseRequestDto& value) {
  json = Json{{"profile_id", value.profile_id}, {"core_id", value.core_id}};
}
inline void from_json(const Json& json, CoreUseRequestDto& value) {
  value.profile_id = json.at("profile_id").get<Id>();
  value.core_id = json.at("core_id").get<Id>();
}

inline void to_json(Json& json, const CoreInstall& value) {
  json = Json{{"id", value.id},
              {"name", value.name},
              {"path", value.path.string()},
              {"reported_version", value.reported_version},
              {"managed", value.managed}};
}

inline void from_json(const Json& json, CoreInstall& value) {
  value.id = json.at("id").get<std::string>();
  value.name = json.at("name").get<std::string>();
  value.path = json.at("path").get<std::string>();
  value.reported_version = json.at("reported_version").get<std::string>();
  value.managed = json.at("managed").get<bool>();
}

}  // namespace xrm
