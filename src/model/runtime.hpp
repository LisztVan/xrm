/// @file
/// @brief Defines runtime file sets, core status, and metrics data.

#pragma once

#include "model/profile.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct uv_loop_s;
using uv_loop_t = uv_loop_s;

namespace xrm {

struct CoreInstall {
  Id id;
  std::string name;
  std::filesystem::path path;
  std::string reported_version;
  bool managed = false;
};

struct ProcessSpec {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path cwd;
  std::map<std::string, std::string> environment;
};
using ProcessId = std::uint64_t;
struct ProcessExit {
  int exit_code = 0;
  int term_signal = 0;
};
struct ProcessResult {
  ProcessExit exit;
  std::string stdout_text;
  std::string stderr_text;
  bool output_truncated = false;
};

struct NodeRuntimeEntry {
  Id id;
  std::string tag;
  std::string display_name;
};
struct GroupRuntimeEntry {
  Id id;
  std::string tag;
  std::vector<Id> candidate_ids;
  std::optional<Id> selected_node_id;
};
struct RuntimeIndex {
  std::vector<NodeRuntimeEntry> nodes;
  std::vector<GroupRuntimeEntry> groups;
  std::vector<std::string> static_outbound_tags;
  std::vector<std::string> static_balancer_tags;
};
/// Complete saved snapshot passed to pure runtime assembly.
struct BuildInput {
  // A snapshot of saved settings plus inventory, never a live UI draft.
  ProfileSnapshot settings;
  std::vector<Node> nodes;
};
struct OutputFile {
  std::filesystem::path relative_name;
  std::string bytes;
};
using FileSet = std::vector<OutputFile>;
/// Native files and resolved identities before filesystem materialization.
struct PreparedFiles {
  FileSet files;
  RuntimeIndex index;
  std::vector<Notice> notices;
};

struct RunSpec {
  std::filesystem::path binary;
  std::filesystem::path confdir;
  std::filesystem::path cwd;
  std::map<std::string, std::string> environment;
};

enum class DirectoryPurpose { preview, candidate };
struct PreparedDirectory {
  Id id;
  DirectoryPurpose purpose = DirectoryPurpose::preview;
  std::filesystem::path confdir;
  RuntimeIndex index;
};

enum class LogStream { server, core_stdout, core_stderr };
enum class CoreState { stopped, starting, running, stopping, failed };
enum class RuntimeHealth { process_only, control_ready, control_unavailable };
enum class OverrideState { none, confirmed, unknown };
/// Prepared run inputs and identity; CoreManager marks them active only after
/// observing the started process.
struct RunRecord {
  Id run_id;
  Id profile_id;
  Id core_id;
  RunSpec spec;
  std::optional<Target> applied_selection;
  RuntimeIndex index;
  UnixMillis started_at = 0;
  std::optional<std::string> api_endpoint;
  std::optional<std::string> metrics_endpoint;
};
/// Persisted recovery intent; it cannot by itself prove a live process.
struct RuntimePersistent {
  bool desired_running = false;
  // Persisted run identity is distinct from the live process status.
  std::optional<RunRecord> active;
  std::optional<RunRecord> previous;
};
/// Current owned-process observation returned to clients.
struct RuntimeStatus {
  CoreState state = CoreState::stopped;
  bool desired_running = false;
  std::optional<int> pid;
  std::optional<RunRecord> active;
  RuntimeHealth health = RuntimeHealth::process_only;
  std::optional<Target> actual_selection;
  OverrideState override_state = OverrideState::none;
  std::optional<std::string> runtime_override;
  std::optional<AppError> last_error;
};
struct RuntimeAssetFile {
  std::filesystem::path immutable_source;
  std::string runtime_name;
};
struct BuildRequest {
  BuildInput input;
  CoreInstall core;
  std::vector<RuntimeAssetFile> assets;
};
struct ReplayRequest {
  RunRecord record;
  CoreInstall core;
};
using ActivationInput = std::variant<BuildRequest, ReplayRequest>;
enum class ActivationMode {
  start_or_replace,
  check_only,
  preserve_running_state
};
struct ActivationRequest {
  ActivationInput input;
  ActivationMode mode;
};
enum class ActivationEffect {
  checked_only,
  running_new,
  restored_old,
  stopped,
  unknown
};
struct ActivationResult {
  ActivationEffect effect = ActivationEffect::unknown;
  std::optional<RunRecord> active;
  std::optional<AppError> attempt_error;
};
struct LifecycleOptions {
  Milliseconds check_timeout{30000};
  Milliseconds stop_grace{5000};
  Milliseconds process_observation{2000};
  std::array<Milliseconds, 3> retry_delay{
      Milliseconds{1000}, Milliseconds{3000}, Milliseconds{10000}};
  enum class Phase {
    prepare,
    checking,
    stopping_old,
    starting_new,
    restoring_old,
    retry_wait
  };
  // Deterministic test synchronization; callback code must not block the loop
  // thread.
  std::function<void(Phase)> phase_observer;
};

void to_json(Json &json, const CoreInstall &value);
void from_json(const Json &json, CoreInstall &value);
void to_json(Json &json, const RuntimePersistent &value);
void from_json(const Json &json, RuntimePersistent &value);

} // namespace xrm
