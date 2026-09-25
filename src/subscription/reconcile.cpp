#include "subscription/reconcile.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

  // Identical items in one response describe one connection. Pick the same
  // display row regardless of provider ordering, before assigning any ID.
  std::map<std::string, std::size_t> unique_parsed;
  std::vector<std::size_t> parsed_indices;
  for (std::size_t i = 0; i < input.parsed.nodes.size(); ++i) {
    const auto& node = input.parsed.nodes[i];
    const auto key = connection_identity(node.settings);
    if (key.empty()) {
      parsed_indices.push_back(i);
      continue;
    }
    auto [found, inserted] = unique_parsed.emplace(key, parsed_indices.size());
    if (inserted) {
      parsed_indices.push_back(i);
    } else {
      const auto& current = input.parsed.nodes[parsed_indices[found->second]];
      if (std::tie(node.name, node.upstream_id) <
          std::tie(current.name, current.upstream_id)) {
        parsed_indices[found->second] = i;
      }
    }
  }

  std::vector<std::optional<std::size_t>> matched(input.parsed.nodes.size());
  std::set<std::size_t> used_old;
  std::set<std::size_t> present_old;
  auto match_unique = [&](auto old_key, auto parsed_key, bool allow_equivalent_old) {
    std::map<std::string, std::vector<std::size_t>> old_groups;
    std::map<std::string, std::vector<std::size_t>> parsed_groups;
    for (std::size_t i = 0; i < old_nodes.size(); ++i) {
      if (used_old.contains(i)) continue;
      const std::string key = old_key(*old_nodes[i]);
      if (!key.empty()) old_groups[key].push_back(i);
    }
    for (const auto i : parsed_indices) {
      if (matched[i]) continue;
      const std::string key = parsed_key(input.parsed.nodes[i]);
      if (!key.empty()) parsed_groups[key].push_back(i);
    }
    for (const auto& [key, old_indices] : old_groups) {
      const auto found = parsed_groups.find(key);
      if (found == parsed_groups.end() || found->second.size() != 1 ||
          (!allow_equivalent_old && old_indices.size() != 1)) continue;
      // Historical equivalent duplicates may have references or user edits.
      // Reuse one stable ID, but leave the others untouched for an explicit
      // user-reviewed cleanup instead of silently deleting or disabling them.
      const auto chosen = *std::min_element(old_indices.begin(), old_indices.end(),
          [&](std::size_t left, std::size_t right) {
            if (old_nodes[left]->source_missing != old_nodes[right]->source_missing)
              return !old_nodes[left]->source_missing;
            return old_nodes[left]->id < old_nodes[right]->id;
          });
      matched[found->second.front()] = chosen;
      used_old.insert(chosen);
      if (allow_equivalent_old) present_old.insert(old_indices.begin(), old_indices.end());
      else present_old.insert(chosen);
    }
  };

  // An exact connection wins over a provider row ID. A reused upstream ID
  // must never turn two distinct live endpoints into one node.
  match_unique(
      [](const Node& node) { return connection_identity(node.settings); },
      [](const ParsedNode& node) { return connection_identity(node.settings); }, true);
  match_unique(
      [](const Node& node) { return node.upstream_id.value_or(std::string{}); },
      [](const ParsedNode& node) { return node.upstream_id.value_or(std::string{}); }, false);

  for (const auto i : parsed_indices) {
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
      if (present_old.contains(i) || old_nodes[i]->source_missing) continue;
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
