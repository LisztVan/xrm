#pragma once

#include "contract/dtos.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"

namespace xrm {

class Application {
 public:
  Application(Store& store, ProcessService& processes, std::string application_version,
              Id boot_id);

  Json server_info() const;
  Result<NodeSummary> add_manual_node(std::string name, Json outbound);
  Result<Page<NodeSummary>> list_nodes(const NodeQuery& query, PageRequest page) const;
  Result<std::vector<CoreInstall>> list_cores() const;
  void register_core(std::filesystem::path path, std::string name,
                     Completion<CoreInstall> completion);

 private:
  Store* store_;
  ProcessService* processes_;
  std::string application_version_;
  Id boot_id_;
};

}  // namespace xrm
