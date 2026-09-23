#pragma once

#include "model/node.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xrm {

struct ModuleDescriptor {
  std::string key;
  std::string label;
  std::filesystem::path default_filename;
  bool dedicated_form_available = false;
};

struct ModuleDocument {
  std::string key;
  std::filesystem::path filename;
  std::string source_text;
};

struct ModuleSetting {
  std::string key;
  std::filesystem::path filename;
  bool included = false;
};

enum class TargetKind { node, group, native_outbound, native_balancer };
struct Target {
  TargetKind kind = TargetKind::node;
  std::string id_or_tag;
};

struct RunEnvironment {
  std::filesystem::path cwd;
  std::map<std::string, std::string> process_environment;
  std::optional<std::filesystem::path> asset_dir;
};

struct ProfileSettings {
  Id id;
  std::string name;
  Id core_id;
  std::optional<Target> selected;
  bool append_selected_default = false;
  RunEnvironment environment;
  std::vector<ModuleSetting> modules;
  std::vector<Id> additional_node_ids;
};

struct ModuleSnapshot {
  ProfileSettings profile;
  std::vector<ModuleDocument> modules;
};
using ProfileSnapshot = ModuleSnapshot;

struct EditBase { std::optional<std::string> expected_text; };
struct ModuleEdit {
  std::string key;
  EditBase base;
  std::string text;
};
struct ModuleView {
  ModuleDescriptor descriptor;
  ModuleSetting setting;
  std::optional<ModuleDocument> document;
  std::string editor_support = "native";
};

std::span<const ModuleDescriptor> module_catalog();
const ModuleDescriptor* find_module(std::string_view key);
std::string node_tag(std::string_view node_id);

void to_json(Json& json, const ProfileSettings& value);
void from_json(const Json& json, ProfileSettings& value);

}  // namespace xrm
