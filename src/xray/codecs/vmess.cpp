// XRM: Decodes supported VMess share payloads into typed node settings.
// This parser's link format is distinct from arbitrary native outbound JSON,
// which has its own storage and assembly path.

#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace xrm {
namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

int base64_value(unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z')
    return ch - 'A';
  if (ch >= 'a' && ch <= 'z')
    return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9')
    return ch - '0' + 52;
  if (ch == '+' || ch == '-')
    return 62;
  if (ch == '/' || ch == '_')
    return 63;
  return -1;
}

Result<std::string> decode_base64(std::string_view input) {
  if (input.empty())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "VMess payload is empty", "payload"));
  const auto padding_at = input.find('=');
  const auto content = input.substr(0, padding_at);
  const auto padding = padding_at == std::string_view::npos
                           ? std::size_t{0}
                           : input.size() - padding_at;
  if (padding > 2 ||
      (padding_at != std::string_view::npos && input.size() % 4 != 0))
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "invalid base64 padding", "payload"));
  if (content.size() % 4 == 1 ||
      (padding != 0 && content.size() % 4 + padding != 4))
    return std::unexpected(
        error(ErrorCode::invalid_argument, "invalid base64 length", "payload"));

  std::string output;
  output.reserve(content.size() * 3 / 4 + 2);
  std::uint32_t accumulator = 0;
  unsigned int bits = 0;
  for (const unsigned char ch : content) {
    if (std::isspace(ch) != 0)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "whitespace in base64 payload", "payload"));
    const int value = base64_value(ch);
    if (value < 0)
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "invalid base64 character", "payload"));
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<char>((accumulator >> bits) & 0xffU));
    }
  }
  if (bits != 0 && (accumulator & ((std::uint32_t{1} << bits) - 1U)) != 0)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "non-canonical base64 payload", "payload"));
  const std::size_t expected_padding = content.size() % 4 == 2   ? 2
                                       : content.size() % 4 == 3 ? 1
                                                                 : 0;
  if (padding != 0 && padding != expected_padding)
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "invalid base64 padding", "payload"));
  return output;
}

Result<std::string> required_string(const Json &value, std::string_view key) {
  const auto it = value.find(std::string(key));
  if (it == value.end() || !it->is_string() ||
      it->get_ref<const std::string &>().empty())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "VMess field must be a non-empty string",
                                 std::string(key)));
  return it->get<std::string>();
}

Result<std::optional<std::string>> optional_string(const Json &value,
                                                   std::string_view key) {
  const auto it = value.find(std::string(key));
  if (it == value.end() || it->is_null())
    return std::optional<std::string>{};
  if (!it->is_string())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "VMess field must be a string",
                                 std::string(key)));
  return std::optional<std::string>(it->get<std::string>());
}

Result<std::uint16_t> parse_port(const Json &value) {
  const auto it = value.find("port");
  if (it == value.end())
    return std::unexpected(
        error(ErrorCode::invalid_argument, "VMess port is required", "port"));
  std::uint32_t port = 0;
  if (it->is_string()) {
    const auto &text = it->get_ref<const std::string &>();
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc{} || end != text.data() + text.size())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "VMess port is invalid", "port"));
  } else if (it->is_number_unsigned()) {
    const auto number = it->get<std::uint64_t>();
    if (number > std::numeric_limits<std::uint32_t>::max())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "VMess port is invalid", "port"));
    port = static_cast<std::uint32_t>(number);
  } else if (it->is_number_integer()) {
    const auto number = it->get<std::int64_t>();
    if (number < 0 || number > std::numeric_limits<std::uint16_t>::max())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "VMess port is invalid", "port"));
    port = static_cast<std::uint32_t>(number);
  } else {
    return std::unexpected(
        error(ErrorCode::invalid_argument, "VMess port is invalid", "port"));
  }
  if (port == 0 || port > std::numeric_limits<std::uint16_t>::max())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "VMess port is outside 1..65535", "port"));
  return static_cast<std::uint16_t>(port);
}

Result<std::uint32_t> parse_aid(const Json &value) {
  const auto it = value.find("aid");
  if (it == value.end())
    return std::uint32_t{0};
  std::uint32_t aid = 0;
  if (it->is_string()) {
    const auto &text = it->get_ref<const std::string &>();
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), aid);
    if (ec != std::errc{} || end != text.data() + text.size())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "VMess aid is invalid", "aid"));
  } else if (it->is_number_unsigned()) {
    const auto number = it->get<std::uint64_t>();
    if (number > std::numeric_limits<std::uint32_t>::max())
      return std::unexpected(error(ErrorCode::unsupported,
                                   "nonzero VMess aid is unsupported", "aid"));
    aid = static_cast<std::uint32_t>(number);
  } else if (it->is_number_integer()) {
    const auto number = it->get<std::int64_t>();
    if (number < 0 || number > std::numeric_limits<std::uint32_t>::max())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "VMess aid is invalid", "aid"));
    aid = static_cast<std::uint32_t>(number);
  } else {
    return std::unexpected(
        error(ErrorCode::invalid_argument, "VMess aid is invalid", "aid"));
  }
  if (aid != 0)
    return std::unexpected(error(ErrorCode::unsupported,
                                 "nonzero VMess aid is unsupported", "aid"));
  return aid;
}

Result<std::vector<std::string>>
parse_alpn(const std::optional<std::string> &value) {
  std::vector<std::string> result;
  if (!value || value->empty())
    return result;
  std::size_t begin = 0;
  while (begin <= value->size()) {
    const auto comma = value->find(',', begin);
    const auto item = value->substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (item.empty())
      return std::unexpected(
          error(ErrorCode::invalid_argument, "ALPN item is empty", "alpn"));
    result.push_back(item);
    if (comma == std::string::npos)
      break;
    begin = comma + 1;
  }
  return result;
}

std::optional<std::string> nonempty(const std::optional<std::string> &value) {
  if (value && !value->empty())
    return value;
  return std::nullopt;
}

Result<StreamNode> make_stream(const Json &share, std::string address,
                               std::uint16_t port, std::string id,
                               std::string security) {
  auto network_value = optional_string(share, "net");
  auto host_value = optional_string(share, "host");
  auto path_value = optional_string(share, "path");
  auto header_type = optional_string(share, "type");
  auto tls_value = optional_string(share, "tls");
  auto sni_value = optional_string(share, "sni");
  auto alpn_value = optional_string(share, "alpn");
  auto fp_value = optional_string(share, "fp");
  for (const auto *item :
       {&network_value, &host_value, &path_value, &header_type, &tls_value,
        &sni_value, &alpn_value, &fp_value}) {
    if (!*item)
      return std::unexpected((*item).error());
  }

  const std::string network = network_value->value_or("tcp");
  const std::string type = header_type->value_or("none");
  if (type != "none" &&
      !((network == "tcp" || network == "raw") && type == "http"))
    return std::unexpected(error(ErrorCode::unsupported,
                                 "VMess transport header type is unsupported",
                                 "type"));
  if (type == "http")
    return std::unexpected(error(ErrorCode::unsupported,
                                 "VMess HTTP header transport is not supported",
                                 "type"));

  StreamNode stream;
  stream.endpoint = Endpoint{std::move(address), port};
  stream.protocol =
      VmessSettings{std::move(id), std::move(security), std::nullopt};

  if (network == "tcp" || network == "raw") {
    if (host_value->has_value() || path_value->has_value())
      return std::unexpected(
          error(ErrorCode::unsupported,
                "VMess host/path require a supported transport", "net"));
    stream.transport = RawTransport{};
  } else if (network == "ws" || network == "websocket") {
    WebSocketTransport transport;
    transport.path = nonempty(*path_value);
    if (const auto host = nonempty(*host_value))
      transport.headers.emplace("Host", *host);
    stream.transport = std::move(transport);
  } else if (network == "grpc") {
    stream.transport =
        GrpcTransport{nonempty(*path_value), nonempty(*host_value)};
  } else if (network == "httpupgrade") {
    stream.transport =
        HttpUpgradeTransport{nonempty(*host_value), nonempty(*path_value)};
  } else if (network == "xhttp") {
    stream.transport =
        XHttpTransport{nonempty(*host_value), nonempty(*path_value),
                       std::nullopt, std::nullopt};
  } else {
    return std::unexpected(
        error(ErrorCode::unsupported, "unsupported VMess transport", "net"));
  }

  const auto tls = tls_value->value_or(std::string{});
  auto alpn = parse_alpn(*alpn_value);
  if (!alpn)
    return std::unexpected(alpn.error());
  if (tls.empty() || tls == "none") {
    if ((sni_value->has_value() && !sni_value->value().empty()) ||
        (fp_value->has_value() && !fp_value->value().empty()) || !alpn->empty())
      return std::unexpected(error(ErrorCode::invalid_argument,
                                   "TLS fields require VMess tls=tls", "tls"));
    stream.security = NoSecurity{};
  } else if (tls == "tls") {
    TlsSecurity settings;
    if (sni_value->has_value() && !sni_value->value().empty())
      settings.server_name = sni_value->value();
    if (fp_value->has_value() && !fp_value->value().empty())
      settings.fingerprint = fp_value->value();
    settings.alpn = std::move(*alpn);
    stream.security = std::move(settings);
  } else {
    return std::unexpected(error(
        ErrorCode::unsupported, "unsupported VMess security transport", "tls"));
  }
  return stream;
}

} // namespace

Result<DecodedNode> parse_vmess_uri(std::string_view uri) {
  constexpr std::string_view scheme = "vmess://";
  if (!uri.starts_with(scheme))
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "expected vmess URI scheme", "scheme"));
  for (const unsigned char ch : uri) {
    if (ch < 0x20 || ch == 0x7f)
      return std::unexpected(
          error(ErrorCode::invalid_argument, "control character in VMess URI"));
  }
  auto decoded = decode_base64(uri.substr(scheme.size()));
  if (!decoded)
    return std::unexpected(decoded.error());

  Json share;
  try {
    share = Json::parse(*decoded);
  } catch (const Json::exception &) {
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "VMess payload is not valid JSON", "payload"));
  }
  if (!share.is_object())
    return std::unexpected(error(ErrorCode::invalid_argument,
                                 "VMess payload must be a JSON object",
                                 "payload"));

  static const std::set<std::string, std::less<>> allowed{
      "v",    "ps",   "add",  "port", "id",  "aid",  "scy", "net",
      "type", "host", "path", "tls",  "sni", "alpn", "fp"};
  for (auto it = share.begin(); it != share.end(); ++it) {
    if (!allowed.contains(it.key()))
      return std::unexpected(error(ErrorCode::unsupported,
                                   "unsupported VMess share field", it.key()));
  }

  if (const auto version = share.find("v"); version != share.end()) {
    if (!version->is_string() || version->get<std::string>() != "2")
      return std::unexpected(error(ErrorCode::unsupported,
                                   "unsupported VMess share version", "v"));
  }
  auto address = required_string(share, "add");
  if (!address)
    return std::unexpected(address.error());
  auto id = required_string(share, "id");
  if (!id)
    return std::unexpected(id.error());
  auto port = parse_port(share);
  if (!port)
    return std::unexpected(port.error());
  auto aid = parse_aid(share);
  if (!aid)
    return std::unexpected(aid.error());
  (void)aid;

  auto security_value = optional_string(share, "scy");
  if (!security_value)
    return std::unexpected(security_value.error());
  const std::string security = security_value->value_or("auto");
  if (security != "auto" && security != "aes-128-gcm" &&
      security != "chacha20-poly1305")
    return std::unexpected(error(ErrorCode::unsupported,
                                 "unsupported VMess user security", "scy"));

  auto name = optional_string(share, "ps");
  if (!name)
    return std::unexpected(name.error());
  auto stream =
      make_stream(share, std::move(*address), *port, std::move(*id), security);
  if (!stream)
    return std::unexpected(stream.error());

  Node node;
  node.original_name = name->value_or(stream->endpoint.address);
  node.settings = std::move(*stream);
  return DecodedNode{std::move(node), {}};
}

} // namespace xrm
