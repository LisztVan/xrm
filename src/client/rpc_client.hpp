// XRM: Declares the daemon RPC client interface.

#pragma once

#include "contract/wire.hpp"
#include "model/runtime.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace xrm {

class RpcClient {
public:
  explicit RpcClient(uv_loop_t &loop);
  ~RpcClient();
  RpcClient(const RpcClient &) = delete;
  RpcClient &operator=(const RpcClient &) = delete;

  Result<Id> open(std::filesystem::path socket, Completion<void> completion);
  Result<std::uint64_t> call(std::string method, Json params,
                             Completion<Json> completion);
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xrm
