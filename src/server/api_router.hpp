#pragma once

#include "server/application.hpp"
#include "server/rpc_server.hpp"

namespace xrm {

class ApiRouter {
 public:
  explicit ApiRouter(Application& application);
  void dispatch(const Id& connection_id, RpcRequest request, RpcReply reply);

 private:
  Application* application_;
};

}  // namespace xrm
