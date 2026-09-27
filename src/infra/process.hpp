/// @file
/// @brief Declares child-process execution and lifecycle primitives.

#pragma once

#include "model/runtime.hpp"

#include <memory>

namespace xrm {

struct ProcessCallbacks {
  std::function<void(ProcessId, std::string)> stdout_chunk;
  std::function<void(ProcessId, std::string)> stderr_chunk;
  std::function<void(ProcessId, ProcessExit)> exited;
  std::function<void(ProcessId, LogStream)> output_closed;
};

/// @brief Owns child processes created on one libuv event loop.
///
/// Callbacks finish after process exit and stream/handle draining. This
/// service cannot adopt or terminate an unrelated historical PID.
class ProcessService {
public:
  explicit ProcessService(uv_loop_t &loop);
  ~ProcessService();
  ProcessService(const ProcessService &) = delete;
  ProcessService &operator=(const ProcessService &) = delete;

  /// Starts an owned long-running child with explicit argv/cwd/environment.
  Result<ProcessId> spawn(ProcessSpec spec, ProcessCallbacks callbacks);
  /// Runs a bounded command and reports its final exit or timeout.
  Result<Id> execute(ProcessSpec spec, Milliseconds timeout,
                     Completion<ProcessResult> completion);
  /// Requests termination of a child returned by spawn(), with a grace time.
  Result<void> terminate(ProcessId process, Milliseconds grace);
  Result<void> cancel_command(const Id &command_id);
  std::optional<int> pid(ProcessId process) const;
  /// Invokes drained after every owned child and libuv handle is released.
  void shutdown(std::function<void()> drained);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xrm
