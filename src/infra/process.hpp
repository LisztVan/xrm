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

class ProcessService {
 public:
  explicit ProcessService(uv_loop_t& loop);
  ~ProcessService();
  ProcessService(const ProcessService&) = delete;
  ProcessService& operator=(const ProcessService&) = delete;

  Result<ProcessId> spawn(ProcessSpec spec, ProcessCallbacks callbacks);
  Result<Id> execute(ProcessSpec spec, Milliseconds timeout,
                     Completion<ProcessResult> completion);
  Result<void> terminate(ProcessId process, Milliseconds grace);
  Result<void> cancel_command(const Id& command_id);
  std::optional<int> pid(ProcessId process) const;
  void shutdown(std::function<void()> drained);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm
