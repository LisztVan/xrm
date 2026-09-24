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
    Node transient;
    transient.settings = *stream;
    const auto serialized = Json(transient).at("settings");
    const auto kind = std::visit([](const auto& protocol) -> std::string_view {
      using T = std::decay_t<decltype(protocol)>;
      if constexpr (std::is_same_v<T, VlessSettings>) return "vless";
      if constexpr (std::is_same_v<T, VmessSettings>) return "vmess";
      if constexpr (std::is_same_v<T, TrojanSettings>) return "trojan";
      if constexpr (std::is_same_v<T, ShadowsocksSettings>) return "shadowsocks";
      return "hysteria2";
    }, stream->protocol);
    // Secret-bearing settings are compared transiently and are never public IDs or logs.
    return std::string(kind) + "\n" + serialized.dump();
  }
  Json outbound = std::get<RawOutbound>(settings).value;
  if (!outbound.is_object()) return {};
  outbound.erase("tag");
  return "native\n" + outbound.dump();
}

bool settings_equal(const NodeSettings& left, const NodeSettings& right) {
  if (left.index() != right.index()) return false;
  if (const auto* a = std::get_if<RawOutbound>(&left))
    return a->value == std::get<RawOutbound>(right).value;
  const auto& a = std::get<StreamNode>(left);
  const auto& b = std::get<StreamNode>(right);
  Node left_node, right_node;
  left_node.settings = a;
  right_node.settings = b;
  return Json(left_node).at("settings") == Json(right_node).at("settings");
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
