// XRM: Defines shared identifiers, errors, and JSON model helpers.

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace xrm {

using Id = std::string;
using Json = nlohmann::json;
using Milliseconds = std::chrono::milliseconds;
using UnixMillis = std::int64_t;

enum class ErrorCode {
  invalid_argument,
  not_found,
  conflict,
  busy,
  unsupported,
  io_error,
  storage_error,
  network_error,
  timed_out,
  cancelled,
  core_check_failed,
  core_start_failed,
  core_control_failed,
  protocol_error,
  too_large,
  result_unknown,
};

struct AppError {
  ErrorCode code;
  std::string message;
  std::optional<std::string> subject;
  std::optional<std::string> hint;
};

template <class T> using Result = std::expected<T, AppError>;

template <class T> using Completion = std::function<void(Result<T>)>;

struct PageRequest {
  std::size_t offset = 0;
  std::size_t limit = 100;
};

template <class T> struct Page {
  std::vector<T> items;
  std::size_t total = 0;
  std::optional<std::size_t> next_offset;
};

struct Notice {
  std::string code;
  std::string message;
};

std::string_view to_string(ErrorCode code);
Result<ErrorCode> error_code_from_string(std::string_view value);

} // namespace xrm
