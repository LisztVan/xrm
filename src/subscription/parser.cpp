#include "subscription/parser.hpp"

#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace xrm {
namespace {

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = std::nullopt) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

std::string lower(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

bool looks_like_html(std::string_view body) {
  const auto text = lower(trim(body));
  return text.starts_with("<!doctype html") || text.starts_with("<html") ||
         text.starts_with("<head") || text.starts_with("<form") ||
         text.starts_with("<?xml") || text.starts_with("<body");
}

std::string_view uri_scheme(std::string_view value) {
  const auto marker = value.find("://");
  return marker == std::string_view::npos ? std::string_view{} : value.substr(0, marker);
}

bool known_unsupported(std::string_view scheme) {
  const auto normalized = lower(scheme);
  static constexpr std::array<std::string_view, 10> schemes{
      "vmess", "trojan", "ss", "socks", "http", "https", "hy2", "hysteria2",
      "hysteria", "tuic"};
  return std::find(schemes.begin(), schemes.end(), normalized) != schemes.end();
}

void add_issue(ParseReport& report, std::size_t index, IssueKind kind,
               std::string code, std::string message) {
  report.issues.push_back(ParseIssue{index, kind, std::move(code), std::move(message)});
}

void parse_uri_items(const std::vector<std::string_view>& items, ParseReport& report) {
  bool malformed = false;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto item = trim(items[i]);
    if (item.empty()) continue;
    const auto scheme = uri_scheme(item);
    if (known_unsupported(scheme)) {
      add_issue(report, i, IssueKind::unsupported, "unsupported_protocol",
                "subscription URI protocol is not supported");
      continue;
    }
    if (lower(scheme) != "vless") {
      malformed = true;
      add_issue(report, i, IssueKind::malformed, "invalid_uri",
                "subscription item is not a supported URI");
      continue;
    }
    auto parsed = parse_share_uri(item);
    if (!parsed) {
      const bool unsupported = parsed.error().code == ErrorCode::unsupported;
      add_issue(report, i, unsupported ? IssueKind::unsupported : IssueKind::malformed,
                unsupported ? "unsupported_vless_feature" : "invalid_vless_uri",
                unsupported ? "VLESS URI uses an unsupported feature" : "VLESS URI is malformed");
      malformed = malformed || !unsupported;
      continue;
    }
    report.nodes.push_back(std::move(*parsed));
  }
  report.structurally_complete = !items.empty() && !malformed;
}

int base64_value(unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+' || ch == '-') return 62;
  if (ch == '/' || ch == '_') return 63;
  return -1;
}

Result<std::string> decode_base64_once(std::string_view input) {
  std::string compact;
  compact.reserve(input.size());
  for (unsigned char ch : input) {
    if (std::isspace(ch)) continue;
    if (ch != '=' && base64_value(ch) < 0)
      return std::unexpected(error(ErrorCode::invalid_argument, "subscription body is not valid Base64"));
    compact.push_back(static_cast<char>(ch));
  }
  if (compact.empty())
    return std::unexpected(error(ErrorCode::invalid_argument, "subscription body is empty"));

  const auto padding_at = compact.find('=');
  const std::size_t data_size = padding_at == std::string::npos ? compact.size() : padding_at;
  if (padding_at != std::string::npos) {
    const auto padding = compact.size() - padding_at;
    if (padding > 2 || compact.size() % 4 != 0 ||
        std::any_of(compact.begin() + static_cast<std::ptrdiff_t>(padding_at), compact.end(),
                    [](char ch) { return ch != '='; }))
      return std::unexpected(error(ErrorCode::invalid_argument, "subscription body has invalid Base64 padding"));
  } else if (data_size % 4 == 1) {
    return std::unexpected(error(ErrorCode::invalid_argument, "subscription body has invalid Base64 length"));
  }

  std::string decoded;
  decoded.reserve((data_size * 3) / 4 + 2);
  unsigned int accumulator = 0;
  int bits = 0;
  for (std::size_t i = 0; i < data_size; ++i) {
    accumulator = (accumulator << 6) |
        static_cast<unsigned int>(base64_value(static_cast<unsigned char>(compact[i])));
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
    }
  }
  if (bits != 0 && (accumulator & ((1u << bits) - 1u)) != 0)
    return std::unexpected(error(ErrorCode::invalid_argument, "subscription body has invalid Base64 tail"));
  return decoded;
}

bool parse_native_json(const Json& json, ParseReport& report) {
  std::vector<const Json*> outbounds;
  if (json.is_object() && json.contains("protocol")) {
    outbounds.push_back(&json);
  } else if (json.is_array()) {
    for (const auto& value : json) outbounds.push_back(&value);
  } else if (json.is_object() && json.size() == 1 && json.contains("outbounds") &&
             json["outbounds"].is_array()) {
    for (const auto& value : json["outbounds"]) outbounds.push_back(&value);
  } else {
    return false;
  }

  if (outbounds.empty()) {
    report.structurally_complete = true;
    return true;
  }
  bool malformed = false;
  for (std::size_t i = 0; i < outbounds.size(); ++i) {
    auto node = xrm::parse_native_outbound(*outbounds[i], "subscription-parse");
    if (!node) {
      const bool unsupported = node.error().code == ErrorCode::unsupported;
      add_issue(report, i, unsupported ? IssueKind::unsupported : IssueKind::malformed,
                unsupported ? "unsupported_native_reference" : "invalid_native_outbound",
                unsupported ? "native outbound contains an unsupported external reference"
                            : "native outbound is incomplete or malformed");
      malformed = malformed || !unsupported;
      continue;
    }
    report.nodes.push_back(ParsedNode{std::nullopt, node->original_name, std::move(node->settings)});
  }
  report.structurally_complete = !malformed;
  return true;
}

Result<ParseReport> parse_content(std::string_view body) {
  ParseReport report;
  if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
  body = trim(body);
  if (body.empty()) {
    add_issue(report, 0, IssueKind::malformed, "empty_body", "subscription body is empty");
    return report;
  }
  if (looks_like_html(body)) {
    add_issue(report, 0, IssueKind::malformed, "html_body", "subscription response is HTML, not a node list");
    return report;
  }
  if (body.front() == '{' || body.front() == '[') {
    Json value;
    try {
      value = Json::parse(body);
    } catch (const Json::exception&) {
      add_issue(report, 0, IssueKind::malformed, "invalid_json", "subscription JSON is malformed");
      return report;
    }
    if (!parse_native_json(value, report))
      add_issue(report, 0, IssueKind::malformed, "not_native_outbound",
                "JSON is not a native outbound or outbound collection");
    return report;
  }

  std::vector<std::string_view> items;
  bool uri_like = false;
  std::size_t offset = 0;
  while (offset <= body.size()) {
    const auto end = body.find('\n', offset);
    auto line = trim(body.substr(offset, end == std::string_view::npos ? body.size() - offset : end - offset));
    if (!line.empty()) {
      items.push_back(line);
      uri_like = uri_like || line.find("://") != std::string_view::npos;
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  if (uri_like) {
    parse_uri_items(items, report);
    return report;
  }
  add_issue(report, 0, IssueKind::malformed, "unrecognized_body",
            "subscription body is not a recognized node format");
  return report;
}

}  // namespace

Result<ParsedNode> parse_share_uri(std::string_view uri) {
  const auto scheme = uri_scheme(trim(uri));
  if (lower(scheme) != "vless") {
    if (known_unsupported(scheme))
      return std::unexpected(error(ErrorCode::unsupported, "subscription URI protocol is not supported", "scheme"));
    return std::unexpected(error(ErrorCode::invalid_argument, "subscription URI scheme is invalid", "scheme"));
  }
  auto decoded = parse_vless_uri(uri);
  if (!decoded) return std::unexpected(decoded.error());
  return ParsedNode{std::nullopt, decoded->node.original_name, std::move(decoded->node.settings)};
}

Result<ParsedNode> parse_native_outbound(std::string_view text, std::string_view fallback_name) {
  Json outbound;
  try {
    outbound = Json::parse(text);
  } catch (const Json::exception&) {
    return std::unexpected(error(ErrorCode::invalid_argument, "native outbound JSON is malformed"));
  }
  auto node = xrm::parse_native_outbound(outbound, "subscription-parse", std::string(fallback_name));
  if (!node) return std::unexpected(node.error());
  return ParsedNode{std::nullopt, node->original_name, std::move(node->settings)};
}

Result<ParseReport> parse_subscription(std::string_view body) {
  if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
  body = trim(body);
  if (body.empty()) {
    ParseReport report;
    add_issue(report, 0, IssueKind::malformed, "empty_body", "subscription body is empty");
    return report;
  }
  const auto scheme = uri_scheme(body);
  if (!scheme.empty() || body.front() == '{' || body.front() == '[' || looks_like_html(body) ||
      body.find("://") != std::string_view::npos)
    return parse_content(body);

  auto decoded = decode_base64_once(body);
  if (!decoded) {
    ParseReport report;
    add_issue(report, 0, IssueKind::malformed, "unrecognized_body",
              "subscription body is not a recognized node format");
    return report;
  }
  // Deliberately decode exactly one subscription wrapper.
  return parse_content(*decoded);
}

}  // namespace xrm
