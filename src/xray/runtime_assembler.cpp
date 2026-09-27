// XRM: Builds an in-memory FileSet from saved native modules and bindings.
// No I/O or process control occurs here. One module owns each native root key;
// outbounds may add a separate generated node tail under the same key.

#include "xray/runtime_assembler.hpp"

#include "model/module_catalog.hpp"
#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace xrm {
namespace {

AppError failure(ErrorCode code, std::string message,
                 std::optional<std::string> subject = {}) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

const ModuleSetting *setting_for(const ProfileSettings &profile,
                                 std::string_view key) {
  const auto found =
      std::find_if(profile.modules.begin(), profile.modules.end(),
                   [&](const auto &setting) { return setting.key == key; });
  return found == profile.modules.end() ? nullptr : &*found;
}

const ModuleDocument *document_for(const ProfileSnapshot &settings,
                                   std::string_view key) {
  const auto found =
      std::find_if(settings.modules.begin(), settings.modules.end(),
                   [&](const auto &document) { return document.key == key; });
  return found == settings.modules.end() ? nullptr : &*found;
}

bool included(const ProfileSnapshot &settings, std::string_view key) {
  const auto *setting = setting_for(settings.profile, key);
  return setting && setting->included;
}

std::string filename_for(const ProfileSettings &profile,
                         const ModuleDescriptor &descriptor) {
  if (const auto *setting = setting_for(profile, descriptor.key);
      setting && !setting->filename.empty())
    return setting->filename.string();
  return descriptor.default_filename.string();
}

Result<Json> parse_document(const ModuleDocument &document) {
  try {
    return Json::parse(document.source_text);
  } catch (const Json::exception &exception) {
    return std::unexpected(
        failure(ErrorCode::invalid_argument,
                "invalid native module JSON: " + std::string(exception.what()),
                document.key));
  }
}

Result<Json> wrapper_value(const ModuleDocument &document,
                           std::string_view key) {
  auto parsed = parse_document(document);
  if (!parsed)
    return std::unexpected(parsed.error());
  if (!parsed->is_object() || !parsed->contains(std::string(key)))
    return std::unexpected(failure(
        ErrorCode::invalid_argument,
        "native module must contain its Xray root key", std::string(key)));
  return *parsed;
}

const Node *find_node(const BuildInput &input, std::string_view id) {
  const auto found =
      std::find_if(input.nodes.begin(), input.nodes.end(),
                   [&](const auto &node) { return node.id == id; });
  return found == input.nodes.end() ? nullptr : &*found;
}

const GroupBinding *find_group(const ManagerBindings &bindings,
                               std::string_view id) {
  const auto found =
      std::find_if(bindings.groups.begin(), bindings.groups.end(),
                   [&](const auto &group) { return group.id == id; });
  return found == bindings.groups.end() ? nullptr : &*found;
}

std::optional<std::string> group_tag(const GroupBinding &group) {
  if (!group.balancer.is_object())
    return std::nullopt;
  const auto found = group.balancer.find("tag");
  if (found == group.balancer.end() || !found->is_string())
    return std::nullopt;
  return found->get<std::string>();
}

const GroupBinding *find_group_by_tag(const ManagerBindings &bindings,
                                      std::string_view tag, bool &ambiguous) {
  const GroupBinding *result = nullptr;
  for (const auto &group : bindings.groups) {
    const auto candidate = group_tag(group);
    if (!candidate || *candidate != tag)
      continue;
    if (result) {
      ambiguous = true;
      return nullptr;
    }
    result = &group;
  }
  return result;
}

std::string protocol_of(const Node &node) {
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
  const auto &value = std::get<RawOutbound>(node.settings).value;
  if (!value.is_object())
    return {};
  const auto protocol = value.find("protocol");
  return protocol != value.end() && protocol->is_string()
             ? protocol->get<std::string>()
             : std::string{};
}

std::string lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool has_any(const std::vector<std::string> &conditions,
             const std::function<bool(const std::string &)> &predicate) {
  return conditions.empty() ||
         std::any_of(conditions.begin(), conditions.end(), predicate);
}

bool matches(const NodeFilter &filter, const Node &node) {
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
  if (!has_any(filter.protocols,
               [&](const auto &item) { return item == protocol_of(node); }))
    return false;
  const auto name = lower(node.original_name);
  if (!has_any(filter.name_contains, [&](const auto &item) {
        return !item.empty() && name.find(lower(item)) != std::string::npos;
      }))
    return false;
  return has_any(filter.labels, [&](const auto &item) {
    return std::find(node.labels.begin(), node.labels.end(), item) !=
           node.labels.end();
  });
}

Result<std::vector<const Node *>> candidates_for(const BuildInput &input,
                                                 const GroupBinding &group) {
  std::vector<const Node *> result;
  if (const auto *explicit_members =
          std::get_if<ExplicitMembers>(&group.members)) {
    std::set<std::string, std::less<>> ids(explicit_members->node_ids.begin(),
                                           explicit_members->node_ids.end());
    for (const auto &id : ids) {
      const auto *node = find_node(input, id);
      if (!node)
        return std::unexpected(failure(
            ErrorCode::not_found, "explicit group member does not exist", id));
      if (node->enabled && !node->source_missing)
        result.push_back(node);
    }
  } else {
    const auto &filter = std::get<NodeFilter>(group.members);
    for (const auto &node : input.nodes)
      if (node.enabled && !node.source_missing && matches(filter, node))
        result.push_back(&node);
  }
  std::sort(
      result.begin(), result.end(),
      [](const Node *left, const Node *right) { return left->id < right->id; });
  return result;
}

Result<const Node *> required_node(const BuildInput &input,
                                   std::string_view id) {
  const auto *node = find_node(input, id);
  if (!node)
    return std::unexpected(failure(ErrorCode::not_found,
                                   "referenced runtime node does not exist",
                                   std::string(id)));
  if (!node->enabled || node->source_missing)
    return std::unexpected(
        failure(ErrorCode::not_found,
                "referenced runtime node is disabled or its source is missing",
                std::string(id)));
  return node;
}

void add_rules_references(const Json *routing, const BuildInput &input,
                          std::set<std::string, std::less<>> &node_ids,
                          std::set<std::string, std::less<>> &group_ids) {
  if (!routing || !routing->is_object())
    return;
  const auto root = routing->find("routing");
  if (root == routing->end() || !root->is_object())
    return;
  const auto rules = root->find("rules");
  if (rules == root->end() || !rules->is_array())
    return;
  for (const auto &rule : *rules) {
    if (!rule.is_object())
      continue;
    const auto outbound = rule.find("outboundTag");
    if (outbound != rule.end() && outbound->is_string()) {
      for (const auto &node : input.nodes)
        if (outbound->get<std::string>() == node_tag(node.id))
          node_ids.insert(node.id);
    }
    const auto balancer = rule.find("balancerTag");
    if (balancer != rule.end() && balancer->is_string()) {
      bool ambiguous = false;
      if (const auto *group = find_group_by_tag(
              input.settings.bindings, balancer->get<std::string>(), ambiguous))
        group_ids.insert(group->id);
      else if (ambiguous)
        group_ids.insert("$ambiguous:" + balancer->get<std::string>());
    }
  }
}

void add_fallback_reference(const BuildInput &input, const GroupBinding &group,
                            std::set<std::string, std::less<>> &node_ids) {
  if (!group.balancer.is_object())
    return;
  const auto fallback = group.balancer.find("fallbackTag");
  if (fallback == group.balancer.end() || !fallback->is_string())
    return;
  for (const auto &node : input.nodes)
    if (fallback->get<std::string>() == node_tag(node.id))
      node_ids.insert(node.id);
}

std::vector<std::string> observation_tags(const BuildInput &input,
                                          const RuntimeIndex &index) {
  std::set<std::string, std::less<>> requested(
      input.settings.bindings.observation.additional_node_ids.begin(),
      input.settings.bindings.observation.additional_node_ids.end());
  if (input.settings.bindings.observation.append_used_group_members)
    for (const auto &group : index.groups)
      requested.insert(group.candidate_ids.begin(), group.candidate_ids.end());
  std::vector<std::string> tags;
  tags.reserve(requested.size());
  for (const auto &id : requested)
    tags.push_back(node_tag(id));
  return tags;
}

bool observation_requested(const BuildInput &input) {
  return input.settings.bindings.observation.append_used_group_members ||
         !input.settings.bindings.observation.additional_node_ids.empty();
}

Result<OutputFile> write_observation_impl(const ModuleDocument &document,
                                          const BuildInput &input,
                                          const RuntimeIndex &index) {
  auto parsed = wrapper_value(document, document.key);
  if (!parsed)
    return std::unexpected(parsed.error());
  if (!observation_requested(input))
    return OutputFile{document.filename, document.source_text};
  const auto tags = observation_tags(input, index);
  if (tags.empty())
    return OutputFile{document.filename, document.source_text};
  auto &root = (*parsed)[document.key];
  if (!root.is_object())
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "observation module must be an object",
                                   document.key));
  if (!root.contains("subjectSelector"))
    root["subjectSelector"] = Json::array();
  if (!root["subjectSelector"].is_array())
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "subjectSelector must be an array",
                                   document.key));
  std::set<std::string, std::less<>> seen;
  for (const auto &subject : root["subjectSelector"]) {
    if (!subject.is_string())
      return std::unexpected(failure(ErrorCode::invalid_argument,
                                     "subjectSelector entries must be strings",
                                     document.key));
    seen.insert(subject.get<std::string>());
  }
  for (const auto &tag : tags)
    if (seen.insert(tag).second)
      root["subjectSelector"].push_back(tag);
  return OutputFile{document.filename, parsed->dump(2) + "\n"};
}

} // namespace

Result<OutputFile> write_observation(const ModuleDocument &document,
                                     const BuildInput &input,
                                     const RuntimeIndex &index) {
  return write_observation_impl(document, input, index);
}

Result<OutputFile> emit_native_module(const ModuleDocument &document) {
  const auto *descriptor = find_module(document.key);
  if (!descriptor)
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "unknown native module", document.key));
  auto parsed = wrapper_value(document, document.key);
  if (!parsed)
    return std::unexpected(parsed.error());
  return OutputFile{descriptor->default_filename, document.source_text};
}

Result<RuntimeIndex> resolve_scope(const BuildInput &input) {
  // Resolve only references reachable from saved selection, routing, groups,
  // and observation bindings. Inventory membership alone never publishes a
  // node into the runtime confdir.
  RuntimeIndex index;
  std::optional<Json> routing;
  if (included(input.settings, "routing")) {
    if (const auto *document = document_for(input.settings, "routing")) {
      auto parsed = wrapper_value(*document, "routing");
      if (!parsed)
        return std::unexpected(parsed.error());
      routing = std::move(*parsed);
    }
  }

  std::set<std::string, std::less<>> wanted_node_ids;
  std::set<std::string, std::less<>> used_group_ids;
  if (input.settings.profile.selected) {
    const auto &target = *input.settings.profile.selected;
    if (target.kind == TargetKind::node)
      wanted_node_ids.insert(target.id_or_tag);
    else if (target.kind == TargetKind::group)
      used_group_ids.insert(target.id_or_tag);
  }
  wanted_node_ids.insert(input.settings.profile.additional_node_ids.begin(),
                         input.settings.profile.additional_node_ids.end());
  wanted_node_ids.insert(
      input.settings.bindings.observation.additional_node_ids.begin(),
      input.settings.bindings.observation.additional_node_ids.end());
  add_rules_references(routing ? &*routing : nullptr, input, wanted_node_ids,
                       used_group_ids);
  for (const auto &id : used_group_ids)
    if (id.starts_with("$ambiguous:"))
      return std::unexpected(
          failure(ErrorCode::conflict,
                  "multiple managed groups use the same balancer tag",
                  id.substr(std::string("$ambiguous:").size())));

  std::vector<const GroupBinding *> used_groups;
  for (const auto &id : used_group_ids) {
    const auto *group = find_group(input.settings.bindings, id);
    if (!group)
      return std::unexpected(failure(
          ErrorCode::not_found, "selected managed group does not exist", id));
    const auto tag = group_tag(*group);
    if (!tag || tag->empty())
      return std::unexpected(failure(
          ErrorCode::invalid_argument,
          "managed group balancer requires a non-empty tag", group->id));
    used_groups.push_back(group);
  }
  std::sort(
      used_groups.begin(), used_groups.end(),
      [](const auto *left, const auto *right) { return left->id < right->id; });

  for (const auto *group : used_groups) {
    auto candidates = candidates_for(input, *group);
    if (!candidates)
      return std::unexpected(candidates.error());
    if (candidates->empty())
      return std::unexpected(failure(
          ErrorCode::not_found,
          "referenced managed group has no available candidates", group->id));
    GroupRuntimeEntry entry;
    entry.id = group->id;
    entry.tag = *group_tag(*group);
    for (const auto *node : *candidates) {
      entry.candidate_ids.push_back(node->id);
      wanted_node_ids.insert(node->id);
    }
    if (group->selection_mode == SelectionMode::manual) {
      if (!group->selected_node_id)
        return std::unexpected(
            failure(ErrorCode::invalid_argument,
                    "manual group requires a saved selected node", group->id));
      const auto selected = std::find_if(
          candidates->begin(), candidates->end(), [&](const Node *node) {
            return node->id == *group->selected_node_id;
          });
      if (selected == candidates->end())
        return std::unexpected(
            failure(ErrorCode::not_found,
                    "manual group selection is not an available candidate",
                    group->selected_node_id));
      entry.selected_node_id = *group->selected_node_id;
    }
    add_fallback_reference(input, *group, wanted_node_ids);
    index.groups.push_back(std::move(entry));
  }

  std::vector<const Node *> nodes;
  for (const auto &id : wanted_node_ids) {
    auto node = required_node(input, id);
    if (!node)
      return std::unexpected(node.error());
    nodes.push_back(*node);
  }
  std::sort(
      nodes.begin(), nodes.end(),
      [](const Node *left, const Node *right) { return left->id < right->id; });
  for (const Node *node : nodes)
    index.nodes.push_back(
        NodeRuntimeEntry{node->id, node_tag(node->id),
                         node->alias.value_or(node->original_name)});

  if (included(input.settings, "outbounds")) {
    if (const auto *outbounds = document_for(input.settings, "outbounds")) {
      auto parsed = wrapper_value(*outbounds, "outbounds");
      if (!parsed)
        return std::unexpected(parsed.error());
      const auto &list = parsed->at("outbounds");
      if (!list.is_array())
        return std::unexpected(
            failure(ErrorCode::invalid_argument, "outbounds must be an array"));
      for (const auto &outbound : list) {
        if (!outbound.is_object())
          continue;
        const auto tag = outbound.find("tag");
        if (tag != outbound.end() && tag->is_string())
          index.static_outbound_tags.push_back(tag->get<std::string>());
      }
    }
  }
  if (routing) {
    const auto &root = routing->at("routing");
    if (root.is_object()) {
      const auto balancers = root.find("balancers");
      if (balancers != root.end() && balancers->is_array())
        for (const auto &balancer : *balancers) {
          if (!balancer.is_object())
            continue;
          const auto tag = balancer.find("tag");
          if (tag != balancer.end() && tag->is_string())
            index.static_balancer_tags.push_back(tag->get<std::string>());
        }
    }
  }
  return index;
}

Result<std::vector<OutputFile>> write_outbounds(const BuildInput &input,
                                                const RuntimeIndex &index) {
  // Keep the user's native outbounds Source intact. Generated node outbounds
  // occupy a separate tail file under the same top-level Xray key.
  std::vector<OutputFile> files;
  const auto *document = document_for(input.settings, "outbounds");
  if (document) {
    auto parsed = wrapper_value(*document, "outbounds");
    if (!parsed)
      return std::unexpected(parsed.error());
    const auto &outbounds = parsed->at("outbounds");
    if (!outbounds.is_array())
      return std::unexpected(
          failure(ErrorCode::invalid_argument, "outbounds must be an array"));
    files.push_back(OutputFile{"03_outbounds.json", document->source_text});
  }
  if (index.nodes.empty())
    return files;

  Json tail = Json::array();
  for (const auto &entry : index.nodes) {
    const auto *node = find_node(input, entry.id);
    if (!node)
      return std::unexpected(
          failure(ErrorCode::not_found, "runtime node disappeared", entry.id));
    auto outbound = write_outbound(*node, entry.tag);
    if (!outbound)
      return std::unexpected(outbound.error());
    for (const auto &tag : index.static_outbound_tags)
      if (tag == entry.tag)
        return std::unexpected(
            failure(ErrorCode::conflict,
                    "managed node tag conflicts with a static outbound", tag));
    tail.push_back(std::move(*outbound));
  }
  files.push_back(
      OutputFile{"90_nodes_tail.json",
                 Json{{"outbounds", std::move(tail)}}.dump(2) + "\n"});
  return files;
}

Result<OutputFile> write_routing(const ModuleDocument &document,
                                 const BuildInput &input,
                                 const RuntimeIndex &index) {
  auto parsed = wrapper_value(document, "routing");
  if (!parsed)
    return std::unexpected(parsed.error());
  const bool append = input.settings.profile.append_selected_default;
  const bool modified = append || !index.groups.empty();
  if (!modified)
    return OutputFile{"04_routing.json", document.source_text};
  auto &root = (*parsed)["routing"];
  if (!root.is_object())
    return std::unexpected(
        failure(ErrorCode::invalid_argument, "routing must be an object"));
  if (!root.contains("rules"))
    root["rules"] = Json::array();
  if (!root["rules"].is_array())
    return std::unexpected(
        failure(ErrorCode::invalid_argument, "routing.rules must be an array"));
  if (!root.contains("balancers"))
    root["balancers"] = Json::array();
  if (!root["balancers"].is_array())
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "routing.balancers must be an array"));

  std::set<std::string, std::less<>> used_tags(
      index.static_balancer_tags.begin(), index.static_balancer_tags.end());
  for (const auto &entry : index.groups) {
    if (!used_tags.insert(entry.tag).second)
      return std::unexpected(failure(
          ErrorCode::conflict,
          "managed group tag conflicts with a native or managed balancer",
          entry.tag));
    const auto *group = find_group(input.settings.bindings, entry.id);
    if (!group)
      return std::unexpected(
          failure(ErrorCode::not_found, "managed group disappeared", entry.id));
    Json balancer = group->balancer;
    Json selector = Json::array();
    const auto &ids = group->selection_mode == SelectionMode::manual
                          ? std::vector<Id>{*entry.selected_node_id}
                          : entry.candidate_ids;
    for (const auto &id : ids)
      selector.push_back(node_tag(id));
    balancer["tag"] = entry.tag;
    balancer["selector"] = std::move(selector);
    root["balancers"].push_back(std::move(balancer));
  }

  if (append) {
    if (!input.settings.profile.selected)
      return std::unexpected(failure(
          ErrorCode::invalid_argument,
          "a selected target is required to append the routing default"));
    const auto &target = *input.settings.profile.selected;
    Json rule{{"type", "field"}, {"network", "tcp,udp"}};
    if (target.kind == TargetKind::node) {
      if (std::none_of(
              index.nodes.begin(), index.nodes.end(),
              [&](const auto &node) { return node.id == target.id_or_tag; }))
        return std::unexpected(
            failure(ErrorCode::not_found,
                    "selected node is outside the runtime scope"));
      const auto tag = node_tag(target.id_or_tag);
      if (std::find(index.static_outbound_tags.begin(),
                    index.static_outbound_tags.end(),
                    tag) != index.static_outbound_tags.end())
        return std::unexpected(
            failure(ErrorCode::conflict,
                    "managed node tag conflicts with a static outbound", tag));
      rule["outboundTag"] = tag;
    } else if (target.kind == TargetKind::group) {
      const auto found = std::find_if(
          index.groups.begin(), index.groups.end(),
          [&](const auto &group) { return group.id == target.id_or_tag; });
      if (found == index.groups.end())
        return std::unexpected(failure(
            ErrorCode::not_found, "selected group is outside the runtime scope",
            target.id_or_tag));
      rule["balancerTag"] = found->tag;
    } else if (target.kind == TargetKind::native_outbound) {
      rule["outboundTag"] = target.id_or_tag;
    } else {
      rule["balancerTag"] = target.id_or_tag;
    }
    root["rules"].push_back(std::move(rule));
  }
  return OutputFile{"04_routing.json", parsed->dump(2) + "\n"};
}

Result<PreparedFiles> assemble(const BuildInput &input) {
  // Resolve the reachable node/binding scope before emitting any native file.
  // The writer later materializes this complete set for an explicit action.
  auto index = resolve_scope(input);
  if (!index)
    return std::unexpected(index.error());
  const bool needs_outbounds = !index->nodes.empty();
  const bool needs_routing =
      input.settings.profile.append_selected_default || !index->groups.empty();
  if (needs_outbounds && !included(input.settings, "outbounds"))
    return std::unexpected(failure(
        ErrorCode::invalid_argument,
        "outbounds module is disabled but the profile references nodes"));
  if (needs_routing && !included(input.settings, "routing"))
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "routing module is disabled but the profile "
                                   "references a group or default route"));

  const bool use_observatory = included(input.settings, "observatory");
  const bool use_burst = included(input.settings, "burstObservatory");
  if (use_observatory && use_burst)
    return std::unexpected(
        failure(ErrorCode::conflict,
                "observatory and burstObservatory cannot both be enabled"));

  PreparedFiles prepared;
  prepared.index = *index;
  for (const auto &setting : input.settings.profile.modules) {
    if (!setting.included)
      continue;
    const auto *descriptor = find_module(setting.key);
    if (!descriptor)
      return std::unexpected(failure(ErrorCode::invalid_argument,
                                     "unknown native module setting",
                                     setting.key));
    const auto *document = document_for(input.settings, setting.key);
    if (!document)
      return std::unexpected(failure(ErrorCode::not_found,
                                     "enabled native module source is missing",
                                     setting.key));
    if (document->filename != setting.filename && !setting.filename.empty())
      return std::unexpected(
          failure(ErrorCode::conflict,
                  "module source filename does not match Profile setting",
                  setting.key));
    if (setting.key == "outbounds" || setting.key == "routing")
      continue;
    if (setting.key == "observatory" || setting.key == "burstObservatory") {
      auto output = write_observation(*document, input, prepared.index);
      if (!output)
        return std::unexpected(output.error());
      prepared.files.push_back(std::move(*output));
      continue;
    }
    prepared.files.push_back(
        OutputFile{filename_for(input.settings.profile, *descriptor),
                   document->source_text});
  }

  if (included(input.settings, "outbounds")) {
    const auto *document = document_for(input.settings, "outbounds");
    if (!document)
      return std::unexpected(failure(ErrorCode::not_found,
                                     "enabled native module source is missing",
                                     "outbounds"));
    auto files = write_outbounds(input, prepared.index);
    if (!files)
      return std::unexpected(files.error());
    prepared.files.insert(prepared.files.end(), files->begin(), files->end());
  }
  if (included(input.settings, "routing")) {
    const auto *document = document_for(input.settings, "routing");
    if (!document)
      return std::unexpected(failure(ErrorCode::not_found,
                                     "enabled native module source is missing",
                                     "routing"));
    auto routing_file = write_routing(*document, input, prepared.index);
    if (!routing_file)
      return std::unexpected(routing_file.error());
    prepared.files.push_back(std::move(*routing_file));
  }
  std::sort(prepared.files.begin(), prepared.files.end(),
            [](const auto &left, const auto &right) {
              return left.relative_name < right.relative_name;
            });
  for (std::size_t i = 1; i < prepared.files.size(); ++i)
    if (prepared.files[i - 1].relative_name == prepared.files[i].relative_name)
      return std::unexpected(
          failure(ErrorCode::conflict,
                  "multiple modules target the same runtime filename",
                  prepared.files[i].relative_name.string()));
  return prepared;
}

} // namespace xrm
