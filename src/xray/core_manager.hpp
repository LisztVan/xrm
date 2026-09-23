#pragma once

#include "infra/process.hpp"
#include "model/runtime.hpp"
#include "xray/runtime_writer.hpp"

#include <functional>

namespace xrm {

class Store;

class CoreManager {
 public:
  CoreManager(uv_loop_t&, Store&, ProcessService&, RuntimeWriter&,
              LifecycleOptions = {});
  bool busy() const;
  RuntimeStatus status() const;
  void on_output(std::function<void(LogStream, std::string)> callback);
  Result<Id> activate(ActivationRequest, Completion<ActivationResult>);
  Result<Id> stop(Completion<ActivationResult>);
  Result<Id> restore_after_server_start(Completion<ActivationResult>);
  void shutdown(std::function<void()> drained);
};

}  // namespace xrm
