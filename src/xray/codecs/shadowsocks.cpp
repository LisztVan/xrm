/// @file
/// @brief Decodes supported Shadowsocks share URIs into typed node settings.
///
/// URI support is deliberately narrower than native Xray JSON; outbound
/// generation for stored typed nodes is maintained in codec.cpp.

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

int base64_value(char value) {
  if (value >= 'A' && value <= 'Z')
    return value - 'A';
  if (value >= 'a' && value <= 'z')
    return value - 'a' + 26;
  if (value >= '0' && value <= '9')
    return value - '0' + 52;
  if (value == '+' || value == '-')
    return 62;
  if (value == '/' || value == '_')
    return 63;
  return -1;
}

Result<std::string> decode_base64(std::string_view encoded) {
  std::string normalized;
  normalized.reserve(encoded.size() + 3);
  std::size_t padding = 0;
  bool saw_padding = false;
  for (char value : encoded) {
    if (value == '=') {
      saw_padding = true;
      ++padding;
      if (padding > 2)
        return std::unexpected(error(ErrorCode::invalid_argument,
                                     "invalid Base64 padding", "userinfo"));
      continue;
    }
    if (saw_padding || base64_value(value) < 0)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "invalid Base64 userinfo", "userinfo"));
    normalized.push_back(value == '-' ? '+' : value == '_' ? '/' : value);
  }
  if (normalized.size() % 4 == 1)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "invalid Base64 length", "userinfo"));
  const auto expected_padding = (4 - (normalized.size() % 4)) % 4;
  if (padding != 0 && (normalized.size() + padding) % 4 != 0)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "invalid Base64 padding", "userinfo"));
  if (padding != 0 && padding != expected_padding)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "invalid Base64 padding", "userinfo"));
  normalized.append(expected_padding, '=');

  std::string decoded;
  decoded.reserve(normalized.size() / 4 * 3);
  for (std::size_t i = 0; i < normalized.size(); i += 4) {
    const int a = base64_value(normalized[i]);
    const int b = base64_value(normalized[i + 1]);
    const int c =
        normalized[i + 2] == '=' ? 0 : base64_value(normalized[i + 2]);
    const int d =
        normalized[i + 3] == '=' ? 0 : base64_value(normalized[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "invalid Base64 userinfo", "userinfo"));
    decoded.push_back(static_cast<char>((a << 2) | (b >> 4)));
    if (normalized[i + 2] != '=')
      decoded.push_back(static_cast<char>((b << 4) | (c >> 2)));
    if (normalized[i + 3] != '=')
      decoded.push_back(static_cast<char>((c << 6) | d));
  }
  for (unsigned char value : decoded) {
    if (is_control(value))
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "control character in decoded userinfo",
                                   "userinfo"));
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

struct Credentials {
  std::string method;
  std::string password;
};

Result<Credentials> split_credentials(std::string_view decoded) {
  const auto colon = decoded.find(':');
  if (colon == std::string_view::npos || colon == 0 ||
      colon + 1 >= decoded.size())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "userinfo must encode method:password",
                                 "userinfo"));
  return Credentials{std::string(decoded.substr(0, colon)),
                     std::string(decoded.substr(colon + 1))};
}

Result<Credentials> parse_credentials(std::string_view userinfo) {
  auto decoded_userinfo = percent_decode(userinfo, "userinfo");
  if (!decoded_userinfo)
    return std::unexpected(decoded_userinfo.error());
  auto decoded = decode_base64(*decoded_userinfo);
  if (!decoded)
    return std::unexpected(decoded.error());
  return split_credentials(*decoded);
}

bool supported_method(std::string_view method) {
  static const std::set<std::string_view> methods{
      "aes-128-gcm",
      "aes-256-gcm",
      "chacha20-ietf-poly1305",
      "xchacha20-poly1305",
      "2022-blake3-aes-128-gcm",
      "2022-blake3-aes-256-gcm",
      "2022-blake3-chacha20-poly1305"};
  return methods.contains(method);
}

struct ParsedUri {
  Credentials credentials;
  Authority authority;
  std::optional<std::string> fragment;
  std::vector<Notice> notices;
};

Result<ParsedUri> parse_uri(std::string_view uri) {
  for (unsigned char ch : uri) {
    if (is_control(ch))
      return std::unexpected(
          error(ErrorCode::invalid_argument, "control character in URI"));
  }
  constexpr std::string_view scheme = "ss://";
  if (!uri.starts_with(scheme))
    return std::unexpected(
        error(ErrorCode::invalid_argument, "expected ss URI scheme", "scheme"));
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
  bool plugin_seen = false;
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
    if (*key != "plugin")
      return std::unexpected(error(ErrorCode::unsupported,
                                   "unsupported Shadowsocks URI parameter",
                                   *key));
    if (*value != "")
      return std::unexpected(error(ErrorCode::unsupported,
                                   "Shadowsocks plugins are unsupported",
                                   "plugin"));
    if (plugin_seen)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "duplicate plugin parameter", "plugin"));
    plugin_seen = true;
  }

  const auto at = uri.find('@');
  if (at != std::string_view::npos) {
    if (uri.find('@', at + 1) != std::string_view::npos)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "multiple authority separators",
                                   "authority"));
    auto credentials = parse_credentials(uri.substr(0, at));
    if (!credentials)
      return std::unexpected(credentials.error());
    auto authority = parse_authority(uri.substr(at + 1));
    if (!authority)
      return std::unexpected(authority.error());
    result.credentials = std::move(*credentials);
    result.authority = std::move(*authority);
  } else {
    auto decoded = decode_base64(uri);
    if (!decoded)
      return std::unexpected(decoded.error());
    const auto decoded_at = decoded->find('@');
    if (decoded_at == std::string::npos ||
        decoded->find('@', decoded_at + 1) != std::string::npos)
      return std::unexpected(error(
          ErrorCode::invalid_argument,
          "legacy share must encode method:password@authority", "userinfo"));
    auto credentials =
        split_credentials(std::string_view(*decoded).substr(0, decoded_at));
    if (!credentials)
      return std::unexpected(credentials.error());
    auto authority =
        parse_authority(std::string_view(*decoded).substr(decoded_at + 1));
    if (!authority)
      return std::unexpected(authority.error());
    result.credentials = std::move(*credentials);
    result.authority = std::move(*authority);
  }
  if (!supported_method(result.credentials.method))
    return std::unexpected(error(ErrorCode::unsupported,
                                 "unsupported Shadowsocks method", "method"));
  return result;
}

} // namespace

Result<DecodedNode> parse_shadowsocks_uri(std::string_view uri) {
  // Accept only URI forms whose credentials and endpoint can be mapped
  // without guessing a different encryption or transport meaning.
  auto parsed = parse_uri(uri);
  if (!parsed)
    return std::unexpected(parsed.error());

  StreamNode stream;
  stream.endpoint = Endpoint{parsed->authority.address, parsed->authority.port};
  stream.protocol = ShadowsocksSettings{parsed->credentials.method,
                                        parsed->credentials.password};
  stream.transport = RawTransport{};
  stream.security = NoSecurity{};

  Node node;
  node.original_name = parsed->fragment.value_or(parsed->authority.address);
  node.settings = std::move(stream);
  return DecodedNode{std::move(node), std::move(parsed->notices)};
}

} // namespace xrm
