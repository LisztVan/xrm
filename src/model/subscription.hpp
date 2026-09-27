/// @file
/// @brief Defines subscription sources and refresh results.

#pragma once

#include "model/node.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xrm {

struct DownloadRoute {
  std::optional<std::string> proxy_url;
};

struct Subscription {
  Id id;
  std::string name;
  std::string url;
  std::string user_agent = "Xray";
  std::map<std::string, std::string> headers;
  bool auto_refresh = true;
  std::chrono::seconds interval{21600};
  DownloadRoute route;
  // null: built-in conservative information prefixes; []: disable this
  // classification for a source. Explicit prefixes are exact starts-with.
  std::optional<std::vector<std::string>> info_name_prefixes;
  std::uint64_t settings_revision = 0;
};

struct SubscriptionState {
  std::optional<UnixMillis> last_attempt;
  std::optional<UnixMillis> last_success;
  std::optional<std::string> etag;
  std::optional<std::string> last_modified;
  std::string last_result;
  // The latest refresh diagnostic belongs to this source, not the last
  // successful inventory. It is cleared when settings change or a refresh
  // succeeds. Older state JSON may omit both fields.
  std::optional<std::string> last_error;
  std::optional<std::string> last_error_stage;
  std::optional<std::string> reported_usage;
  std::vector<std::string> reported_notices;
  std::size_t last_unsupported_count = 0;
  std::size_t last_malformed_count = 0;
};

struct ParsedNode {
  std::optional<std::string> upstream_id;
  std::string name;
  NodeSettings settings;
};

enum class IssueKind { malformed, unsupported, warning };

struct ParseIssue {
  std::size_t item_index = 0;
  IssueKind kind = IssueKind::malformed;
  std::string code;
  std::string message;
};

struct ParseReport {
  std::vector<ParsedNode> nodes;
  std::vector<ParseIssue> issues;
  bool structurally_complete = false;
};

struct RefreshReport {
  Id source_id;
  std::size_t added = 0;
  std::size_t updated = 0;
  std::size_t missing = 0;
  std::size_t malformed = 0;
  std::size_t unsupported = 0;
  bool changed = false;
  bool partial = false;
  std::optional<std::string> first_issue;
  std::vector<Notice> notices;
};

struct SourceCommit {
  std::uint64_t expected_settings_revision = 0;
  NodeChanges changes;
  SubscriptionState state;
  RefreshReport report;
};

void to_json(Json &json, const Subscription &value);
void from_json(const Json &json, Subscription &value);
void to_json(Json &json, const SubscriptionState &value);
void from_json(const Json &json, SubscriptionState &value);

} // namespace xrm
