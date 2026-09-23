#pragma once

#include "model/node.hpp"
#include "model/runtime.hpp"
#include "model/subscription.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace xrm {

class Store {
 public:
  static Result<std::unique_ptr<Store>> open(std::filesystem::path database);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  Result<Page<Node>> list_nodes(const NodeQuery&, PageRequest) const;
  Result<Node> get_node(const Id&) const;
  Result<std::vector<Node>> all_nodes() const;
  Result<void> put_node(const Node&);
  Result<void> erase_node(const Id&);

  Result<std::vector<Subscription>> list_sources() const;
  Result<Subscription> get_source(const Id&) const;
  Result<SubscriptionState> get_source_state(const Id&) const;
  Result<void> put_source(const Subscription&);
  Result<void> remove_source(const Id&, bool keep_as_manual);
  Result<void> commit_source(const Id&, const SourceCommit&);

  Result<void> register_profile(const Id&, const std::filesystem::path&);
  Result<std::filesystem::path> profile_path(const Id&) const;
  Result<std::vector<Id>> profile_ids() const;
  Result<void> unregister_profile(const Id&);

  Result<RuntimePersistent> load_runtime() const;
  Result<void> save_runtime(const RuntimePersistent&);

  Result<std::vector<CoreInstall>> cores() const;
  Result<CoreInstall> get_core(const Id&) const;
  Result<void> put_core(const CoreInstall&);
  Result<void> remove_core(const Id&);

 private:
  struct Impl;
  explicit Store(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm
