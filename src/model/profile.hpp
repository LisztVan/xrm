// XRM: Defines Profiles, module sources, and saved selection state.

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
  // The user's Source representation; Runtime assembly must not write back.
  std::string source_text;
};

struct ModuleSetting {
  std::string key;
  std::filesystem::path filename;
  // Controls runtime inclusion without deleting or rewriting Source.
  bool included = false;
};

struct ExplicitMembers {
  std::vector<Id> node_ids;
};
struct NodeFilter {
  std::vector<Id> sources;
  std::vector<std::string> protocols;
  std::vector<std::string> name_contains;
  std::vector<std::string> labels;
  std::vector<Id> explicit_node_ids; // Union with dynamically matched sources.
  bool include_local =
      false; // Also include manual inventory when sources are specified.
};
using NodeSelector = std::variant<ExplicitMembers, NodeFilter>;
enum class SelectionMode { automatic, manual };
struct GroupBinding {
  Id id;
  std::string name;
  NodeSelector members = ExplicitMembers{};
  SelectionMode selection_mode = SelectionMode::automatic;
  std::optional<Id> selected_node_id;
  Json balancer = Json::object();
};
struct ObservationBinding {
  bool append_used_group_members = false;
  std::vector<Id> additional_node_ids;
};
struct ManagerBindings {
  std::vector<GroupBinding> groups;
  ObservationBinding observation;
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
  // Persisted target for a future connection, independent of TUI Use/Edit.
  std::optional<Target> selected;
  bool append_selected_default = false;
  RunEnvironment environment;
  std::vector<ModuleSetting> modules;
  std::vector<Id> additional_node_ids;
  // Missing in older profiles: all global sources remain available.
  // An explicit empty vector means a new profile has not bound a source yet.
  std::optional<std::vector<Id>> enabled_source_ids;
  // Explicit shared resource references. Runtime snapshots copy validated
  // asset bytes; the Profile never stores a second copy of those resources.
  std::map<std::string, Id> geodata_resource_ids;
};

struct ProfileSnapshot {
  ProfileSettings profile;
  std::vector<ModuleDocument> modules;
  ManagerBindings bindings;
};
using ModuleSnapshot = ProfileSnapshot;

struct EditBase {
  // Compare against the current source before a write; null means no source.
  std::optional<std::string> expected_text;
};
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
const ModuleDescriptor *find_module(std::string_view key);
std::string node_tag(std::string_view node_id);

void to_json(Json &json, const ProfileSettings &value);
void from_json(const Json &json, ProfileSettings &value);
void to_json(Json &json, const ExplicitMembers &value);
void from_json(const Json &json, ExplicitMembers &value);
void to_json(Json &json, const NodeFilter &value);
void from_json(const Json &json, NodeFilter &value);
void to_json(Json &json, const NodeSelector &value);
void from_json(const Json &json, NodeSelector &value);
void to_json(Json &json, const GroupBinding &value);
void from_json(const Json &json, GroupBinding &value);
void to_json(Json &json, const ObservationBinding &value);
void from_json(const Json &json, ObservationBinding &value);
void to_json(Json &json, const ManagerBindings &value);
void from_json(const Json &json, ManagerBindings &value);

} // namespace xrm
