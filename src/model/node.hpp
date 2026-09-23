#pragma once

#include "model/common.hpp"

#include <optional>
#include <string>
#include <vector>

namespace xrm {

// M0 stores opaque native outbound data so node inventory does not depend on
// the later protocol codec work. The stable identity and user metadata remain
// manager-owned; Xray configuration is never stored in the profile database.
struct Node {
  Id id;
  std::optional<Id> source_id;
  std::string original_name;
  std::optional<std::string> alias;
  std::vector<std::string> labels;
  bool enabled = true;
  bool source_missing = false;
  Json outbound = Json::object();
};

struct NodeQuery {
  std::optional<Id> source_id;
  std::optional<std::string> text;
  std::vector<std::string> labels;
  bool include_missing = false;
};

}  // namespace xrm
