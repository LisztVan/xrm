// XRM: Manages dynamic node groups and balancer bindings.

#include "profile/profile_service.hpp"

#include "infra/files.hpp"
#include "infra/store.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace xrm {
namespace {

constexpr std::size_t max_bindings_bytes = 8 * 1024 * 1024;

AppError error(ErrorCode code, std::string message,
               std::optional<std::string> subject = {}) {
  return {code, std::move(message), std::move(subject), std::nullopt};
}

template <class T>
Result<T> fail(ErrorCode code, std::string message,
               std::optional<std::string> subject = {}) {
  return std::unexpected(error(code, std::move(message), std::move(subject)));
}

Result<std::filesystem::path> profile_directory(Store &store, const Id &id) {
  return store.profile_path(id);
}

Result<std::string> read_bindings(const std::filesystem::path &path,
                                  std::string default_text) {
  std::error_code fs_error;
  const bool exists = std::filesystem::exists(path, fs_error);
  if (fs_error)
    return fail<std::string>(ErrorCode::io_error,
                             "检查绑定文件失败：" + fs_error.message(),
                             path.string());
  if (!exists)
    return default_text;
  return read_text(path, max_bindings_bytes);
}

std::string folded(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string node_protocol(const Node &node) {
  if (const auto *stream = std::get_if<StreamNode>(&node.settings)) {
    return std::visit(
        [](const auto &protocol) -> std::string {
          using T = std::decay_t<decltype(protocol)>;
          if constexpr (std::is_same_v<T, VlessSettings>)
            return "vless";
          if constexpr (std::is_same_v<T, VmessSettings>)
            return "vmess";
          if constexpr (std::is_same_v<T, TrojanSettings>)
            return "trojan";
          if constexpr (std::is_same_v<T, ShadowsocksSettings>)
            return "shadowsocks";
          return "hysteria2";
        },
        stream->protocol);
  }
  const auto &raw = std::get<RawOutbound>(node.settings).value;
  if (raw.is_object() && raw.contains("protocol") &&
      raw["protocol"].is_string())
    return raw["protocol"].get<std::string>();
  return {};
}

bool has_any_equal(const std::vector<std::string> &choices,
                   std::string_view value) {
  if (choices.empty())
    return true;
  const auto compared = folded(std::string(value));
  return std::any_of(
      choices.begin(), choices.end(),
      [&](const std::string &choice) { return folded(choice) == compared; });
}

bool matches(const NodeSelector &selector, const Node &node) {
  if (const auto *explicit_members = std::get_if<ExplicitMembers>(&selector)) {
    return std::find(explicit_members->node_ids.begin(),
                     explicit_members->node_ids.end(),
                     node.id) != explicit_members->node_ids.end();
  }
  const auto &filter = std::get<NodeFilter>(selector);
  if (std::find(filter.explicit_node_ids.begin(),
                filter.explicit_node_ids.end(),
                node.id) != filter.explicit_node_ids.end())
    return true;
  if ((!filter.sources.empty() || filter.include_local ||
       !filter.explicit_node_ids.empty()) &&
      (node.source_id ? std::find(filter.sources.begin(), filter.sources.end(),
                                  *node.source_id) == filter.sources.end()
                      : !filter.include_local))
    return false;
  if (!has_any_equal(filter.protocols, node_protocol(node)))
    return false;
  if (!filter.name_contains.empty()) {
    const auto name = folded(node.original_name);
    if (std::none_of(filter.name_contains.begin(), filter.name_contains.end(),
                     [&](const std::string &text) {
                       return !text.empty() &&
                              name.find(folded(text)) != std::string::npos;
                     }))
      return false;
  }
  if (!filter.labels.empty() &&
      std::none_of(filter.labels.begin(), filter.labels.end(),
                   [&](const std::string &label) {
                     return std::find(node.labels.begin(), node.labels.end(),
                                      label) != node.labels.end();
                   }))
    return false;
  return true;
}

Result<void> validate_group_set(const std::vector<GroupBinding> &groups,
                                const std::vector<Node> &nodes) {
  std::set<Id> ids;
  std::set<std::string> tags;
  for (const auto &group : groups) {
    if (group.id.empty() || group.name.empty())
      return fail<void>(ErrorCode::invalid_argument, "组 ID 与名称不能为空",
                        group.id);
    if (!ids.insert(group.id).second)
      return fail<void>(ErrorCode::conflict, "组 ID 重复", group.id);
    if (!group.balancer.is_object() || !group.balancer.contains("tag") ||
        !group.balancer["tag"].is_string() ||
        group.balancer["tag"].get<std::string>().empty())
      return fail<void>(ErrorCode::invalid_argument,
                        "动态组必须保存非空原生 balancer tag", group.id);
    const auto tag = group.balancer["tag"].get<std::string>();
    if (!tags.insert(tag).second)
      return fail<void>(ErrorCode::conflict, "动态组 balancer tag 重复", tag);
    if (group.balancer.contains("selector"))
      return fail<void>(ErrorCode::invalid_argument,
                        "balancer.selector 由组装器生成，不能保存在组绑定中",
                        group.id);
    if (group.selected_node_id) {
      if (group.selection_mode != SelectionMode::manual)
        return fail<void>(ErrorCode::invalid_argument,
                          "只有手选组可以保存 selected_node_id", group.id);
      const auto found =
          std::find_if(nodes.begin(), nodes.end(), [&](const Node &node) {
            return node.id == *group.selected_node_id && node.enabled &&
                   !node.source_missing && matches(group.members, node);
          });
      if (found == nodes.end())
        return fail<void>(ErrorCode::invalid_argument,
                          "手选节点必须是组内可用候选节点",
                          group.selected_node_id);
    }
  }
  return {};
}

std::string serialize_groups(const std::vector<GroupBinding> &groups) {
  return Json(groups).dump(2) + "\n";
}

} // namespace

Result<std::vector<GroupBinding>> ProfileService::groups(const Id &id) const {
  auto directory = profile_directory(store_, id);
  if (!directory)
    return std::unexpected(directory.error());
  auto text = read_bindings(*directory / "bindings" / "groups.json", "[]\n");
  if (!text)
    return std::unexpected(text.error());
  try {
    auto result = Json::parse(*text).get<std::vector<GroupBinding>>();
    std::set<Id> ids;
    for (const auto &group : result) {
      if (group.id.empty() || !ids.insert(group.id).second)
        return fail<std::vector<GroupBinding>>(
            ErrorCode::invalid_argument, "组绑定包含空或重复 ID", group.id);
    }
    return result;
  } catch (const std::exception &exception) {
    return fail<std::vector<GroupBinding>>(ErrorCode::invalid_argument,
                                           "读取组绑定 JSON 失败：" +
                                               std::string(exception.what()),
                                           "groups.json");
  }
}

Result<void> ProfileService::save_groups(const Id &id,
                                         const BindingsEdit &edit) {
  auto directory = profile_directory(store_, id);
  if (!directory)
    return std::unexpected(directory.error());
  auto nodes = store_.all_nodes();
  if (!nodes)
    return std::unexpected(nodes.error());
  if (auto valid = validate_group_set(edit.value, *nodes); !valid)
    return valid;
  const auto path = *directory / "bindings" / "groups.json";
  return write_if_unchanged(path, edit.base, serialize_groups(edit.value));
}

Result<ObservationBinding> ProfileService::observation(const Id &id) const {
  auto directory = profile_directory(store_, id);
  if (!directory)
    return std::unexpected(directory.error());
  auto text =
      read_bindings(*directory / "bindings" / "observation.json", "{}\n");
  if (!text)
    return std::unexpected(text.error());
  try {
    return Json::parse(*text).get<ObservationBinding>();
  } catch (const std::exception &exception) {
    return fail<ObservationBinding>(ErrorCode::invalid_argument,
                                    "读取观测绑定 JSON 失败：" +
                                        std::string(exception.what()),
                                    "observation.json");
  }
}

Result<void>
ProfileService::save_observation(const Id &id, const EditBase &base,
                                 const ObservationBinding &binding) {
  auto directory = profile_directory(store_, id);
  if (!directory)
    return std::unexpected(directory.error());
  const auto path = *directory / "bindings" / "observation.json";
  return write_if_unchanged(path, base, Json(binding).dump(2) + "\n");
}

Result<void> ProfileService::select_group_node(const Id &id, const Id &group_id,
                                               const Id &node_id) {
  auto groups_result = groups(id);
  if (!groups_result)
    return std::unexpected(groups_result.error());
  auto group = std::find_if(
      groups_result->begin(), groups_result->end(),
      [&](const GroupBinding &value) { return value.id == group_id; });
  if (group == groups_result->end())
    return fail<void>(ErrorCode::not_found, "找不到策略组", group_id);
  if (group->selection_mode != SelectionMode::manual)
    return fail<void>(ErrorCode::invalid_argument, "只有手选组允许手动选择节点",
                      group_id);
  auto nodes = store_.all_nodes();
  if (!nodes)
    return std::unexpected(nodes.error());
  const auto candidate =
      std::find_if(nodes->begin(), nodes->end(), [&](const Node &node) {
        return node.id == node_id && node.enabled && !node.source_missing &&
               matches(group->members, node);
      });
  if (candidate == nodes->end())
    return fail<void>(ErrorCode::invalid_argument,
                      "选择的节点不是该组的可用候选节点", node_id);

  auto directory = profile_directory(store_, id);
  if (!directory)
    return std::unexpected(directory.error());
  const auto path = *directory / "bindings" / "groups.json";
  auto current = read_bindings(path, "[]\n");
  if (!current)
    return std::unexpected(current.error());
  group->selected_node_id = node_id;
  return write_if_unchanged(path, EditBase{*current},
                            serialize_groups(*groups_result));
}

Result<void>
ProfileService::choose_observer(const Id &id,
                                std::optional<std::string> native_key) {
  if (native_key && *native_key != "observatory" &&
      *native_key != "burstObservatory")
    return fail<void>(ErrorCode::invalid_argument,
                      "观测类型必须是 observatory、burstObservatory 或关闭",
                      native_key);
  auto current = get(id);
  if (!current)
    return std::unexpected(current.error());
  auto directory = profile_directory(store_, id);
  if (!directory)
    return std::unexpected(directory.error());
  const auto path = *directory / "profile.json";
  auto text = read_text(path, max_bindings_bytes);
  if (!text)
    return std::unexpected(text.error());
  bool found_background = false;
  bool found_burst = false;
  for (auto &module : current->modules) {
    if (module.key == "observatory") {
      module.included = native_key == module.key;
      found_background = true;
    } else if (module.key == "burstObservatory") {
      module.included = native_key == module.key;
      found_burst = true;
    }
  }
  if (!found_background || !found_burst)
    return fail<void>(ErrorCode::invalid_argument, "Profile 缺少观测模块设置",
                      id);
  return write_if_unchanged(path, EditBase{*text},
                            Json(*current).dump(2) + "\n");
}

} // namespace xrm
