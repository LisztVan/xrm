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
  if (std::holds_alternative<RawOutbound>(left))
    return connection_identity(left) == connection_identity(right);
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

  // Connection settings, not display names or provider row order, identify
  // one usable exit within a source. The parser has already normalized URI
  // syntax and known equivalent defaults into NodeSettings.
  std::map<std::string, std::vector<std::size_t>> parsed_by_connection;
  for (std::size_t i = 0; i < input.parsed.nodes.size(); ++i) {
    const auto& node = input.parsed.nodes[i];
    parsed_by_connection[connection_identity(node.settings)].push_back(i);
  }
  std::map<std::string, std::vector<std::size_t>> old_by_connection;
  for (std::size_t i = 0; i < old_nodes.size(); ++i)
    old_by_connection[connection_identity(old_nodes[i]->settings)].push_back(i);
  std::set<std::size_t> retained_old;
  const bool complete = input.parsed.structurally_complete &&
                        !input.parsed.nodes.empty() && input.parsed.issues.empty();
  auto customized = [](const Node& node) {
    return node.alias.has_value() || !node.labels.empty() || !node.enabled;
  };
  for (const auto& [connection, parsed_indices] : parsed_by_connection) {
    if (connection.empty()) continue;
    const auto old_found = old_by_connection.find(connection);
    const std::vector<std::size_t> empty_old;
    const auto& old_indices = old_found == old_by_connection.end() ? empty_old : old_found->second;
    auto selected_old = old_indices.end();
    if (!old_indices.empty()) {
      selected_old = std::min_element(old_indices.begin(), old_indices.end(),
          [&](std::size_t left, std::size_t right) {
            const auto& a = *old_nodes[left];
            const auto& b = *old_nodes[right];
            if (customized(a) != customized(b)) return customized(a);
            if (a.source_missing != b.source_missing) return !a.source_missing;
            return a.id < b.id;
          });
      retained_old.insert(*selected_old);
    }
    // Preserve the prior provider name when it is still one of the equivalent
    // rows; otherwise pick a deterministic name, independent of payload order.
    auto selected_parsed = *std::min_element(parsed_indices.begin(), parsed_indices.end(),
        [&](std::size_t left, std::size_t right) {
          const auto& a = input.parsed.nodes[left];
          const auto& b = input.parsed.nodes[right];
          return std::tie(a.name, a.upstream_id) < std::tie(b.name, b.upstream_id);
        });
    if (selected_old != old_indices.end()) {
      const auto& old = *old_nodes[*selected_old];
      const auto name_match = std::find_if(parsed_indices.begin(), parsed_indices.end(),
          [&](std::size_t i) { return input.parsed.nodes[i].name == old.original_name; });
      if (name_match != parsed_indices.end()) selected_parsed = *name_match;
    }
    const auto& parsed = input.parsed.nodes[selected_parsed];
    if (selected_old != old_indices.end()) {
      const auto& old = *old_nodes[*selected_old];
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
    } else {
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
    if (complete) for (const auto index : old_indices) {
      if (retained_old.contains(index)) continue;
      const auto& duplicate = *old_nodes[index];
      const auto& keeper = *old_nodes[*selected_old];
      // Conflicting user attributes need an explicit application-level choice.
      if (customized(duplicate) &&
          (duplicate.alias != keeper.alias || duplicate.labels != keeper.labels ||
           duplicate.enabled != keeper.enabled)) {
        retained_old.insert(index);
        if (std::none_of(result.report.notices.begin(), result.report.notices.end(),
                         [](const Notice& notice) { return notice.code == "duplicate_conflict"; })) {
          result.report.notices.push_back({"duplicate_conflict",
              "同来源存在连接等价但用户属性冲突的旧记录；未自动删除，请预览后处理"});
        }
        continue;
      }
      result.changes.erase_ids.push_back(duplicate.id);
      ++result.report.missing;
      result.report.changed = true;
    }
  }

  // A list with any parse issue cannot prove that absent old nodes disappeared.
  if (complete) {
    for (std::size_t i = 0; i < old_nodes.size(); ++i) {
      if (retained_old.contains(i)) continue;
      const auto& old = *old_nodes[i];
      if (parsed_by_connection.contains(connection_identity(old.settings))) continue;
      result.changes.erase_ids.push_back(old.id);
      ++result.report.missing;
      result.report.changed = true;
    }
  }
  return result;
}

}  // namespace xrm
