#include "model/node.hpp"

#include <stdexcept>

namespace xrm {

void to_json(Json& json, const Node& value) {
  json = Json{{"id", value.id},
              {"source_id", value.source_id ? Json(*value.source_id) : Json(nullptr)},
              {"original_name", value.original_name},
              {"alias", value.alias ? Json(*value.alias) : Json(nullptr)},
              {"labels", value.labels},
              {"enabled", value.enabled},
              {"source_missing", value.source_missing},
              {"outbound", value.outbound}};
}

void from_json(const Json& json, Node& value) {
  if (!json.is_object()) throw Json::type_error::create(302, "node must be an object", &json);
  value.id = json.at("id").get<std::string>();
  value.source_id = json.at("source_id").is_null()
                        ? std::nullopt
                        : std::optional<Id>(json.at("source_id").get<std::string>());
  value.original_name = json.at("original_name").get<std::string>();
  value.alias = json.at("alias").is_null()
                    ? std::nullopt
                    : std::optional<std::string>(json.at("alias").get<std::string>());
  value.labels = json.at("labels").get<std::vector<std::string>>();
  value.enabled = json.at("enabled").get<bool>();
  value.source_missing = json.at("source_missing").get<bool>();
  value.outbound = json.at("outbound");
  if (!value.outbound.is_object()) {
    throw Json::type_error::create(302, "node outbound must be an object", &json);
  }
}

}  // namespace xrm
