// XRM: Encodes Hysteria node settings as native Xray outbounds.

#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
                                     "invalid percent escape",
                                     std::move(subject)));
      ch = static_cast<unsigned char>((hex_value(value[i + 1]) << 4) |
                                      hex_value(value[i + 2]));
      i += 2;
    }
    if (is_control(ch))
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "control character in URI component",
                                   std::move(subject)));
    decoded.push_back(static_cast<char>(ch));
  }
  return decoded;
}

struct ParsedUri {
  std::string auth;
  std::string address;
  std::uint16_t port = 443;
  std::vector<std::pair<std::string, std::string>> parameters;
  std::optional<std::string> fragment;
};

Result<ParsedUri> parse_uri(std::string_view uri) {
  for (const unsigned char ch : uri) {
    if (is_control(ch))
      return std::unexpected(
          error(ErrorCode::invalid_argument, "control character in URI"));
  }

  if (uri.starts_with("hysteria2://"))
    uri.remove_prefix(std::string_view("hysteria2://").size());
  else if (uri.starts_with("hy2://"))
    uri.remove_prefix(std::string_view("hy2://").size());
  else
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "expected hysteria2 or hy2 URI scheme",
                                 "scheme"));

  ParsedUri result;
  if (const auto hash = uri.find('#'); hash != std::string_view::npos) {
    auto fragment = percent_decode(uri.substr(hash + 1), "fragment");
    if (!fragment)
      return std::unexpected(fragment.error());
    result.fragment = std::move(*fragment);
    uri = uri.substr(0, hash);
  }

  std::string_view query;
  if (const auto question = uri.find('?'); question != std::string_view::npos) {
    query = uri.substr(question + 1);
    uri = uri.substr(0, question);
  }

  if (const auto slash = uri.find('/'); slash != std::string_view::npos) {
    if (uri.substr(slash) != "/")
      return std::unexpected(error(ErrorCode::unsupported,
                                   "Hysteria2 URI paths are unsupported",
                                   "path"));
    uri = uri.substr(0, slash);
  }
  if (uri.empty())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "Hysteria2 authority is empty", "authority"));

  std::string_view authority = uri;
  if (const auto at = uri.find('@'); at != std::string_view::npos) {
    if (uri.find('@', at + 1) != std::string_view::npos)
      return std::unexpected(
          error(ErrorCode::invalid_argument,
                "URI must contain at most one user separator", "authority"));
    auto auth = percent_decode(uri.substr(0, at), "auth");
    if (!auth)
      return std::unexpected(auth.error());
    result.auth = std::move(*auth);
    authority = uri.substr(at + 1);
  }

  std::string_view host;
  std::string_view port_text;
  bool has_port = false;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "invalid bracketed IPv6 authority",
                                   "authority"));
    host = authority.substr(1, close - 1);
    if (host.empty() || host.find(':') == std::string_view::npos)
      return std::unexpected(
          error(ErrorCode::invalid_argument, "invalid IPv6 host", "host"));
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':')
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "invalid text after IPv6 host",
                                     "authority"));
      has_port = true;
      port_text = authority.substr(close + 2);
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      if (authority.find(':') != colon)
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "IPv6 hosts must be bracketed",
                                     "authority"));
      host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
      has_port = true;
    } else {
      host = authority;
    }
  }
  if (host.empty())
    return std::unexpected(
        error(ErrorCode::invalid_argument, "host is empty", "host"));
  result.address = std::string(host);
  if (has_port) {
    if (port_text.find_first_of(",-") != std::string_view::npos)
      return std::unexpected(error(ErrorCode::unsupported,
                                   "Hysteria2 port hopping is unsupported",
                                   "port"));
    unsigned int port = 0;
    const auto [end, ec] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port);
    if (ec != std::errc{} || end != port_text.data() + port_text.size() ||
        port == 0 || port > 65535)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "port must be between 1 and 65535", "port"));
    result.port = static_cast<std::uint16_t>(port);
  }

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
    result.parameters.emplace_back(std::move(*key), std::move(*value));
  }
  return result;
}

struct CollectedParameters {
  Json values = Json::object();
  std::vector<Notice> notices;
};

Result<CollectedParameters> collect_parameters(const ParsedUri &uri) {
  CollectedParameters result;
  for (const auto &[key, value] : uri.parameters) {
    if (!result.values.contains(key)) {
      result.values[key] = value;
      continue;
    }
    if (result.values[key] != value)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "conflicting duplicate URI parameter", key));
    if (std::none_of(result.notices.begin(), result.notices.end(),
                     [&](const Notice &notice) {
                       return notice.code == "duplicate_parameter" &&
                              notice.message == key;
                     }))
      result.notices.push_back(Notice{"duplicate_parameter", key});
  }
  return result;
}

} // namespace

Result<DecodedNode> parse_hysteria2_uri(std::string_view uri) {
  auto parsed = parse_uri(uri);
  if (!parsed)
    return std::unexpected(parsed.error());
  auto parameters = collect_parameters(*parsed);
  if (!parameters)
    return std::unexpected(parameters.error());

  static const std::set<std::string, std::less<>> supported{"sni", "insecure",
                                                            "udpIdleTimeout"};
  for (const auto &[key, _] : parameters->values.items()) {
    if (!supported.contains(key))
      return std::unexpected(error(ErrorCode::unsupported,
                                   "unsupported Hysteria2 URI parameter", key));
  }
  auto get = [&](std::string_view key) -> std::optional<std::string> {
    const auto it = parameters->values.find(key);
    return it == parameters->values.end()
               ? std::nullopt
               : std::optional<std::string>(it->get<std::string>());
  };

  TlsSecurity tls;
  tls.server_name = get("sni");
  if (const auto insecure = get("insecure")) {
    if (*insecure == "1" || *insecure == "true")
      tls.options["allowInsecure"] = true;
    else if (*insecure != "0" && *insecure != "false")
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "insecure must be boolean", "insecure"));
  }
  if (tls.server_name && tls.server_name->empty())
    return std::unexpected(
        error(ErrorCode::invalid_argument, "SNI must not be empty", "sni"));

  Json hysteria_options{{"version", 2}};
  if (const auto idle_timeout = get("udpIdleTimeout")) {
    std::int64_t seconds = 0;
    const auto [end, ec] =
        std::from_chars(idle_timeout->data(),
                        idle_timeout->data() + idle_timeout->size(), seconds);
    if (ec != std::errc{} ||
        end != idle_timeout->data() + idle_timeout->size() || seconds < 2 ||
        seconds > 600)
      return std::unexpected(
          error(ErrorCode::invalid_argument,
                "udpIdleTimeout must be between 2 and 600 seconds",
                "udpIdleTimeout"));
    hysteria_options["udpIdleTimeout"] = seconds;
  }

  StreamNode stream;
  stream.endpoint = Endpoint{parsed->address, parsed->port};
  stream.protocol = Hysteria2Settings{parsed->auth};
  stream.transport = HysteriaTransport{std::move(hysteria_options)};
  stream.security = std::move(tls);

  Node node;
  node.original_name = parsed->fragment.value_or(parsed->address);
  node.settings = std::move(stream);
  return DecodedNode{std::move(node), std::move(parameters->notices)};
}

} // namespace xrm
