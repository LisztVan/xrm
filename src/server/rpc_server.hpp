#pragma once

#include "contract/wire.hpp"
#include "model/runtime.hpp"

#include <filesystem>
#include <functional>
#include <memory>

namespace xrm {

using RpcReply = std::function<void(RpcResponse)>;
using RpcHandler = std::function<void(const Id&, RpcRequest, RpcReply)>;

class RpcServer {
 public:
  RpcServer(uv_loop_t& loop, RpcHandler handler);
  ~RpcServer();
  RpcServer(const RpcServer&) = delete;
  RpcServer& operator=(const RpcServer&) = delete;

  Result<void> listen(const std::filesystem::path& socket);
  void close(std::function<void()> drained);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm
