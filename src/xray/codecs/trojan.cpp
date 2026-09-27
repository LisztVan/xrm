// XRM: Decodes supported Trojan share URIs into typed node settings.
// URI parsing is separate from native outbound JSON import/generation; do not
// infer support for every Xray field from this link parser.

#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace xrm {
namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

bool is_control(unsigned char value) { return value < 0x20 || value == 0x7f; }

int hex_value(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

Result<std::string> percent_decode(std::string_view value,
                                   std::string subject) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(value[i]);
    if (ch == '%') {
      if (i + 2 >= value.size() || hex_value(value[i + 1]) < 0 ||
          hex_value(value[i + 2]) < 0)
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "invalid percent escape", subject));
      ch = static_cast<unsigned char>((hex_value(value[i + 1]) << 4) |
                                      hex_value(value[i + 2]));
      i += 2;
    }
    if (is_control(ch))
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "control character in URI component",
                                   subject));
    decoded.push_back(static_cast<char>(ch));
  }
  return decoded;
}

struct Authority {
  std::string address;
  std::uint16_t port = 0;
};

Result<Authority> parse_authority(std::string_view authority) {
  std::string_view host;
  std::string_view port_text;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close + 1 >= authority.size() ||
        authority[close + 1] != ':')
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "invalid bracketed IPv6 authority",
                                   "authority"));
    host = authority.substr(1, close - 1);
    port_text = authority.substr(close + 2);
    if (host.empty() || host.find(':') == std::string_view::npos)
      return std::unexpected(
          error(ErrorCode::invalid_argument, "invalid IPv6 host", "host"));
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos || authority.find(':') != colon)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "host and port are required", "authority"));
    host = authority.substr(0, colon);
    port_text = authority.substr(colon + 1);
  }
  if (host.empty())
    return std::unexpected(
        error(ErrorCode::invalid_argument, "host is empty", "host"));
  unsigned int port = 0;
  const auto [end, ec] = std::from_chars(
      port_text.data(), port_text.data() + port_text.size(), port);
  if (ec != std::errc{} || end != port_text.data() + port_text.size() ||
      port == 0 || port > 65535)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "port must be between 1 and 65535", "port"));
  return Authority{std::string(host), static_cast<std::uint16_t>(port)};
}

struct ParsedUri {
  std::string password;
  Authority authority;
  std::optional<std::string> fragment;
  std::map<std::string, std::string, std::less<>> parameters;
  std::vector<Notice> notices;
};

Result<ParsedUri> parse_uri(std::string_view uri) {
  for (unsigned char ch : uri) {
    if (is_control(ch))
      return std::unexpected(
          error(ErrorCode::invalid_argument, "control character in URI"));
  }
  constexpr std::string_view scheme = "trojan://";
  if (!uri.starts_with(scheme))
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "expected trojan URI scheme", "scheme"));
  uri.remove_prefix(scheme.size());

  ParsedUri result;
  if (const auto hash = uri.find('#'); hash != std::string_view::npos) {
    auto name = percent_decode(uri.substr(hash + 1), "fragment");
    if (!name)
      return std::unexpected(name.error());
    result.fragment = std::move(*name);
    uri = uri.substr(0, hash);
  }
  std::string_view query;
  if (const auto question = uri.find('?'); question != std::string_view::npos) {
    query = uri.substr(question + 1);
    uri = uri.substr(0, question);
  }

  const auto at = uri.find('@');
  if (at == std::string_view::npos ||
      uri.find('@', at + 1) != std::string_view::npos)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "URI must contain one password and authority",
                                 "authority"));
  auto password = percent_decode(uri.substr(0, at), "password");
  if (!password)
    return std::unexpected(password.error());
  if (password->empty())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "Trojan password is empty", "password"));
  result.password = std::move(*password);
  auto authority = parse_authority(uri.substr(at + 1));
  if (!authority)
    return std::unexpected(authority.error());
  result.authority = std::move(*authority);

  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto component = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{}
                                          : query.substr(amp + 1);
    if (component.empty())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "empty query parameter", "query"));
    const auto equals = component.find('=');
    auto key = percent_decode(component.substr(0, equals), "query");
    auto value = percent_decode(equals == std::string_view::npos
                                    ? std::string_view{}
                                    : component.substr(equals + 1),
                                "query");
    if (!key)
      return std::unexpected(key.error());
    if (!value)
      return std::unexpected(value.error());
    if (key->empty())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "query key is empty", "query"));
    const auto [it, inserted] = result.parameters.emplace(*key, *value);
    if (!inserted) {
      if (it->second != *value)
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "conflicting duplicate URI parameter",
                                     *key));
      if (std::none_of(result.notices.begin(), result.notices.end(),
                       [&](const Notice &notice) {
                         return notice.code == "duplicate_parameter" &&
                                notice.message == *key;
                       }))
        result.notices.push_back(Notice{"duplicate_parameter", *key});
    }
  }
  return result;
}

Result<std::vector<std::string>>
parse_alpn(const std::optional<std::string> &value) {
  std::vector<std::string> result;
  if (!value)
    return result;
  std::size_t begin = 0;
  while (begin <= value->size()) {
    const auto comma = value->find(',', begin);
    auto entry = value->substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (entry.empty())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "ALPN item is empty", "alpn"));
    result.push_back(std::move(entry));
    if (comma == std::string::npos)
      break;
    begin = comma + 1;
  }
  return result;
}

} // namespace

Result<DecodedNode> parse_trojan_uri(std::string_view uri) {
  auto parsed = parse_uri(uri);
  if (!parsed)
    return std::unexpected(parsed.error());

  const auto get = [&](std::string_view key) -> std::optional<std::string> {
    const auto it = parsed->parameters.find(key);
    return it == parsed->parameters.end()
               ? std::nullopt
               : std::optional<std::string>(it->second);
  };
  if (const auto flow = get("flow"); flow && !flow->empty())
    return std::unexpected(error(ErrorCode::unsupported,
                                 "Trojan flow is not supported by Xray v26.9.9",
                                 "flow"));

  static const std::set<std::string, std::less<>> supported{
      "type",     "host", "path", "serviceName", "authority", "mode",
      "security", "sni",  "alpn", "fp",          "flow"};
  for (const auto &[key, _] : parsed->parameters) {
    if (!supported.contains(key))
      return std::unexpected(error(ErrorCode::unsupported,
                                   "unsupported Trojan URI parameter", key));
  }

  StreamNode stream;
  stream.endpoint = Endpoint{parsed->authority.address, parsed->authority.port};
  stream.protocol = TrojanSettings{parsed->password};

  const auto type = get("type").value_or("raw");
  if (type == "raw")
    stream.transport = RawTransport{};
  else if (type == "ws") {
    WebSocketTransport ws;
    ws.path = get("path");
    if (const auto host = get("host"))
      ws.headers.emplace("Host", *host);
    stream.transport = std::move(ws);
  } else if (type == "grpc") {
    stream.transport = GrpcTransport{get("serviceName"), get("authority")};
  } else if (type == "httpupgrade") {
    stream.transport = HttpUpgradeTransport{get("host"), get("path")};
  } else if (type == "xhttp") {
    stream.transport =
        XHttpTransport{get("host"), get("path"), get("mode"), std::nullopt};
  } else {
    return std::unexpected(
        error(ErrorCode::unsupported, "unsupported Trojan transport", "type"));
  }

  const auto network = type;
  for (const auto key : {"host", "path", "serviceName", "authority", "mode"}) {
    if (!get(key))
      continue;
    const bool belongs =
        ((std::string_view(key) == "host" || std::string_view(key) == "path") &&
         (network == "ws" || network == "httpupgrade" || network == "xhttp")) ||
        ((std::string_view(key) == "serviceName" ||
          std::string_view(key) == "authority") &&
         network == "grpc") ||
        (std::string_view(key) == "mode" && network == "xhttp");
    if (!belongs)
      return std::unexpected(
          error(ErrorCode::invalid_argument,
                "transport parameter does not match selected transport", key));
  }

  const auto security = get("security").value_or("none");
  if (security == "none") {
    if (get("sni") || get("alpn") || get("fp"))
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "TLS parameter requires security=tls",
                                   "security"));
    stream.security = NoSecurity{};
  } else if (security == "tls") {
    auto alpn = parse_alpn(get("alpn"));
    if (!alpn)
      return std::unexpected(alpn.error());
    TlsSecurity tls;
    tls.server_name = get("sni");
    tls.fingerprint = get("fp");
    tls.alpn = std::move(*alpn);
    stream.security = std::move(tls);
  } else {
    return std::unexpected(error(ErrorCode::unsupported,
                                 "unsupported Trojan security", "security"));
  }

  Node node;
  node.original_name = parsed->fragment.value_or(parsed->authority.address);
  node.settings = std::move(stream);
  return DecodedNode{std::move(node), std::move(parsed->notices)};
}

} // namespace xrm
