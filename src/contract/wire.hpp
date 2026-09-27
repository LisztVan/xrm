/// @file
/// @brief Declares the RPC wire format and error envelope.

#pragma once

#include "model/common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xrm {

struct RpcRequest {
  // v is the wire version; id correlates one response with this request.
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

/// Decodes one JSON-line request and validates its envelope.
Result<RpcRequest> decode_request(std::string_view line);
/// Encodes one response as a complete wire line, including its request ID.
std::string encode_response(const RpcResponse &response);
Json error_to_json(const AppError &error);
Result<AppError> error_from_json(const Json &value);

} // namespace xrm
