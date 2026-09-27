#pragma once

#include "model/profile.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace xrm {

class Store;

struct ProfileCreate {
  std::string name;
  Id core_id;
  std::string preset;
  RunEnvironment environment;
};
struct ProfileSummary { Id id; std::string name; Id core_id; };
struct ProfileEdit { EditBase base; ProfileSettings value; };
struct BindingsEdit { EditBase base; std::vector<GroupBinding> value; };

class ProfileService {
 public:
  ProfileService(Store&, std::filesystem::path profiles_root);
  Result<ProfileSettings> create(const ProfileCreate&);
  Result<std::vector<ProfileSummary>> list() const;
  Result<ProfileSettings> get(const Id&) const;
  Result<std::pair<ProfileSettings, std::string>> get_with_source(const Id&) const;
  Result<void> save(const Id&, const ProfileEdit&);
  Result<void> rename(const Id&, std::string new_name);
  Result<ProfileSettings> clone(const Id&, std::string new_name);
  Result<void> remove(const Id&);
  Result<std::vector<ModuleView>> modules(const Id&) const;
  Result<ModuleView> module(const Id&, std::string_view key) const;
  Result<void> save_module(const Id&, const ModuleEdit&);
  Result<void> include_module(const Id&, std::string_view key, bool included);
  Result<void> rename_module_file(const Id&, std::string_view key,
                                  const std::filesystem::path& relative_name);
  Result<std::vector<GroupBinding>> groups(const Id&) const;
  Result<void> save_groups(const Id&, const BindingsEdit&);
  Result<ObservationBinding> observation(const Id&) const;
  Result<void> save_observation(const Id&, const EditBase&, const ObservationBinding&);
  Result<void> select_group_node(const Id&, const Id& group_id, const Id& node_id);
  Result<void> choose_observer(const Id&, std::optional<std::string> native_key);
  Result<ModuleSnapshot> snapshot(const Id&) const;

 private:
  Store& store_;
  std::filesystem::path profiles_root_;
};

}  // namespace xrm
