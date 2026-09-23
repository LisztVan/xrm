#pragma once

#include "contract/dtos.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
#include "profile/profile_service.hpp"
#include "server/log_buffer.hpp"
#include "server/operations.hpp"
#include "subscription/subscription_service.hpp"
#include "xray/core_manager.hpp"

namespace xrm {

class Application {
 public:
  Application(Store& store, ProcessService& processes, uv_loop_t& loop,
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
  Result<Json> show_config(const Id& profile_id, std::string_view source,
                           std::optional<std::string> module) const;
  void recover(Completion<ActivationResult> completion);
  void shutdown(std::function<void()> drained);

 private:
  Store* store_;
  SubscriptionService* subscriptions_;
  ProcessService* processes_;
  std::string application_version_;
  Id boot_id_;
  std::filesystem::path data_root_;
  std::unique_ptr<ProfileService> profiles_;
  std::unique_ptr<RuntimeWriter> runtime_writer_;
  std::unique_ptr<CoreManager> core_manager_;
  LogBuffer logs_;
  Operations operations_;
};

}  // namespace xrm
