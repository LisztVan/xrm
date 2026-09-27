/// @file
/// @brief Declares the daemon RPC client interface.

#pragma once

#include "contract/wire.hpp"
#include "model/runtime.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace xrm {

/// @brief Correlates framed requests and responses over one local IPC socket.
///
/// open() completes asynchronously on the supplied libuv loop. call() uses
/// request IDs so overlapping replies reach their respective callbacks.
class RpcClient {
public:
  explicit RpcClient(uv_loop_t &loop);
  ~RpcClient();
  RpcClient(const RpcClient &) = delete;
  RpcClient &operator=(const RpcClient &) = delete;

  Result<Id> open(std::filesystem::path socket, Completion<void> completion);
  /// Requires a connected socket; transport failure completes pending calls
  /// with errors instead of leaving callbacks unresolved.
  Result<std::uint64_t> call(std::string method, Json params,
                             Completion<Json> completion);
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xrm
