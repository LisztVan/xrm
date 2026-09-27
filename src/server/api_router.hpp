/// @file
/// @brief Declares the daemon RPC method router.

#pragma once

#include "server/application.hpp"
#include "server/rpc_server.hpp"

namespace xrm {

/// @brief Translates a validated RPC envelope into a domain call.
///
/// Method-specific parameter checks and reply shapes belong here; saved
/// state and runtime effects belong to Application and its services.
class ApiRouter {
public:
  explicit ApiRouter(Application &application);
  void dispatch(const Id &connection_id, RpcRequest request, RpcReply reply);

private:
  Application *application_;
};

} // namespace xrm
