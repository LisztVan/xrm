#include "model/node.hpp"
#include "model/runtime.hpp"

#include <stdexcept>

namespace xrm {
namespace {

Json optional_string(const std::optional<std::string>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json settings_json(const NodeSettings& settings) {
  if (const auto* raw = std::get_if<RawOutbound>(&settings))
    return Json{{"kind", "raw"}, {"value", raw->value}};
  const auto& stream = std::get<StreamNode>(settings);
  Json protocol{{"kind", "vless"}, {"id", stream.protocol.id},
                {"encryption", optional_string(stream.protocol.encryption)},
                {"flow", optional_string(stream.protocol.flow)},
                {"level", stream.protocol.level ? Json(*stream.protocol.level) : Json(nullptr)}};
  Json transport;
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, RawTransport>) transport = Json{{"kind", "raw"}};
    else if constexpr (std::is_same_v<T, XHttpTransport>)
      transport = Json{{"kind", "xhttp"}, {"host", optional_string(value.host)},
                       {"path", optional_string(value.path)}, {"mode", optional_string(value.mode)},
                       {"extra", value.extra ? *value.extra : Json(nullptr)}};
    else if constexpr (std::is_same_v<T, WebSocketTransport>)
      transport = Json{{"kind", "websocket"}, {"path", optional_string(value.path)}, {"headers", value.headers}};
    else if constexpr (std::is_same_v<T, GrpcTransport>)
      transport = Json{{"kind", "grpc"}, {"service_name", optional_string(value.service_name)},
                       {"authority", optional_string(value.authority)}};
    else transport = Json{{"kind", "httpupgrade"}, {"host", optional_string(value.host)},
                          {"path", optional_string(value.path)}};
  }, stream.transport);
  Json security;
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, NoSecurity>) security = Json{{"kind", "none"}};
    else if constexpr (std::is_same_v<T, TlsSecurity>)
      security = Json{{"kind", "tls"}, {"server_name", optional_string(value.server_name)},
                      {"fingerprint", optional_string(value.fingerprint)}, {"alpn", value.alpn}, {"options", value.options}};
    else security = Json{{"kind", "reality"}, {"server_name", optional_string(value.server_name)},
                         {"fingerprint", optional_string(value.fingerprint)}, {"password", value.password},
                         {"short_id", optional_string(value.short_id)}, {"spider_x", optional_string(value.spider_x)},
                         {"options", value.options}};
  }, stream.security);
  return Json{{"kind", "stream"},
              {"endpoint", Json{{"address", stream.endpoint.address}, {"port", stream.endpoint.port}}},
              {"protocol", std::move(protocol)}, {"transport", std::move(transport)},
              {"security", std::move(security)},
              {"sockopt", stream.sockopt ? *stream.sockopt : Json(nullptr)},
              {"finalmask", stream.finalmask ? *stream.finalmask : Json(nullptr)},
              {"mux", stream.mux ? *stream.mux : Json(nullptr)}};
}

std::optional<std::string> read_optional_string(const Json& value, const char* key) {
  const auto& field = value.at(key);
  if (field.is_null()) return std::nullopt;
  return field.get<std::string>();
}

NodeSettings settings_from_json(const Json& json) {
  const auto kind = json.at("kind").get<std::string>();
  if (kind == "raw") return RawOutbound{json.at("value")};
  if (kind != "stream") throw std::invalid_argument("unknown node settings kind");
  const auto& p = json.at("protocol");
  if (p.at("kind") != "vless") throw std::invalid_argument("unsupported node protocol kind");
  const auto& e = json.at("endpoint");
  StreamNode stream;
  stream.endpoint = {e.at("address").get<std::string>(), e.at("port").get<std::uint16_t>()};
  stream.protocol.id = p.at("id").get<std::string>();
  stream.protocol.encryption = read_optional_string(p, "encryption");
  stream.protocol.flow = read_optional_string(p, "flow");
  if (!p.at("level").is_null()) stream.protocol.level = p.at("level").get<std::uint32_t>();
  const auto& t = json.at("transport");
  const auto tk = t.at("kind").get<std::string>();
  if (tk == "raw") stream.transport = RawTransport{};
  else if (tk == "xhttp") stream.transport = XHttpTransport{read_optional_string(t, "host"), read_optional_string(t, "path"), read_optional_string(t, "mode"), t.at("extra").is_null() ? std::nullopt : std::optional<Json>(t.at("extra"))};
  else if (tk == "websocket") stream.transport = WebSocketTransport{read_optional_string(t, "path"), t.at("headers").get<std::map<std::string, std::string>>()};
  else if (tk == "grpc") stream.transport = GrpcTransport{read_optional_string(t, "service_name"), read_optional_string(t, "authority")};
  else if (tk == "httpupgrade") stream.transport = HttpUpgradeTransport{read_optional_string(t, "host"), read_optional_string(t, "path")};
  else throw std::invalid_argument("unknown transport kind");
  const auto& s = json.at("security");
  const auto sk = s.at("kind").get<std::string>();
  if (sk == "none") stream.security = NoSecurity{};
  else if (sk == "tls") stream.security = TlsSecurity{read_optional_string(s, "server_name"), read_optional_string(s, "fingerprint"), s.at("alpn").get<std::vector<std::string>>(), s.at("options")};
  else if (sk == "reality") stream.security = RealitySecurity{read_optional_string(s, "server_name"), read_optional_string(s, "fingerprint"), s.at("password").get<std::string>(), read_optional_string(s, "short_id"), read_optional_string(s, "spider_x"), s.at("options")};
  else throw std::invalid_argument("unknown security kind");
  for (const auto key : {"sockopt", "finalmask", "mux"}) {
    const auto& field = json.at(key);
    if (field.is_null()) continue;
    if (key == std::string_view("sockopt")) stream.sockopt = field;
    else if (key == std::string_view("finalmask")) stream.finalmask = field;
    else stream.mux = field;
  }
  return stream;
}

}  // namespace

void to_json(Json& json, const Node& value) {
  json = Json{{"id", value.id}, {"source_id", value.source_id ? Json(*value.source_id) : Json(nullptr)},
              {"original_name", value.original_name}, {"alias", optional_string(value.alias)},
              {"labels", value.labels}, {"enabled", value.enabled},
              {"source_missing", value.source_missing}, {"settings", settings_json(value.settings)}};
}

void from_json(const Json& json, Node& value) {
  if (!json.is_object()) throw Json::type_error::create(302, "node must be an object", &json);
  value.id = json.at("id").get<std::string>();
  value.source_id = json.at("source_id").is_null() ? std::nullopt : std::optional<Id>(json.at("source_id").get<std::string>());
  value.original_name = json.at("original_name").get<std::string>();
  value.alias = read_optional_string(json, "alias");
  value.labels = json.at("labels").get<std::vector<std::string>>();
  value.enabled = json.at("enabled").get<bool>();
  value.source_missing = json.at("source_missing").get<bool>();
  value.settings = settings_from_json(json.at("settings"));
}

}  // namespace xrm
