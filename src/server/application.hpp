#pragma once

#include "contract/dtos.hpp"
#include "infra/http.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
#include "profile/profile_service.hpp"
#include "profile/forms.hpp"
#include "server/log_buffer.hpp"
#include "server/operations.hpp"
#include "subscription/subscription_service.hpp"
#include "xray/control.hpp"
#include "xray/core_manager.hpp"
#include "xray/installer.hpp"

#include <set>

namespace xrm {

class Application {
 public:
  Application(Store& store, ProcessService& processes, HttpClient& http, uv_loop_t& loop,
              std::filesystem::path data_root, std::string application_version,
              Id boot_id, SubscriptionService& subscriptions);

  Json server_info() const;
  Result<NodeSummary> add_manual_node(std::string name, Json outbound);
  Result<Page<NodeSummary>> list_nodes(const NodeQuery& query, PageRequest page) const;
  Result<Node> get_node(const Id& id) const;
  Result<NodeSummary> copy_node(const Id& id, std::string new_name);
  Result<NodeSummary> set_node_enabled(const Id& id, bool enabled);
  Result<NodeSummary> set_node_alias(const Id& id, std::optional<std::string> alias);
  Result<NodeSummary> set_node_labels(const Id& id, std::vector<std::string> labels);
  Store& store();
  SubscriptionService& subscriptions();
  Result<std::vector<CoreInstall>> list_cores() const;
  Result<Id> core_releases(bool include_prerelease,
                           Completion<std::vector<CoreRelease>> completion);
  Result<Id> install_core(InstallRequest request,
                          Completion<CoreInstall> completion);
  void use_core(const Id& profile_id, const Id& core_id,
                Completion<ActivationResult> completion);
  void register_core(std::filesystem::path path, std::string name,
                     Completion<CoreInstall> completion);

  ProfileService& profiles();
  Operations& operations();
  CoreManager& core_manager();
  LogBuffer& logs();
  Result<BuildInput> build_input(const Id& profile_id);
  Result<void> select(const Id& profile_id, std::optional<Target> target);
  void connect(const Id& profile_id, std::optional<Target> target,
               Completion<ActivationResult> completion);
  void check(const Id& profile_id, Completion<ActivationResult> completion);
  void dump(const Id& profile_id, Completion<Json> completion);
  Result<PreparedDirectory> render(const Id& profile_id);
  Result<void> export_config(const Id& profile_id, std::string_view source,
                             const std::filesystem::path& destination);
  Result<Json> runtime_status() const;
  Json runtime_metrics(const Id& connection_id, bool watch);
  void release_metrics_viewer(const Id& connection_id);
  Result<ModuleChangePlan> plan_module_changes(const Id&, std::string_view action,
                                                const Json& arguments) const;
  SaveManyResult save_module_changes(const Id&, const ModuleChangePlan&);
  Result<ControlTarget> control_target() const;
  Result<Id> override_runtime_group(std::string balancer, std::string outbound,
                                    Completion<OverrideResult> completion);
  Result<Id> clear_runtime_group_override(std::string balancer,
                                          Completion<OverrideResult> completion);
  Result<Json> show_config(const Id& profile_id, std::string_view source,
                           std::optional<std::string> module) const;
  void recover(Completion<ActivationResult> completion);
  void shutdown(std::function<void()> drained);

 private:
  Store* store_;
  SubscriptionService* subscriptions_;
  ProcessService* processes_;
  HttpClient* http_;
  std::string application_version_;
  Id boot_id_;
  std::filesystem::path data_root_;
  std::unique_ptr<ProfileService> profiles_;
  std::unique_ptr<RuntimeWriter> runtime_writer_;
  std::unique_ptr<CoreManager> core_manager_;
  std::unique_ptr<XrayControl> xray_control_;
  std::unique_ptr<MetricsClient> metrics_client_;
  std::unique_ptr<CoreInstaller> core_installer_;
  LogBuffer logs_;
  Operations operations_;
  std::optional<Id> override_run_id_;
  std::optional<std::string> runtime_override_;
  OverrideState override_state_ = OverrideState::none;
  std::set<Id> metrics_viewers_;
  std::optional<Id> metrics_run_id_;
};

}  // namespace xrm
