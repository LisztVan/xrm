#include "xray/runtime_assembler.hpp"

#include "model/module_catalog.hpp"
#include "xray/codecs/codec.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace xrm {
namespace {

AppError failure(ErrorCode code, std::string message, std::optional<std::string> subject = {}) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

const ModuleSetting* setting_for(const ProfileSettings& profile, std::string_view key) {
  const auto found = std::find_if(profile.modules.begin(), profile.modules.end(),
                                  [&](const auto& setting) { return setting.key == key; });
  return found == profile.modules.end() ? nullptr : &*found;
}

const ModuleDocument* document_for(const ProfileSnapshot& settings, std::string_view key) {
  const auto found = std::find_if(settings.modules.begin(), settings.modules.end(),
                                  [&](const auto& document) { return document.key == key; });
  return found == settings.modules.end() ? nullptr : &*found;
}

std::string filename_for(const ProfileSettings& profile, const ModuleDescriptor& descriptor) {
  if (const auto* setting = setting_for(profile, descriptor.key); setting && !setting->filename.empty())
    return setting->filename.string();
  return descriptor.default_filename.string();
}

Result<Json> parse_document(const ModuleDocument& document) {
  try {
    return Json::parse(document.source_text);
  } catch (const Json::exception& exception) {
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "invalid native module JSON: " + std::string(exception.what()),
                                   document.key));
  }
}

Result<Json> wrapper_value(const ModuleDocument& document, std::string_view key) {
  auto parsed = parse_document(document);
  if (!parsed) return std::unexpected(parsed.error());
  if (!parsed->is_object() || !parsed->contains(std::string(key)))
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "native module must contain its Xray root key", std::string(key)));
  return *parsed;
}

const Node* find_node(const BuildInput& input, std::string_view id) {
  const auto found = std::find_if(input.nodes.begin(), input.nodes.end(),
                                  [&](const auto& node) { return node.id == id; });
  return found == input.nodes.end() ? nullptr : &*found;
}

Result<std::vector<const Node*>> selected_nodes(const BuildInput& input, const Json* routing) {
  std::set<std::string, std::less<>> wanted;
  if (input.settings.profile.selected) {
    if (input.settings.profile.selected->kind != TargetKind::node)
      return std::unexpected(failure(ErrorCode::unsupported, "M1 runtime supports node targets only"));
    wanted.insert(input.settings.profile.selected->id_or_tag);
  }
  wanted.insert(input.settings.profile.additional_node_ids.begin(),
                input.settings.profile.additional_node_ids.end());

  // Rules may explicitly reference a managed node tag. Include only exact known tags.
  if (routing && routing->is_object()) {
    const auto root = routing->find("routing");
    if (root != routing->end() && root->is_object()) {
      const auto rules = root->find("rules");
      if (rules != root->end() && rules->is_array()) {
        for (const auto& rule : *rules) {
          if (!rule.is_object()) continue;
          const auto tag = rule.find("outboundTag");
          if (tag == rule.end() || !tag->is_string()) continue;
          for (const auto& node : input.nodes)
            if (tag->get<std::string>() == node_tag(node.id)) wanted.insert(node.id);
        }
      }
    }
  }

  std::vector<const Node*> result;
  std::set<std::string, std::less<>> seen_tags;
  for (const auto& node : input.nodes) {
    if (!wanted.contains(node.id)) continue;
    if (!node.enabled || node.source_missing)
      return std::unexpected(failure(ErrorCode::not_found,
                                     "selected runtime node is disabled or its source is missing", node.id));
    if (!seen_tags.insert(node_tag(node.id)).second) continue;
    result.push_back(&node);
  }
  for (const auto& id : wanted)
    if (!find_node(input, id))
      return std::unexpected(failure(ErrorCode::not_found, "referenced runtime node does not exist", id));
  return result;
}

}  // namespace

Result<OutputFile> emit_native_module(const ModuleDocument& document) {
  const auto* descriptor = find_module(document.key);
  if (!descriptor)
    return std::unexpected(failure(ErrorCode::invalid_argument, "unknown native module", document.key));
  auto parsed = wrapper_value(document, document.key);
  if (!parsed) return std::unexpected(parsed.error());
  return OutputFile{descriptor->default_filename, document.source_text};
}

Result<RuntimeIndex> resolve_scope(const BuildInput& input) {
  RuntimeIndex index;
  const auto* routing_document = document_for(input.settings, "routing");
  std::optional<Json> routing;
  if (routing_document) {
    auto parsed = wrapper_value(*routing_document, "routing");
    if (!parsed) return std::unexpected(parsed.error());
    routing = std::move(*parsed);
  }
  auto nodes = selected_nodes(input, routing ? &*routing : nullptr);
  if (!nodes) return std::unexpected(nodes.error());
  for (const Node* node : *nodes)
    index.nodes.push_back(NodeRuntimeEntry{node->id, node_tag(node->id),
                                           node->alias.value_or(node->original_name)});

  if (const auto* outbounds = document_for(input.settings, "outbounds")) {
    auto parsed = wrapper_value(*outbounds, "outbounds");
    if (!parsed) return std::unexpected(parsed.error());
    const auto& list = parsed->at("outbounds");
    if (!list.is_array())
      return std::unexpected(failure(ErrorCode::invalid_argument, "outbounds must be an array"));
    for (const auto& outbound : list) {
      if (!outbound.is_object()) continue;
      const auto tag = outbound.find("tag");
      if (tag != outbound.end() && tag->is_string())
        index.static_outbound_tags.push_back(tag->get<std::string>());
    }
  }
  if (routing) {
    const auto& root = routing->at("routing");
    if (root.is_object()) {
      const auto balancers = root.find("balancers");
      if (balancers != root.end() && balancers->is_array())
        for (const auto& balancer : *balancers) {
          if (!balancer.is_object()) continue;
          const auto tag = balancer.find("tag");
          if (tag != balancer.end() && tag->is_string())
            index.static_balancer_tags.push_back(tag->get<std::string>());
        }
    }
  }
  return index;
}

Result<std::vector<OutputFile>> write_outbounds(const BuildInput& input, const RuntimeIndex& index) {
  std::vector<OutputFile> files;
  const auto* document = document_for(input.settings, "outbounds");
  if (document) {
    auto parsed = wrapper_value(*document, "outbounds");
    if (!parsed) return std::unexpected(parsed.error());
    const auto& outbounds = parsed->at("outbounds");
    if (!outbounds.is_array())
      return std::unexpected(failure(ErrorCode::invalid_argument, "outbounds must be an array"));
    files.push_back(OutputFile{"03_outbounds.json", document->source_text});
  }
  if (index.nodes.empty()) return files;

  Json tail = Json::array();
  for (const auto& entry : index.nodes) {
    const auto* node = find_node(input, entry.id);
    if (!node) return std::unexpected(failure(ErrorCode::not_found, "runtime node disappeared", entry.id));
    auto outbound = write_outbound(*node, entry.tag);
    if (!outbound) return std::unexpected(outbound.error());
    for (const auto& tag : index.static_outbound_tags)
      if (tag == entry.tag)
        return std::unexpected(failure(ErrorCode::conflict, "managed node tag conflicts with a static outbound", tag));
    tail.push_back(std::move(*outbound));
  }
  files.push_back(OutputFile{"90_nodes_tail.json", Json{{"outbounds", std::move(tail)}}.dump(2) + "\n"});
  return files;
}

Result<OutputFile> write_routing(const ModuleDocument& document, const BuildInput& input,
                                 const RuntimeIndex& index) {
  auto parsed = wrapper_value(document, "routing");
  if (!parsed) return std::unexpected(parsed.error());
  const bool append = input.settings.profile.append_selected_default;
  if (append) {
    if (!input.settings.profile.selected || input.settings.profile.selected->kind != TargetKind::node)
      return std::unexpected(failure(ErrorCode::invalid_argument,
                                     "a selected node is required to append the routing default"));
    if (std::find_if(index.nodes.begin(), index.nodes.end(), [&](const auto& node) {
          return node.id == input.settings.profile.selected->id_or_tag;
        }) == index.nodes.end())
      return std::unexpected(failure(ErrorCode::not_found, "selected node is outside the runtime scope"));
    auto& root = (*parsed)["routing"];
    if (!root.is_object())
      return std::unexpected(failure(ErrorCode::invalid_argument, "routing must be an object"));
    if (!root.contains("rules")) root["rules"] = Json::array();
    if (!root["rules"].is_array())
      return std::unexpected(failure(ErrorCode::invalid_argument, "routing.rules must be an array"));
    const auto tag = node_tag(input.settings.profile.selected->id_or_tag);
    if (std::find(index.static_outbound_tags.begin(), index.static_outbound_tags.end(), tag) !=
        index.static_outbound_tags.end())
      return std::unexpected(failure(ErrorCode::conflict, "managed node tag conflicts with a static outbound", tag));
    root["rules"].push_back(Json{{"type", "field"}, {"network", "tcp,udp"}, {"outboundTag", tag}});
  }
  return OutputFile{"04_routing.json", append ? parsed->dump(2) + "\n" : document.source_text};
}

Result<PreparedFiles> assemble(const BuildInput& input) {
  bool needs_outbounds = input.settings.profile.selected.has_value() ||
                         !input.settings.profile.additional_node_ids.empty();
  const bool needs_routing = input.settings.profile.append_selected_default;
  if (needs_outbounds && (!setting_for(input.settings.profile, "outbounds") ||
                          !setting_for(input.settings.profile, "outbounds")->included))
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "outbounds module is disabled but the profile references nodes"));
  if (needs_routing && (!setting_for(input.settings.profile, "routing") ||
                        !setting_for(input.settings.profile, "routing")->included))
    return std::unexpected(failure(ErrorCode::invalid_argument,
                                   "routing module is disabled but a default route is requested"));

  auto index = resolve_scope(input);
  if (!index) return std::unexpected(index.error());
  PreparedFiles prepared;
  prepared.index = *index;

  for (const auto& setting : input.settings.profile.modules) {
    if (!setting.included) continue;
    const auto* descriptor = find_module(setting.key);
    if (!descriptor)
      return std::unexpected(failure(ErrorCode::invalid_argument, "unknown native module setting", setting.key));
    const auto* document = document_for(input.settings, setting.key);
    if (!document)
      return std::unexpected(failure(ErrorCode::not_found, "enabled native module source is missing", setting.key));
    if (setting.key == "outbounds" || setting.key == "routing") continue;
    if (document->filename != setting.filename && !setting.filename.empty())
      return std::unexpected(failure(ErrorCode::conflict, "module source filename does not match Profile setting", setting.key));
    prepared.files.push_back(OutputFile{filename_for(input.settings.profile, *descriptor), document->source_text});
  }

  const auto* outbounds_setting = setting_for(input.settings.profile, "outbounds");
  if (outbounds_setting && outbounds_setting->included) {
    const auto* document = document_for(input.settings, "outbounds");
    if (!document)
      return std::unexpected(failure(ErrorCode::not_found, "enabled native module source is missing", "outbounds"));
    auto files = write_outbounds(input, prepared.index);
    if (!files) return std::unexpected(files.error());
    prepared.files.insert(prepared.files.end(), files->begin(), files->end());
  }
  const auto* routing_setting = setting_for(input.settings.profile, "routing");
  if (routing_setting && routing_setting->included) {
    const auto* document = document_for(input.settings, "routing");
    if (!document)
      return std::unexpected(failure(ErrorCode::not_found, "enabled native module source is missing", "routing"));
    auto routing_file = write_routing(*document, input, prepared.index);
    if (!routing_file) return std::unexpected(routing_file.error());
    prepared.files.push_back(std::move(*routing_file));
  }
  std::sort(prepared.files.begin(), prepared.files.end(), [](const auto& left, const auto& right) {
    return left.relative_name < right.relative_name;
  });
  for (std::size_t i = 1; i < prepared.files.size(); ++i)
    if (prepared.files[i - 1].relative_name == prepared.files[i].relative_name)
      return std::unexpected(failure(ErrorCode::conflict, "multiple modules target the same runtime filename",
                                     prepared.files[i].relative_name.string()));
  return prepared;
}

}  // namespace xrm
