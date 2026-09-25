#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xrm {
namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

bool is_control(unsigned char value) { return value < 0x20 || value == 0x7f; }

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

Result<std::string> percent_decode(std::string_view value, std::string subject) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(value[i]);
    if (ch == '%') {
      if (i + 2 >= value.size() || hex_value(value[i + 1]) < 0 || hex_value(value[i + 2]) < 0)
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "invalid percent escape", std::move(subject)));
      ch = static_cast<unsigned char>((hex_value(value[i + 1]) << 4) | hex_value(value[i + 2]));
      i += 2;
    }
    if (is_control(ch))
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "control character in URI component", std::move(subject)));
    decoded.push_back(static_cast<char>(ch));
  }
  return decoded;
}

struct UriParts {
  std::string user;
  std::string address;
  std::uint16_t port = 0;
  std::vector<std::pair<std::string, std::string>> parameters;
  std::optional<std::string> fragment;
};

Result<UriParts> parse_uri(std::string_view uri) {
  for (unsigned char ch : uri) {
    if (is_control(ch))
      return std::unexpected(error(ErrorCode::invalid_argument, "control character in URI"));
  }
  constexpr std::string_view scheme = "vless://";
  if (!uri.starts_with(scheme))
    return std::unexpected(error(ErrorCode::invalid_argument, "expected vless URI scheme", "scheme"));
  uri.remove_prefix(scheme.size());

  UriParts parts;
  if (const auto hash = uri.find('#'); hash != std::string_view::npos) {
    auto decoded = percent_decode(uri.substr(hash + 1), "fragment");
    if (!decoded) return std::unexpected(decoded.error());
    parts.fragment = std::move(*decoded);
    uri = uri.substr(0, hash);
  }
  std::string_view query;
  if (const auto question = uri.find('?'); question != std::string_view::npos) {
    query = uri.substr(question + 1);
    uri = uri.substr(0, question);
  }

  const auto at = uri.find('@');
  if (at == std::string_view::npos || uri.find('@', at + 1) != std::string_view::npos)
    return std::unexpected(error(ErrorCode::invalid_argument, "URI must contain one user and authority", "authority"));
  auto user = percent_decode(uri.substr(0, at), "user");
  if (!user) return std::unexpected(user.error());
  if (user->empty() || user->find(':') != std::string::npos)
    return std::unexpected(error(ErrorCode::invalid_argument, "VLESS user id is invalid", "user"));
  parts.user = std::move(*user);

  auto authority = uri.substr(at + 1);
  std::string_view host;
  std::string_view port_text;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close + 1 >= authority.size() || authority[close + 1] != ':')
      return std::unexpected(error(ErrorCode::invalid_argument, "invalid bracketed IPv6 authority", "authority"));
    host = authority.substr(1, close - 1);
    port_text = authority.substr(close + 2);
    if (host.empty() || host.find(':') == std::string_view::npos)
      return std::unexpected(error(ErrorCode::invalid_argument, "invalid IPv6 host", "host"));
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos || authority.find(':') != colon)
      return std::unexpected(error(ErrorCode::invalid_argument, "host and port are required", "authority"));
    host = authority.substr(0, colon);
    port_text = authority.substr(colon + 1);
  }
  if (host.empty()) return std::unexpected(error(ErrorCode::invalid_argument, "host is empty", "host"));
  parts.address = std::string(host);
  unsigned int port = 0;
  const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (ec != std::errc{} || end != port_text.data() + port_text.size() || port == 0 || port > 65535)
    return std::unexpected(error(ErrorCode::invalid_argument, "port must be between 1 and 65535", "port"));
  parts.port = static_cast<std::uint16_t>(port);

  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto component = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
    if (component.empty())
      return std::unexpected(error(ErrorCode::invalid_argument, "empty query parameter", "query"));
    const auto equals = component.find('=');
    auto key = percent_decode(component.substr(0, equals), "query");
    auto value = percent_decode(equals == std::string_view::npos ? std::string_view{} : component.substr(equals + 1),
                                "query");
    if (!key) return std::unexpected(key.error());
    if (!value) return std::unexpected(value.error());
    if (key->empty()) return std::unexpected(error(ErrorCode::invalid_argument, "query key is empty", "query"));
    parts.parameters.emplace_back(std::move(*key), std::move(*value));
  }
  return parts;
}

struct Parameters {
  std::map<std::string, std::string, std::less<>> values;
  std::vector<Notice> notices;
};

Result<Parameters> collect_parameters(const UriParts& uri) {
  Parameters result;
  for (const auto& [key, value] : uri.parameters) {
    const auto [it, inserted] = result.values.emplace(key, value);
    if (!inserted) {
      if (it->second != value)
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "conflicting duplicate URI parameter", key));
      if (std::none_of(result.notices.begin(), result.notices.end(), [&](const Notice& notice) {
            return notice.code == "duplicate_parameter" && notice.message == key;
          }))
        result.notices.push_back(Notice{"duplicate_parameter", key});
    }
  }
  return result;
}

Result<StreamNode> make_stream(const UriParts& uri, const Parameters& params) {
  static const std::set<std::string, std::less<>> supported{
      "encryption", "flow", "type", "security", "sni", "fp", "alpn", "pbk", "sid", "spx",
      "host", "path", "mode", "extra", "serviceName", "authority", "allowInsecure",
      "insecure", "headerType", "quicSecurity", "ech", "servername"};
  for (const auto& [key, _] : params.values) {
    if (!supported.contains(key))
      return std::unexpected(error(ErrorCode::unsupported, "unsupported VLESS URI parameter", key));
  }
  auto get = [&](std::string_view key) -> std::optional<std::string> {
    const auto it = params.values.find(key);
    return it == params.values.end() ? std::nullopt : std::optional<std::string>(it->second);
  };

  if (const auto sni = get("sni"), alias = get("servername"); sni && alias && *sni != *alias)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "sni and servername contain conflicting values", "servername"));

  StreamNode stream;
  stream.endpoint = Endpoint{uri.address, uri.port};
  stream.protocol = VlessSettings{uri.user, get("encryption").value_or("none"),
                                 get("flow"), std::nullopt};

  const auto type = get("type").value_or("tcp");
  if (type == "tcp" || type == "raw") stream.transport = RawTransport{};
  else if (type == "xhttp") {
    XHttpTransport transport;
    transport.host = get("host");
    transport.path = get("path");
    transport.mode = get("mode");
    if (const auto extra = get("extra")) {
      try {
        transport.extra = Json::parse(*extra);
      } catch (const Json::exception&) {
        return std::unexpected(error(ErrorCode::invalid_argument, "XHTTP extra must be valid JSON", "extra"));
      }
    }
    stream.transport = std::move(transport);
  } else if (type == "ws" || type == "websocket") {
    WebSocketTransport transport;
    transport.path = get("path");
    if (const auto host = get("host")) transport.headers.emplace("Host", *host);
    stream.transport = std::move(transport);
  } else if (type == "grpc") {
    const auto mode = get("mode");
    if (mode && *mode != "gun" && *mode != "multi")
      return std::unexpected(error(ErrorCode::unsupported,
                                   "unsupported VLESS gRPC mode", "mode"));
    stream.transport = GrpcTransport{get("serviceName"), get("authority"), mode == "multi"};
  } else if (type == "httpupgrade") {
    stream.transport = HttpUpgradeTransport{get("host"), get("path")};
  } else {
    return std::unexpected(error(ErrorCode::unsupported, "unsupported VLESS transport", "type"));
  }

  const auto network = std::holds_alternative<XHttpTransport>(stream.transport) ? std::string_view("xhttp")
      : std::holds_alternative<WebSocketTransport>(stream.transport) ? std::string_view("ws")
      : std::holds_alternative<GrpcTransport>(stream.transport) ? std::string_view("grpc")
      : std::holds_alternative<HttpUpgradeTransport>(stream.transport) ? std::string_view("httpupgrade")
      : std::string_view("raw");
  // Some subscription generators emit the gRPC-only mode field on TCP/RAW
  // links as well. It has no Xray TCP representation or effect there; accept
  // only the standardized gRPC values and keep the actual transport selected
  // by `type=tcp`. For actual gRPC links it is preserved in GrpcTransport.
  if (network == "raw") {
    if (const auto mode = get("mode"); mode && *mode != "gun" && *mode != "multi")
      return std::unexpected(error(ErrorCode::unsupported,
                                   "VLESS mode value is not supported for TCP/RAW", "mode"));
  }
  for (const auto& [key, _] : params.values) {
    const bool belongs =
        ((key == "host" || key == "path") &&
         (network == "xhttp" || network == "ws" || network == "httpupgrade")) ||
        ((key == "mode" || key == "extra") && network == "xhttp") ||
        (key == "mode" && network == "grpc") ||
        (key == "mode" && network == "raw" &&
         (params.values.at("mode") == "gun" || params.values.at("mode") == "multi")) ||
        ((key == "serviceName" || key == "authority") && network == "grpc");
    if ((key == "host" || key == "path" || key == "mode" || key == "extra" ||
         key == "serviceName" || key == "authority") && !belongs)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "transport parameter does not match selected transport", key));
  }
  const auto security = get("security").value_or("none");
  const auto insecure = get("insecure") ? get("insecure") : get("allowInsecure");
  const auto parse_bool = [](std::string_view value) -> std::optional<bool> {
    if (value == "1" || value == "true") return true;
    if (value == "0" || value == "false") return false;
    return std::nullopt;
  };
  if (insecure && !parse_bool(*insecure))
    return std::unexpected(error(ErrorCode::invalid_argument, "insecure must be true, false, 1 or 0", "insecure"));
  if (const auto header = get("headerType"); header && *header != "none")
    return std::unexpected(error(ErrorCode::unsupported,
                                 "VLESS headerType has connection semantics not supported by this transport", "headerType"));
  if (const auto quic = get("quicSecurity"); quic && *quic != "none")
    return std::unexpected(error(ErrorCode::unsupported,
                                 "VLESS quicSecurity is only accepted as the neutral none default", "quicSecurity"));
  auto alpn = get("alpn");
  std::vector<std::string> alpns;
  if (alpn) {
    std::size_t begin = 0;
    while (begin <= alpn->size()) {
      const auto comma = alpn->find(',', begin);
      const auto entry = alpn->substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
      if (entry.empty()) return std::unexpected(error(ErrorCode::invalid_argument, "ALPN item is empty", "alpn"));
      alpns.push_back(entry);
      if (comma == std::string::npos) break;
      begin = comma + 1;
    }
  }
  if (security == "none") {
    for (const auto key : {"sni", "servername", "fp", "alpn", "pbk", "sid", "spx", "ech"}) {
      if (get(key)) return std::unexpected(error(ErrorCode::invalid_argument,
                                                 "security parameter requires TLS or REALITY", key));
    }
    stream.security = NoSecurity{};
  } else if (security == "tls") {
    if (get("pbk") || get("sid") || get("spx"))
      return std::unexpected(error(ErrorCode::invalid_argument, "REALITY parameter cannot be used with TLS", "security"));
    TlsSecurity tls;
    tls.server_name = get("sni") ? get("sni") : get("servername");
    tls.fingerprint = get("fp");
    tls.alpn = std::move(alpns);
    if (insecure && *parse_bool(*insecure))
      return std::unexpected(error(ErrorCode::unsupported,
          "TLS insecure=true cannot be represented safely by the selected Xray version; configure a trusted CA or explicit certificate pin",
          "insecure"));
    if (const auto ech = get("ech")) tls.options["echConfigList"] = *ech;
    stream.security = std::move(tls);
  } else if (security == "reality") {
    RealitySecurity reality;
    if (get("ech")) return std::unexpected(error(ErrorCode::invalid_argument, "ech requires TLS security", "ech"));
    reality.server_name = get("sni") ? get("sni") : get("servername");
    reality.fingerprint = get("fp");
    reality.password = get("pbk").value_or("");
    reality.short_id = get("sid");
    reality.spider_x = get("spx");
    if (reality.password.empty())
      return std::unexpected(error(ErrorCode::invalid_argument, "REALITY public key is required", "pbk"));
    if (!alpns.empty()) reality.options["alpn"] = std::move(alpns);
    stream.security = std::move(reality);
  } else {
    return std::unexpected(error(ErrorCode::unsupported, "unsupported VLESS security", "security"));
  }
  return stream;
}

}  // namespace

Result<DecodedNode> parse_vless_uri(std::string_view uri) {
  auto parsed = parse_uri(uri);
  if (!parsed) return std::unexpected(parsed.error());
  auto params = collect_parameters(*parsed);
  if (!params) return std::unexpected(params.error());
  auto stream = make_stream(*parsed, *params);
  if (!stream) return std::unexpected(stream.error());

  Node node;
  node.original_name = parsed->fragment.value_or(parsed->address);
  node.settings = std::move(*stream);
  return DecodedNode{std::move(node), std::move(params->notices)};
}

}  // namespace xrm
