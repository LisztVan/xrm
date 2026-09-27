// XRM: Declares the RPC wire format and error envelope.

#pragma once

#include "model/common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xrm {

struct RpcRequest {
  int v = 1;
  std::uint64_t id = 0;
  std::string method;
  Json params = Json::object();
};

struct RpcResponse {
  int v = 1;
  std::uint64_t id = 0;
  bool ok = false;
  std::optional<Json> result;
  std::optional<AppError> error;
};

Result<RpcRequest> decode_request(std::string_view line);
std::string encode_response(const RpcResponse &response);
Json error_to_json(const AppError &error);
Result<AppError> error_from_json(const Json &value);

} // namespace xrm
