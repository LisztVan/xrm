#pragma once

#include "model/node.hpp"
#include "model/runtime.hpp"

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

  Result<void> register_profile(const Id&, const std::filesystem::path&);
  Result<std::filesystem::path> profile_path(const Id&) const;
  Result<std::vector<Id>> profile_ids() const;
  Result<void> unregister_profile(const Id&);

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
