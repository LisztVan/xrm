#include "subscription/reconcile.hpp"

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

std::string connection_identity(const NodeSettings& settings) {
  if (const auto* stream = std::get_if<StreamNode>(&settings)) {
    Json transport;
    if (std::holds_alternative<RawTransport>(stream->transport)) {
      transport = Json{{"type", "raw"}};
    } else if (const auto* value = std::get_if<XHttpTransport>(&stream->transport)) {
      transport = Json{{"type", "xhttp"},
                       {"host", value->host ? Json(*value->host) : Json(nullptr)},
                       {"path", value->path ? Json(*value->path) : Json(nullptr)},
                       {"mode", value->mode ? Json(*value->mode) : Json(nullptr)},
                       {"extra", value->extra.value_or(Json(nullptr))}};
    } else if (const auto* value = std::get_if<WebSocketTransport>(&stream->transport)) {
      transport = Json{{"type", "websocket"},
                       {"path", value->path ? Json(*value->path) : Json(nullptr)},
                       {"headers", value->headers}};
    } else if (const auto* value = std::get_if<GrpcTransport>(&stream->transport)) {
      transport = Json{{"type", "grpc"},
                       {"service_name", value->service_name ? Json(*value->service_name) : Json(nullptr)},
                       {"authority", value->authority ? Json(*value->authority) : Json(nullptr)}};
    } else {
      const auto& http_upgrade = std::get<HttpUpgradeTransport>(stream->transport);
      transport = Json{{"type", "httpupgrade"},
                       {"host", http_upgrade.host ? Json(*http_upgrade.host) : Json(nullptr)},
                       {"path", http_upgrade.path ? Json(*http_upgrade.path) : Json(nullptr)}};
    }

    Json security;
    if (std::holds_alternative<NoSecurity>(stream->security)) {
      security = Json{{"type", "none"}};
    } else if (const auto* value = std::get_if<TlsSecurity>(&stream->security)) {
      security = Json{{"type", "tls"},
                      {"server_name", value->server_name ? Json(*value->server_name) : Json(nullptr)},
                      {"fingerprint", value->fingerprint ? Json(*value->fingerprint) : Json(nullptr)},
                      {"alpn", value->alpn},
                      {"options", value->options}};
    } else {
      const auto& reality = std::get<RealitySecurity>(stream->security);
      security = Json{{"type", "reality"},
                      {"server_name", reality.server_name ? Json(*reality.server_name) : Json(nullptr)},
                      {"fingerprint", reality.fingerprint ? Json(*reality.fingerprint) : Json(nullptr)},
                      {"password", reality.password},
                      {"short_id", reality.short_id ? Json(*reality.short_id) : Json(nullptr)},
                      {"spider_x", reality.spider_x ? Json(*reality.spider_x) : Json(nullptr)},
                      {"options", reality.options}};
    }

    // These secret-bearing values are compared transiently and are never public IDs or logs.
    const Json identity{
        {"protocol", "vless"},
        {"address", stream->endpoint.address},
        {"port", stream->endpoint.port},
        {"id", stream->protocol.id},
        {"encryption", stream->protocol.encryption ? Json(*stream->protocol.encryption) : Json(nullptr)},
        {"flow", stream->protocol.flow ? Json(*stream->protocol.flow) : Json(nullptr)},
        {"level", stream->protocol.level ? Json(*stream->protocol.level) : Json(nullptr)},
        {"transport", std::move(transport)},
        {"security", std::move(security)},
        {"sockopt", stream->sockopt.value_or(Json(nullptr))},
        {"finalmask", stream->finalmask.value_or(Json(nullptr))},
        {"mux", stream->mux.value_or(Json(nullptr))}};
    return "vless\n" + identity.dump();
  }
  Json outbound = std::get<RawOutbound>(settings).value;
  if (!outbound.is_object()) return {};
  outbound.erase("tag");
  return "native\n" + outbound.dump();
}

bool transport_equal(const Transport& left, const Transport& right) {
  if (left.index() != right.index()) return false;
  if (const auto* a = std::get_if<RawTransport>(&left)) {
    (void)a;
    return true;
  }
  if (const auto* a = std::get_if<XHttpTransport>(&left)) {
    const auto& b = std::get<XHttpTransport>(right);
    return a->host == b.host && a->path == b.path && a->mode == b.mode && a->extra == b.extra;
  }
  if (const auto* a = std::get_if<WebSocketTransport>(&left)) {
    const auto& b = std::get<WebSocketTransport>(right);
    return a->path == b.path && a->headers == b.headers;
  }
  if (const auto* a = std::get_if<GrpcTransport>(&left)) {
    const auto& b = std::get<GrpcTransport>(right);
    return a->service_name == b.service_name && a->authority == b.authority;
  }
  const auto& a = std::get<HttpUpgradeTransport>(left);
  const auto& b = std::get<HttpUpgradeTransport>(right);
  return a.host == b.host && a.path == b.path;
}

bool security_equal(const Security& left, const Security& right) {
  if (left.index() != right.index()) return false;
  if (std::holds_alternative<NoSecurity>(left)) return true;
  if (const auto* a = std::get_if<TlsSecurity>(&left)) {
    const auto& b = std::get<TlsSecurity>(right);
    return a->server_name == b.server_name && a->fingerprint == b.fingerprint &&
           a->alpn == b.alpn && a->options == b.options;
  }
  const auto& a = std::get<RealitySecurity>(left);
  const auto& b = std::get<RealitySecurity>(right);
  return a.server_name == b.server_name && a.fingerprint == b.fingerprint &&
         a.password == b.password && a.short_id == b.short_id &&
         a.spider_x == b.spider_x && a.options == b.options;
}

bool settings_equal(const NodeSettings& left, const NodeSettings& right) {
  if (left.index() != right.index()) return false;
  if (const auto* a = std::get_if<RawOutbound>(&left))
    return a->value == std::get<RawOutbound>(right).value;
  const auto& a = std::get<StreamNode>(left);
  const auto& b = std::get<StreamNode>(right);
  return a.endpoint.address == b.endpoint.address && a.endpoint.port == b.endpoint.port &&
         a.protocol.id == b.protocol.id && a.protocol.encryption == b.protocol.encryption &&
         a.protocol.flow == b.protocol.flow && a.protocol.level == b.protocol.level &&
         transport_equal(a.transport, b.transport) && security_equal(a.security, b.security) &&
         a.sockopt == b.sockopt && a.finalmask == b.finalmask && a.mux == b.mux;
}

}  // namespace

Result<ReconcileResult> reconcile(const ReconcileInput& input,
                                  const std::function<Id()>& make_id) {
  if (input.source.id.empty())
    return std::unexpected(error(ErrorCode::invalid_argument, "subscription source id must not be empty", "source_id"));
  if (!make_id)
    return std::unexpected(error(ErrorCode::invalid_argument, "node id generator is required", "make_id"));

  ReconcileResult result;
  result.report.source_id = input.source.id;
  for (const auto& item : input.parsed.issues) {
    if (item.kind == IssueKind::malformed) ++result.report.malformed;
    if (item.kind == IssueKind::unsupported) ++result.report.unsupported;
  }
  result.report.partial = !input.parsed.issues.empty();

  std::vector<const Node*> old_nodes;
  for (const auto& node : input.old_nodes)
    if (node.source_id == input.source.id) old_nodes.push_back(&node);

  std::vector<std::optional<std::size_t>> matched(input.parsed.nodes.size());
  std::set<std::size_t> used_old;
  auto match_unique = [&](auto old_key, auto parsed_key) {
    std::map<std::string, std::vector<std::size_t>> old_groups;
    std::map<std::string, std::vector<std::size_t>> parsed_groups;
    for (std::size_t i = 0; i < old_nodes.size(); ++i) {
      if (used_old.contains(i)) continue;
      const std::string key = old_key(*old_nodes[i]);
      if (!key.empty()) old_groups[key].push_back(i);
    }
    for (std::size_t i = 0; i < input.parsed.nodes.size(); ++i) {
      if (matched[i]) continue;
      const std::string key = parsed_key(input.parsed.nodes[i]);
      if (!key.empty()) parsed_groups[key].push_back(i);
    }
    for (const auto& [key, old_indices] : old_groups) {
      const auto found = parsed_groups.find(key);
      if (old_indices.size() != 1 || found == parsed_groups.end() || found->second.size() != 1) continue;
      matched[found->second.front()] = old_indices.front();
      used_old.insert(old_indices.front());
    }
  };

  match_unique(
      [](const Node& node) { return node.upstream_id.value_or(std::string{}); },
      [](const ParsedNode& node) { return node.upstream_id.value_or(std::string{}); });
  match_unique(
      [](const Node& node) { return connection_identity(node.settings); },
      [](const ParsedNode& node) { return connection_identity(node.settings); });

  for (std::size_t i = 0; i < input.parsed.nodes.size(); ++i) {
    const auto& parsed = input.parsed.nodes[i];
    if (matched[i]) {
      const auto& old = *old_nodes[*matched[i]];
      Node updated = old;
      updated.original_name = parsed.name;
      updated.settings = parsed.settings;
      updated.upstream_id = parsed.upstream_id;
      updated.source_missing = false;
      if (updated.original_name != old.original_name || !settings_equal(updated.settings, old.settings) ||
          updated.upstream_id != old.upstream_id || old.source_missing) {
        result.changes.upserts.push_back(std::move(updated));
        ++result.report.updated;
        result.report.changed = true;
      }
      continue;
    }

    Node added;
    added.id = make_id();
    if (added.id.empty())
      return std::unexpected(error(ErrorCode::invalid_argument, "node id generator returned an empty id", "make_id"));
    added.source_id = input.source.id;
    added.original_name = parsed.name;
    added.upstream_id = parsed.upstream_id;
    added.settings = parsed.settings;
    result.changes.upserts.push_back(std::move(added));
    ++result.report.added;
    result.report.changed = true;
  }

  // A list with any parse issue cannot prove that absent old nodes disappeared.
  const bool complete = input.parsed.structurally_complete &&
                        !input.parsed.nodes.empty() && input.parsed.issues.empty();
  if (complete) {
    for (std::size_t i = 0; i < old_nodes.size(); ++i) {
      if (used_old.contains(i) || old_nodes[i]->source_missing) continue;
      Node missing = *old_nodes[i];
      missing.source_missing = true;
      result.changes.upserts.push_back(std::move(missing));
      ++result.report.missing;
      result.report.changed = true;
    }
  }
  return result;
}

}  // namespace xrm
