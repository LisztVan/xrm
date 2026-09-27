/// @file
/// @brief Encodes and decodes framed RPC messages.

#include "contract/wire.hpp"

#include <array>

namespace xrm {
namespace {

constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;

} // namespace

std::string_view to_string(ErrorCode code) {
  constexpr std::array values{"invalid_argument",
                              "not_found",
                              "conflict",
                              "busy",
                              "unsupported",
                              "io_error",
                              "storage_error",
                              "network_error",
                              "timed_out",
                              "cancelled",
                              "core_check_failed",
                              "core_start_failed",
                              "core_control_failed",
                              "protocol_error",
                              "too_large",
                              "result_unknown"};
  const auto index = static_cast<std::size_t>(code);
  return index < values.size() ? values[index] : "protocol_error";
}

Result<ErrorCode> error_code_from_string(std::string_view value) {
  for (std::size_t index = 0;
       index <= static_cast<std::size_t>(ErrorCode::result_unknown); ++index) {
    const auto code = static_cast<ErrorCode>(index);
    if (to_string(code) == value)
      return code;
  }
  return std::unexpected(AppError{ErrorCode::protocol_error, "未知错误代码",
                                  std::nullopt, std::nullopt});
}

Json error_to_json(const AppError &error) {
  Json value{{"code", to_string(error.code)}, {"message", error.message}};
  if (error.subject)
    value["subject"] = *error.subject;
  if (error.hint)
    value["hint"] = *error.hint;
  return value;
}

Result<AppError> error_from_json(const Json &value) {
  if (!value.is_object() || !value.contains("code") ||
      !value.at("code").is_string() || !value.contains("message") ||
      !value.at("message").is_string()) {
    return std::unexpected(AppError{ErrorCode::protocol_error,
                                    "错误响应缺少有效的 code 或 message",
                                    std::nullopt, std::nullopt});
  }
  auto code = error_code_from_string(value.at("code").get<std::string>());
  if (!code)
    return std::unexpected(code.error());
  AppError error{*code, value.at("message").get<std::string>(), std::nullopt,
                 std::nullopt};
  if (value.contains("subject")) {
    if (!value.at("subject").is_string()) {
      return std::unexpected(AppError{ErrorCode::protocol_error,
                                      "错误响应 subject 必须是字符串",
                                      std::nullopt, std::nullopt});
    }
    error.subject = value.at("subject").get<std::string>();
  }
  if (value.contains("hint")) {
    if (!value.at("hint").is_string()) {
      return std::unexpected(AppError{ErrorCode::protocol_error,
                                      "错误响应 hint 必须是字符串",
                                      std::nullopt, std::nullopt});
    }
    error.hint = value.at("hint").get<std::string>();
  }
  return error;
}

Result<RpcRequest> decode_request(std::string_view line) {
  if (line.size() > kMaxFrameBytes) {
    return std::unexpected(AppError{ErrorCode::too_large, "RPC帧超过8MiB上限",
                                    std::nullopt, std::nullopt});
  }
  try {
    const auto json = Json::parse(line);
    if (!json.is_object() || !json.contains("v") ||
        !json.at("v").is_number_integer() || !json.contains("id") ||
        !json.at("id").is_number_unsigned() || !json.contains("method") ||
        !json.at("method").is_string() || !json.contains("params") ||
        !json.at("params").is_object()) {
      return std::unexpected(
          AppError{ErrorCode::protocol_error,
                   "RPC请求缺少有效的 v、id、method 或 params", std::nullopt,
                   std::nullopt});
    }
    const int version = json.at("v").get<int>();
    if (version != 1) {
      return std::unexpected(AppError{ErrorCode::unsupported,
                                      "不支持的RPC协议版本", std::nullopt,
                                      "客户端和服务端必须使用协议版本1"});
    }
    return RpcRequest{version, json.at("id").get<std::uint64_t>(),
                      json.at("method").get<std::string>(), json.at("params")};
  } catch (const Json::exception &) {
    return std::unexpected(AppError{ErrorCode::protocol_error,
                                    "RPC请求不是有效JSON", std::nullopt,
                                    std::nullopt});
  }
}

std::string encode_response(const RpcResponse &response) {
  Json value{{"v", response.v}, {"id", response.id}, {"ok", response.ok}};
  if (response.ok && response.result) {
    value["result"] = *response.result;
  } else if (!response.ok && response.error) {
    value["error"] = error_to_json(*response.error);
  }
  return value.dump() + '\n';
}

} // namespace xrm
