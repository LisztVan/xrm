#include "model/node.hpp"

#include <type_traits>

namespace xrm {
namespace {

Json transport_settings(const Transport& transport) {
  return std::visit([](const auto& value) -> Json {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, RawTransport>) return Json::object();
    if constexpr (std::is_same_v<T, XHttpTransport>) {
      Json result = Json::object();
      if (value.host) result["host"] = *value.host;
      if (value.path) result["path"] = *value.path;
      if (value.mode) result["mode"] = *value.mode;
      if (value.extra) result["extra"] = *value.extra;
      return result;
    }
    if constexpr (std::is_same_v<T, WebSocketTransport>) {
      Json result = Json::object();
      if (value.path) result["path"] = *value.path;
      if (!value.headers.empty()) result["headers"] = value.headers;
      return result;
    }
    if constexpr (std::is_same_v<T, GrpcTransport>) {
      Json result = Json::object();
      if (value.service_name) result["serviceName"] = *value.service_name;
      if (value.authority) result["authority"] = *value.authority;
      return result;
    }
    if constexpr (std::is_same_v<T, HttpUpgradeTransport>) {
      Json result = Json::object();
      if (value.host) result["host"] = *value.host;
      if (value.path) result["path"] = *value.path;
      return result;
    }
  }, transport);
}

Json security_settings(const Security& security) {
  return std::visit([](const auto& value) -> Json {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, NoSecurity>) return Json::object();
    if constexpr (std::is_same_v<T, TlsSecurity>) {
      Json result = value.options.is_object() ? value.options : Json::object();
      if (value.server_name) result["serverName"] = *value.server_name;
      if (value.fingerprint) result["fingerprint"] = *value.fingerprint;
      if (!value.alpn.empty()) result["alpn"] = value.alpn;
      return result;
    }
    if constexpr (std::is_same_v<T, RealitySecurity>) {
      Json result = value.options.is_object() ? value.options : Json::object();
      if (value.server_name) result["serverName"] = *value.server_name;
      if (value.fingerprint) result["fingerprint"] = *value.fingerprint;
      if (!value.password.empty()) result["publicKey"] = value.password;
      if (value.short_id) result["shortId"] = *value.short_id;
      if (value.spider_x) result["spiderX"] = *value.spider_x;
      return result;
    }
  }, security);
}

std::string_view network_name(const Transport& transport) {
  return std::visit([](const auto& value) -> std::string_view {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, RawTransport>) return "raw";
    if constexpr (std::is_same_v<T, XHttpTransport>) return "xhttp";
    if constexpr (std::is_same_v<T, WebSocketTransport>) return "websocket";
    if constexpr (std::is_same_v<T, GrpcTransport>) return "grpc";
    if constexpr (std::is_same_v<T, HttpUpgradeTransport>) return "httpupgrade";
  }, transport);
}

}  // namespace

Json write_stream_settings(const StreamNode& stream) {
  Json result{{"network", network_name(stream.transport)}};
  if (std::holds_alternative<NoSecurity>(stream.security)) {
    result["security"] = "none";
  } else if (std::holds_alternative<TlsSecurity>(stream.security)) {
    result["security"] = "tls";
    result["tlsSettings"] = security_settings(stream.security);
  } else {
    result["security"] = "reality";
    result["realitySettings"] = security_settings(stream.security);
  }

  const auto settings = transport_settings(stream.transport);
  if (!settings.empty()) {
    const auto network = network_name(stream.transport);
    if (network == "xhttp") result["xhttpSettings"] = settings;
    else if (network == "websocket") result["wsSettings"] = settings;
    else if (network == "grpc") result["grpcSettings"] = settings;
    else if (network == "httpupgrade") result["httpupgradeSettings"] = settings;
  }
  if (stream.sockopt) result["sockopt"] = *stream.sockopt;
  if (stream.finalmask) result["finalmask"] = *stream.finalmask;
  return result;
}

}  // namespace xrm
