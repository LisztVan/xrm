/// @file
/// @brief Declares the daemon's business coordinator and owned services.
///
/// Follow a request from ApiRouter here, then into Store/ProfileService for
/// saved data or CoreManager for an explicit running-state change.

#pragma once

#include "contract/dtos.hpp"
#include "geodata/cache.hpp"
#include "infra/http.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
#include "profile/forms.hpp"
#include "profile/profile_service.hpp"
#include "server/log_buffer.hpp"
#include "server/node_test_service.hpp"
#include "server/operations.hpp"
#include "subscription/subscription_service.hpp"
#include "xray/control.hpp"
#include "xray/core_manager.hpp"
#include "xray/installer.hpp"

#include <map>
#include <set>

namespace xrm {

/// @brief Coordinates saved domain operations and explicit runtime actions.
///
/// ApiRouter validates wire shapes before calling here. This class collects
/// saved data for CoreManager; it never uses a client-side TUI draft.
class Application {
public:
  Application(Store &store, ProcessService &processes, HttpClient &http,
              uv_loop_t &loop, std::filesystem::path data_root,
              std::string application_version, Id boot_id,
              SubscriptionService &subscriptions);
  ~Application();

  // Inventory reads/writes and isolated node tests; no core activation.
  Json server_info() const;
  Result<NodeSummary> add_manual_node(std::string name, Json outbound);
  Result<NodeSummary> add_manual_typed_node(std::string name, Json settings);
  Result<NodeSummary> add_manual_share_uri(std::string uri,
                                           std::optional<std::string> name);
  Result<NodeSummary> replace_manual_node(const Id &id, std::string name,
                                          Json settings);
  Result<void> remove_manual_node(const Id &id);
  Result<Page<NodeSummary>> list_nodes(const NodeQuery &query,
                                       PageRequest page) const;
  Result<Node> get_node(const Id &id) const;
  Result<NodeSummary> copy_node(const Id &id, std::string new_name);
  Result<NodeSummary> set_node_enabled(const Id &id, bool enabled);
  Result<NodeSummary> set_node_alias(const Id &id,
                                     std::optional<std::string> alias);
  Result<NodeSummary> set_node_labels(const Id &id,
                                      std::vector<std::string> labels);
  Result<NodeSummary> edit_node_form(const Id &id, std::string_view field,
                                     Json value);
  Result<Json> preview_node_history(const Id &source_id) const;
  Result<Json> confirm_node_history(const Id &source_id, const Json &selected);
  Result<Id> start_node_test(const Id &node_id, NodeTestKind kind,
                             std::optional<std::string> target_url,
                             std::optional<Id> profile_id, Milliseconds timeout,
                             std::size_t max_body_bytes);
  Result<void> cancel_node_test(const Id &operation_id);
  Json node_test_results() const;

  // Core inventory and selection. use_core may replace an active Profile's
  // core; install/register alone only make an executable available.
  Store &store();
  SubscriptionService &subscriptions();
  Result<std::vector<CoreInstall>> list_cores() const;
  Result<void> remove_managed_core(const Id &core_id);
  Result<Id> core_releases(bool include_prerelease,
                           Completion<std::vector<CoreRelease>> completion);
  Result<Id> install_core(InstallRequest request,
                          Completion<CoreInstall> completion);
  void use_core(const Id &profile_id, const Id &core_id,
                Completion<ActivationResult> completion);
  void register_core(std::filesystem::path path, std::string name,
                     Completion<CoreInstall> completion);

  ProfileService &profiles();
  Operations &operations();
  CoreManager &core_manager();
  LogBuffer &logs();

  // Saved configuration and explicit runtime actions. build_input excludes
  // any client-side draft; connect/check/render choose distinct effects.
  Result<BuildInput> build_input(const Id &profile_id);
  Result<void> select(const Id &profile_id, std::optional<Target> target);
  Result<void> set_profile_sources(const Id &profile_id,
                                   std::vector<Id> source_ids);
  Result<void> set_profile_geodata(const Id &profile_id,
                                   std::map<std::string, Id> resources);
  Result<std::vector<GeodataResourceRecord>> list_geodata_resources() const;
  Result<GeodataResourceRecord> get_geodata_resource(const Id &) const;
  Result<GeodataResourceRecord> put_geodata_resource(GeodataResourceRecord);
  Result<void> remove_geodata_resource(const Id &);
  Result<Id> update_geodata_resource(const Id &resource_id, const Id &core_id);
  Result<Json> plan_routing_preset(const Id &profile_id,
                                   std::string_view preset) const;
  Result<Json> save_routing_preset(const Id &profile_id,
                                   std::string_view preset,
                                   std::string_view expected_text);
  /// Builds from saved state and starts/replaces the owned core. A supplied
  /// target is saved first; an omitted target uses the Profile's saved choice.
  void connect(const Id &profile_id, std::optional<Target> target,
               Completion<ActivationResult> completion);
  /// Validates the candidate without changing the active process.
  void check(const Id &profile_id, Completion<ActivationResult> completion);
  void dump(const Id &profile_id, Completion<Json> completion);
  /// Materializes a preview confdir from saved state without activation.
  Result<PreparedDirectory> render(const Id &profile_id);
  Result<void> export_config(const Id &profile_id, std::string_view source,
                             const std::filesystem::path &destination);
  Result<Json> runtime_status() const;
  Json runtime_metrics(const Id &connection_id, bool watch);
  void release_metrics_viewer(const Id &connection_id);
  Result<ModuleChangePlan> plan_module_changes(const Id &,
                                               std::string_view action,
                                               const Json &arguments) const;
  SaveManyResult save_module_changes(const Id &, const ModuleChangePlan &);
  Result<ControlTarget> control_target() const;
  Result<Id> override_runtime_group(std::string balancer, std::string outbound,
                                    Completion<OverrideResult> completion);
  Result<Id>
  clear_runtime_group_override(std::string balancer,
                               Completion<OverrideResult> completion);
  Result<Json> show_config(const Id &profile_id, std::string_view source,
                           std::optional<std::string> module) const;
  void recover(Completion<ActivationResult> completion);
  void shutdown(std::function<void()> drained);

private:
  Store *store_;
  SubscriptionService *subscriptions_;
  ProcessService *processes_;
  HttpClient *http_;
  std::string application_version_;
  Id boot_id_;
  std::filesystem::path data_root_;
  std::unique_ptr<ProfileService> profiles_;
  std::unique_ptr<RuntimeWriter> runtime_writer_;
  std::unique_ptr<CoreManager> core_manager_;
  std::unique_ptr<XrayControl> xray_control_;
  std::unique_ptr<MetricsClient> metrics_client_;
  std::unique_ptr<CoreInstaller> core_installer_;
  std::unique_ptr<NodeTestService> node_test_service_;
  std::unique_ptr<GeodataCache> geodata_cache_;
  LogBuffer logs_;
  Operations operations_;
  std::optional<Id> override_run_id_;
  std::optional<std::string> runtime_override_;
  OverrideState override_state_ = OverrideState::none;
  std::set<Id> metrics_viewers_;
  std::optional<Id> metrics_run_id_;
  std::map<Id, Id> node_test_processes_;
  std::map<Id, Json> node_test_results_;
  std::map<Id, Json> node_test_snapshots_;
  struct GeodataSchedule;
  GeodataSchedule *geodata_schedule_ = nullptr;
  std::set<Id> geodata_inflight_;
  void schedule_geodata_updates();
  void on_geodata_schedule();
  Result<std::vector<RuntimeAssetFile>>
  resolve_runtime_assets(const ProfileSettings &) const;
};

} // namespace xrm
