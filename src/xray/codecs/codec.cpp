#include "xray/codecs/codec.hpp"

namespace xrm {
Json write_stream_settings(const StreamNode& stream);

namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

}  // namespace

Result<Node> parse_native_outbound(const Json& outbound, Id id, std::string display_name) {
  if (id.empty())
    return std::unexpected(error(ErrorCode::invalid_argument, "node id must not be empty", "id"));
  if (!outbound.is_object())
    return std::unexpected(error(ErrorCode::invalid_argument, "native outbound must be one object", "outbound"));
  if (!outbound.contains("protocol") || !outbound["protocol"].is_string() ||
      !outbound.contains("settings") || !outbound["settings"].is_object())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "expected one complete Xray outbound, not a config collection", "outbound"));
  if (outbound.contains("proxySettings")) {
    const auto& proxy = outbound["proxySettings"];
    if (proxy.is_object() && proxy.contains("tag"))
      return std::unexpected(error(ErrorCode::unsupported,
                                   "standalone import cannot resolve cross-node proxy references",
                                   "proxySettings.tag"));
  }

  Node node;
  node.id = std::move(id);
  if (display_name.empty()) {
    if (outbound.contains("tag") && outbound["tag"].is_string())
      display_name = outbound["tag"].get<std::string>();
    if (display_name.empty()) display_name = outbound["protocol"].get<std::string>();
  }
  node.original_name = std::move(display_name);
  node.settings = RawOutbound{outbound};
  return node;
}

Result<Json> write_outbound(const Node& node, std::string_view tag) {
  if (tag.empty())
    return std::unexpected(error(ErrorCode::invalid_argument, "runtime tag must not be empty", "tag"));
  if (const auto* raw = std::get_if<RawOutbound>(&node.settings)) {
    if (!raw->value.is_object() || !raw->value.contains("protocol") ||
        !raw->value.contains("settings") || !raw->value["settings"].is_object())
      return std::unexpected(error(ErrorCode::invalid_argument, "raw node is not a complete native outbound", "outbound"));
    Json result = raw->value;
    result["tag"] = tag;
    return result;
  }

  const auto& stream = std::get<StreamNode>(node.settings);
  if (stream.endpoint.address.empty() || stream.endpoint.port == 0 || stream.protocol.id.empty())
    return std::unexpected(error(ErrorCode::invalid_argument, "VLESS endpoint and user id are required", "node"));
  if (const auto* tls = std::get_if<TlsSecurity>(&stream.security)) {
    if (!tls->options.is_object())
      return std::unexpected(error(ErrorCode::invalid_argument, "TLS options must be an object", "security.options"));
    for (const auto key : {"serverName", "fingerprint", "alpn"}) {
      if (tls->options.contains(key))
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "TLS option duplicates a typed field", std::string("security.options.") + key));
    }
  }
  if (const auto* reality = std::get_if<RealitySecurity>(&stream.security)) {
    if (!reality->options.is_object())
      return std::unexpected(error(ErrorCode::invalid_argument, "REALITY options must be an object", "security.options"));
    for (const auto key : {"serverName", "fingerprint", "publicKey", "shortId", "spiderX"}) {
      if (reality->options.contains(key))
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "REALITY option duplicates a typed field", std::string("security.options.") + key));
    }
  }

  Json user{{"id", stream.protocol.id}};
  user["encryption"] = stream.protocol.encryption.value_or("none");
  if (stream.protocol.flow) user["flow"] = *stream.protocol.flow;
  if (stream.protocol.level) user["level"] = *stream.protocol.level;

  Json result{{"tag", tag}, {"protocol", "vless"},
              {"settings", {{"vnext", Json::array({Json{
                  {"address", stream.endpoint.address}, {"port", stream.endpoint.port},
                  {"users", Json::array({std::move(user)})}}})}}},
              {"streamSettings", write_stream_settings(stream)}};
  if (stream.mux) result["mux"] = *stream.mux;
  return result;
}

}  // namespace xrm
