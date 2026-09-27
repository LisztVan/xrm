/// @file
/// @brief Maps daemon errors to human-readable client messages.

#pragma once

#include "model/common.hpp"
#include <string_view>

namespace xrm::human {

enum class Recovery {
  endpoint,
  core,
  object,
  source,
  runtime,
  operation,
  retry,
  none
};

struct Message {
  std::string title;
  std::string reason;
  std::string recovery;
  std::string diagnostic;
  Recovery next = Recovery::none;
};

// Presentation only: the wire error and its original diagnostic are unchanged.
// Callers supply an action and object from their own interaction context.
inline Message error(const AppError &error, std::string_view action,
                     std::string_view object = {},
                     std::string_view method = {}) {
  Message out;
  out.title = std::string(action);
  if (!object.empty())
    out.title += " — " + std::string(object);
  out.diagnostic = error.message;
  if (error.subject && !error.subject->empty())
    out.diagnostic += "\nSubject: " + *error.subject;
  if (error.hint && !error.hint->empty())
    out.diagnostic += "\nHint: " + *error.hint;
  const auto contains = [&](std::string_view value) {
    return error.message.find(value) != std::string::npos;
  };
  if (error.code == ErrorCode::io_error &&
      (contains("连接xrmd失败") || contains("IPC") ||
       contains("xrmd connection"))) {
    out.reason =
        "The xrmd endpoint is unavailable or the connection was interrupted.";
    out.recovery = "Check the socket path and the server serving that "
                   "endpoint. Confirm the result before retrying a write.";
    out.next = Recovery::endpoint;
  } else if (error.code == ErrorCode::not_found &&
             (contains("核心不存在") || contains("找不到已登记的核心"))) {
    out.reason = error.subject && error.subject->empty()
                     ? "No Xray core is selected for this Profile."
                     : "The selected Xray core is not registered.";
    out.recovery = "Choose a registered Xray core for this Profile; register a "
                   "binary if needed.";
    out.next = Recovery::core;
  } else if (error.code == ErrorCode::not_found && method == "config.files" &&
             contains("当前没有运行配置")) {
    out.reason = "No Xray process is running, so there are no runtime "
                 "configuration files to show.";
    out.recovery = "Inspect saved Source with config show PROFILE MODULE; use "
                   "status to check the actual runtime before connecting.";
    out.next = Recovery::runtime;
  } else if (error.code == ErrorCode::conflict && contains("外部修改")) {
    out.reason = "The source file changed after it was read.";
    out.recovery = "Keep your draft, reload the saved source, and reconcile "
                   "the changes before saving.";
    out.next = Recovery::source;
  } else if (error.code == ErrorCode::conflict &&
             error.message.starts_with("Subscription name is ambiguous.")) {
    out.reason = error.message;
    out.recovery = "Use one exact source ID shown above; the name alone does "
                   "not select a unique source.";
    out.next = Recovery::object;
  } else if (error.code == ErrorCode::core_check_failed &&
             method == "core.register") {
    out.reason =
        "The executable did not identify itself as a working Xray core.";
    out.recovery = "Check the binary path and run its version command, then "
                   "register the correct Xray executable.";
    out.next = Recovery::core;
  } else if (error.code == ErrorCode::invalid_argument &&
             error.message.starts_with("Invalid native module JSON at byte ")) {
    out.reason = error.message;
    out.recovery = "Correct the JSON syntax at the reported byte, then retry; "
                   "no source file was saved.";
    out.next = Recovery::source;
  } else if (error.code == ErrorCode::invalid_argument &&
             error.subject == "url" &&
             (method == "subscription.add" ||
              method == "subscription.replace")) {
    out.reason =
        "The subscription URL must be a complete HTTP(S) address with a host.";
    out.recovery =
        "Correct the URL field before saving; no download was attempted.";
    out.next = Recovery::object;
  } else if (method == "subscription.refresh" &&
             error.code == ErrorCode::network_error) {
    const auto status = error.message.find("HTTP ");
    if (status == std::string::npos) {
      out.reason = "The subscription download failed.";
    } else {
      const auto start = status + 5;
      const auto end = error.message.find_first_not_of("0123456789", start);
      out.reason = "The subscription server returned HTTP " +
                   error.message.substr(start, end - start) + ".";
    }
    out.recovery = "Inspect the source's last error and check its URL, server "
                   "response, and network route before retrying.";
    out.next = Recovery::object;
  } else if (method == "subscription.refresh" &&
             error.code == ErrorCode::invalid_argument &&
             contains("订阅没有可用节点")) {
    out.reason = "The subscription response contained no usable nodes.";
    out.recovery = "Inspect the source's parse error and correct the source or "
                   "its URL before retrying.";
    out.next = Recovery::object;
  } else {
    switch (error.code) {
    case ErrorCode::invalid_argument:
      out.reason = "The requested values were rejected.";
      out.recovery = "Review the diagnostic and correct the indicated field; "
                     "your local draft can be kept.";
      out.next = Recovery::source;
      break;
    case ErrorCode::not_found:
      out.reason =
          "The requested object or a required reference was not found.";
      out.recovery =
          "Refresh the relevant list and use an existing name or ID.";
      out.next = Recovery::object;
      break;
    case ErrorCode::conflict:
      out.reason = "The operation conflicts with the current saved state.";
      out.recovery =
          "Inspect the current object and its references before retrying.";
      out.next = Recovery::object;
      break;
    case ErrorCode::busy:
      out.reason = "Another operation is still in progress.";
      out.recovery = "Wait for its result, then try again.";
      out.next = Recovery::retry;
      break;
    case ErrorCode::unsupported:
      out.reason = "This operation is not supported for the selected object.";
      out.recovery =
          "Review the object's supported operations and the diagnostic.";
      out.next = Recovery::object;
      break;
    case ErrorCode::io_error:
    case ErrorCode::storage_error:
      out.reason = "The requested data could not be read or written.";
      out.recovery = "Check the path, access permissions and storage "
                     "diagnostic; keep unsaved changes.";
      out.next = Recovery::source;
      break;
    case ErrorCode::network_error:
      out.reason = "The network request failed.";
      out.recovery =
          "Check the source address and network connection before retrying.";
      out.next = Recovery::object;
      break;
    case ErrorCode::timed_out:
    case ErrorCode::result_unknown:
      out.reason = "No final result was confirmed.";
      out.recovery = "Inspect the operation or current saved state before "
                     "retrying; it may still complete.";
      out.next = Recovery::operation;
      break;
    case ErrorCode::cancelled:
      out.reason = "The operation was cancelled.";
      out.recovery =
          "Inspect the current state before starting a new operation.";
      out.next = Recovery::object;
      break;
    case ErrorCode::core_check_failed:
      out.reason = "Xray rejected the candidate configuration.";
      out.recovery = "Use --verbose to read Xray's diagnostic, then correct "
                     "the saved configuration before connecting.";
      out.next = Recovery::source;
      break;
    case ErrorCode::core_start_failed:
    case ErrorCode::core_control_failed:
      out.reason = "The requested Xray process action failed.";
      out.recovery = "Inspect the actual runtime state and core diagnostic "
                     "before retrying.";
      out.next = Recovery::runtime;
      break;
    case ErrorCode::protocol_error:
      out.reason = "The server response could not be interpreted.";
      out.recovery =
          "Check the client and server versions, and retain the diagnostic.";
      out.next = Recovery::endpoint;
      break;
    case ErrorCode::too_large:
      out.reason = "The input exceeds the supported size.";
      out.recovery =
          "Reduce the input according to the limit in the diagnostic.";
      out.next = Recovery::source;
      break;
    }
  }
  return out;
}

} // namespace xrm::human
