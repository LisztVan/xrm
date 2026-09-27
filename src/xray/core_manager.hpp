// XRM: Declares owned core-process lifecycle state and operations.

#pragma once

#include "infra/process.hpp"
#include "model/runtime.hpp"
#include "xray/runtime_writer.hpp"

#include <functional>
#include <memory>

namespace xrm {

class Store;

class CoreManager {
public:
  CoreManager(uv_loop_t &, Store &, ProcessService &, RuntimeWriter &,
              LifecycleOptions = {});
  ~CoreManager();
  CoreManager(const CoreManager &) = delete;
  CoreManager &operator=(const CoreManager &) = delete;
  bool busy() const;
  RuntimeStatus status() const;
  void on_output(std::function<void(LogStream, std::string)> callback);
  void on_output_closed(std::function<void(LogStream)> callback);
  // Admission can fail synchronously (e.g. busy) without taking the callback.
  // Once accepted, completion describes the eventual checked/running outcome.
  Result<Id> activate(ActivationRequest, Completion<ActivationResult>);
  // Stop records user intent, preventing automatic retry/recovery.
  Result<Id> stop(Completion<ActivationResult>);
  Result<Id> restore_after_server_start(Completion<ActivationResult>);
  void shutdown(std::function<void()> drained);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xrm
