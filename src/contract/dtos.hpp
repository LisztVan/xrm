#pragma once

#include "model/node.hpp"
#include "model/runtime.hpp"

#include <nlohmann/json.hpp>

namespace xrm {

struct NodeSummary {
  Id id;
  std::string name;
  std::vector<std::string> labels;
  bool enabled = true;
  bool source_missing = false;
  std::string protocol;
};

inline void to_json(Json& json, const NodeSummary& value) {
  json = Json{{"id", value.id},
              {"name", value.name},
              {"labels", value.labels},
              {"enabled", value.enabled},
              {"source_missing", value.source_missing},
              {"protocol", value.protocol}};
}

inline void from_json(const Json& json, NodeSummary& value) {
  value.id = json.at("id").get<std::string>();
  value.name = json.at("name").get<std::string>();
  value.labels = json.at("labels").get<std::vector<std::string>>();
  value.enabled = json.at("enabled").get<bool>();
  value.source_missing = json.at("source_missing").get<bool>();
  value.protocol = json.at("protocol").get<std::string>();
}

inline NodeSummary summarize_node(const Node& node) {
  const auto name = node.alias.value_or(node.original_name);
  std::string protocol = "unknown";
  if (const auto* raw = std::get_if<RawOutbound>(&node.settings)) {
    if (raw->value.is_object() && raw->value.contains("protocol") &&
        raw->value.at("protocol").is_string()) {
      protocol = raw->value.at("protocol").get<std::string>();
    }
  } else {
    protocol = "vless";
  }
  return {node.id, name, node.labels, node.enabled, node.source_missing,
          std::move(protocol)};
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
