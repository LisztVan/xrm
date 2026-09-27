// XRM: Dispatches node protocols to native Xray outbound encoders.

#include "xray/codecs/codec.hpp"

namespace xrm {
Json write_stream_settings(const StreamNode &stream);

namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

} // namespace

Result<Node> parse_native_outbound(const Json &outbound, Id id,
                                   std::string display_name) {
  if (id.empty())
    return std::unexpected(
        error(ErrorCode::invalid_argument, "node id must not be empty", "id"));
  if (!outbound.is_object())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "native outbound must be one object",
                                 "outbound"));
  if (!outbound.contains("protocol") || !outbound["protocol"].is_string() ||
      !outbound.contains("settings") || !outbound["settings"].is_object())
    return std::unexpected(
        error(ErrorCode::invalid_argument,
              "expected one complete Xray outbound, not a config collection",
              "outbound"));
  if (outbound.contains("proxySettings")) {
    const auto &proxy = outbound["proxySettings"];
    if (proxy.is_object() && proxy.contains("tag"))
      return std::unexpected(
          error(ErrorCode::unsupported,
                "standalone import cannot resolve cross-node proxy references",
                "proxySettings.tag"));
  }

  Node node;
  node.id = std::move(id);
  if (display_name.empty()) {
    if (outbound.contains("tag") && outbound["tag"].is_string())
      display_name = outbound["tag"].get<std::string>();
    if (display_name.empty())
      display_name = outbound["protocol"].get<std::string>();
  }
  node.original_name = std::move(display_name);
  node.settings = RawOutbound{outbound};
  return node;
}

Result<Json> write_outbound(const Node &node, std::string_view tag) {
  if (tag.empty())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "runtime tag must not be empty", "tag"));
  if (const auto *raw = std::get_if<RawOutbound>(&node.settings)) {
    if (!raw->value.is_object() || !raw->value.contains("protocol") ||
        !raw->value.contains("settings") || !raw->value["settings"].is_object())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "raw node is not a complete native outbound",
                                   "outbound"));
    Json result = raw->value;
    result["tag"] = tag;
    return result;
  }

  const auto &stream = std::get<StreamNode>(node.settings);
  if (stream.endpoint.address.empty() || stream.endpoint.port == 0)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "typed outbound endpoint is required",
                                 "node.endpoint"));
  if (const auto *tls = std::get_if<TlsSecurity>(&stream.security)) {
    if (!tls->options.is_object())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "TLS options must be an object",
                                   "security.options"));
    for (const auto key : {"serverName", "fingerprint", "alpn"}) {
      if (tls->options.contains(key))
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "TLS option duplicates a typed field",
                                     std::string("security.options.") + key));
    }
  }
  if (const auto *reality = std::get_if<RealitySecurity>(&stream.security)) {
    if (!reality->options.is_object())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "REALITY options must be an object",
                                   "security.options"));
    for (const auto key :
         {"serverName", "fingerprint", "publicKey", "shortId", "spiderX"}) {
      if (reality->options.contains(key))
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "REALITY option duplicates a typed field",
                                     std::string("security.options.") + key));
    }
  }

  Json settings;
  std::string_view protocol;
  if (const auto *vless = std::get_if<VlessSettings>(&stream.protocol)) {
    if (vless->id.empty())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "VLESS user id is required",
                                   "node.protocol.id"));
    Json user{{"id", vless->id},
              {"encryption", vless->encryption.value_or("none")}};
    if (vless->flow)
      user["flow"] = *vless->flow;
    if (vless->level)
      user["level"] = *vless->level;
    settings = {
        {"vnext",
         Json::array({Json{{"address", stream.endpoint.address},
                           {"port", stream.endpoint.port},
                           {"users", Json::array({std::move(user)})}}})}};
    protocol = "vless";
  } else if (const auto *vmess = std::get_if<VmessSettings>(&stream.protocol)) {
    if (vmess->id.empty())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "VMess user id is required",
                                   "node.protocol.id"));
    Json user{{"id", vmess->id},
              {"security", vmess->security.value_or("auto")}};
    if (vmess->level)
      user["level"] = *vmess->level;
    settings = {
        {"vnext",
         Json::array({Json{{"address", stream.endpoint.address},
                           {"port", stream.endpoint.port},
                           {"users", Json::array({std::move(user)})}}})}};
    protocol = "vmess";
  } else if (const auto *trojan =
                 std::get_if<TrojanSettings>(&stream.protocol)) {
    if (trojan->password.empty())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "Trojan password is required",
                                   "node.protocol.password"));
    settings = {
        {"servers", Json::array({Json{{"address", stream.endpoint.address},
                                      {"port", stream.endpoint.port},
                                      {"password", trojan->password}}})}};
    protocol = "trojan";
  } else if (const auto *shadowsocks =
                 std::get_if<ShadowsocksSettings>(&stream.protocol)) {
    if (shadowsocks->method.empty() || shadowsocks->password.empty())
      return std::unexpected(error(
          ErrorCode::invalid_argument,
          "Shadowsocks method and password are required", "node.protocol"));
    settings = {
        {"servers", Json::array({Json{{"address", stream.endpoint.address},
                                      {"port", stream.endpoint.port},
                                      {"method", shadowsocks->method},
                                      {"password", shadowsocks->password}}})}};
    protocol = "shadowsocks";
  } else {
    const auto &hysteria = std::get<Hysteria2Settings>(stream.protocol);
    const auto *transport = std::get_if<HysteriaTransport>(&stream.transport);
    if (!transport || !transport->options.is_object() ||
        !std::holds_alternative<TlsSecurity>(stream.security))
      return std::unexpected(
          error(ErrorCode::invalid_argument,
                "Hysteria2 requires its native transport and TLS settings",
                "node.streamSettings"));
    if (transport->options.contains("auth"))
      return std::unexpected(
          error(ErrorCode::invalid_argument,
                "Hysteria auth must come from the typed protocol setting",
                "node.transport.auth"));
    settings = {{"version", 2},
                {"address", stream.endpoint.address},
                {"port", stream.endpoint.port}};
    protocol = "hysteria";
  }

  // Xray's native Shadowsocks envelope has no streamSettings for the default
  // raw/no-security case. Preserve an explicit stream object only when it has
  // meaningful settings (transport/security/socket/finalmask), otherwise use
  // the canonical outbound shape from the pinned v26.9.9 fixture.
  const bool default_shadowsocks_stream =
      std::holds_alternative<ShadowsocksSettings>(stream.protocol) &&
      std::holds_alternative<RawTransport>(stream.transport) &&
      std::holds_alternative<NoSecurity>(stream.security) && !stream.sockopt &&
      !stream.finalmask;
  auto stream_settings = write_stream_settings(stream);
  if (const auto *hysteria = std::get_if<Hysteria2Settings>(&stream.protocol)) {
    auto &transport = stream_settings["hysteriaSettings"];
    transport["auth"] = hysteria->auth;
  }
  Json result{
      {"tag", tag}, {"protocol", protocol}, {"settings", std::move(settings)}};
  if (!default_shadowsocks_stream)
    result["streamSettings"] = std::move(stream_settings);
  if (stream.mux)
    result["mux"] = *stream.mux;
  return result;
}

} // namespace xrm
