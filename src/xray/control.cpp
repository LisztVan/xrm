// XRM: Issues explicit control requests to the owned Xray process.

#include "xray/control.hpp"

#include <algorithm>
#include <cmath>

namespace xrm {
namespace {

AppError control_error(std::string message,
                       const std::optional<Id> &subject = std::nullopt) {
  return {ErrorCode::core_control_failed, std::move(message), subject,
          std::nullopt};
}

Result<void> validate(const ControlTarget &target,
                      std::string_view balancer_tag) {
  if (target.run.binary.empty() || target.endpoint.empty() ||
      balancer_tag.empty()) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "Xray API control requires a running "
                                    "binary, endpoint, and balancer tag",
                                    std::nullopt, std::nullopt});
  }
  if (target.timeout <= Milliseconds::zero()) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "Xray API timeout must be positive",
                                    std::nullopt, std::nullopt});
  }
  return {};
}

ProcessSpec process_spec(const ControlTarget &target,
                         std::vector<std::string> arguments) {
  ProcessSpec spec;
  spec.executable = target.run.binary;
  spec.arguments = std::move(arguments);
  spec.cwd = target.run.cwd;
  spec.environment = target.run.environment;
  return spec;
}

std::vector<std::string> api_options(const ControlTarget &target,
                                     bool json = false) {
  const auto seconds =
      std::max<std::int64_t>(1, (target.timeout.count() + 999) / 1000);
  std::vector<std::string> options{"--server=" + target.endpoint, "-t",
                                   std::to_string(seconds)};
  if (json)
    options.push_back("--json");
  return options;
}

std::optional<std::string> string_member(const Json &value,
                                         std::string_view key) {
  const auto found = value.find(std::string(key));
  if (found != value.end() && found->is_string())
    return found->get<std::string>();
  return std::nullopt;
}

const Json *balancer_info_object(const Json &value) {
  const auto found = value.find("balancer");
  return found != value.end() && found->is_object() ? &*found : nullptr;
}

std::optional<std::string> override_target(const Json &value) {
  const auto *balancer = balancer_info_object(value);
  if (!balancer)
    return std::nullopt;
  const auto override = balancer->find("override");
  if (override == balancer->end() || !override->is_object())
    return std::nullopt;
  return string_member(*override, "target");
}

void query_after_write(ProcessService &process, ControlTarget target,
                       std::string balancer_tag,
                       std::optional<std::string> expected,
                       bool write_timed_out,
                       Completion<OverrideResult> completion) {
  std::vector<std::string> arguments{"api", "bi"};
  auto options = api_options(target, true);
  arguments.insert(arguments.end(), options.begin(), options.end());
  arguments.push_back(balancer_tag);
  auto complete =
      std::make_shared<Completion<OverrideResult>>(std::move(completion));
  auto started = process.execute(
      process_spec(target, std::move(arguments)), target.timeout,
      [balancer_tag = std::move(balancer_tag), expected = std::move(expected),
       write_timed_out, complete](auto result) mutable {
        if (!result) {
          (*complete)(OverrideResult{false, std::nullopt});
          return;
        }
        if (result->exit.exit_code != 0 || result->exit.term_signal != 0) {
          (*complete)(OverrideResult{false, std::nullopt});
          return;
        }
        try {
          const auto info = Json::parse(result->stdout_text);
          const auto actual = override_target(info);
          const bool exists = balancer_info_object(info) != nullptr;
          const bool confirmed = expected
                                     ? actual == expected
                                     : (exists && !actual && !write_timed_out);
          (*complete)(OverrideResult{confirmed, actual});
        } catch (const Json::exception &) {
          (*complete)(OverrideResult{false, std::nullopt});
        }
      });
  if (!started)
    (*complete)(OverrideResult{false, std::nullopt});
}

} // namespace

XrayControl::XrayControl(ProcessService &process) : process_(process) {}

Result<Id> XrayControl::balancer_info(ControlTarget target,
                                      std::string balancer_tag,
                                      Completion<Json> completion) {
  auto valid = validate(target, balancer_tag);
  if (!valid)
    return std::unexpected(valid.error());
  std::vector<std::string> arguments{"api", "bi"};
  auto options = api_options(target, true);
  arguments.insert(arguments.end(), options.begin(), options.end());
  arguments.push_back(balancer_tag);
  return process_.execute(
      process_spec(target, std::move(arguments)), target.timeout,
      [completion = std::move(completion)](auto result) mutable {
        if (!result) {
          completion(std::unexpected(result.error()));
          return;
        }
        if (result->exit.exit_code != 0 || result->exit.term_signal != 0) {
          completion(std::unexpected(control_error(
              "Xray balancer query failed: " + result->stderr_text)));
          return;
        }
        try {
          completion(Json::parse(result->stdout_text));
        } catch (const Json::exception &error) {
          completion(std::unexpected(control_error(
              std::string("Xray balancer query did not return JSON: ") +
              error.what())));
        }
      });
}

Result<Id>
XrayControl::override_outbound(ControlTarget target, std::string balancer_tag,
                               std::string outbound_tag,
                               Completion<OverrideResult> completion) {
  auto valid = validate(target, balancer_tag);
  if (!valid)
    return std::unexpected(valid.error());
  if (outbound_tag.empty()) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "outbound tag must not be empty",
                                    std::nullopt, std::nullopt});
  }
  std::vector<std::string> arguments{"api", "bo"};
  auto options = api_options(target);
  arguments.insert(arguments.end(), options.begin(), options.end());
  arguments.insert(arguments.end(), {"-b", balancer_tag, outbound_tag});
  auto started = process_.execute(
      process_spec(target, std::move(arguments)), target.timeout,
      [this, target, balancer_tag = std::move(balancer_tag),
       outbound_tag = std::move(outbound_tag),
       completion = std::move(completion)](auto result) mutable {
        if (!result && result.error().code != ErrorCode::timed_out) {
          completion(std::unexpected(result.error()));
          return;
        }
        if (result &&
            (result->exit.exit_code != 0 || result->exit.term_signal != 0)) {
          completion(std::unexpected(control_error(
              "Xray balancer override failed: " + result->stderr_text)));
          return;
        }
        const bool timed_out =
            !result && result.error().code == ErrorCode::timed_out;
        query_after_write(process_, std::move(target), std::move(balancer_tag),
                          std::move(outbound_tag), timed_out,
                          std::move(completion));
      });
  return started;
}

Result<Id> XrayControl::clear_override(ControlTarget target,
                                       std::string balancer_tag,
                                       Completion<OverrideResult> completion) {
  auto valid = validate(target, balancer_tag);
  if (!valid)
    return std::unexpected(valid.error());
  std::vector<std::string> arguments{"api", "bo"};
  auto options = api_options(target);
  arguments.insert(arguments.end(), options.begin(), options.end());
  arguments.insert(arguments.end(), {"-b", balancer_tag, "-r"});
  return process_.execute(
      process_spec(target, std::move(arguments)), target.timeout,
      [this, target, balancer_tag = std::move(balancer_tag),
       completion = std::move(completion)](auto result) mutable {
        if (!result && result.error().code != ErrorCode::timed_out) {
          completion(std::unexpected(result.error()));
          return;
        }
        if (result &&
            (result->exit.exit_code != 0 || result->exit.term_signal != 0)) {
          completion(std::unexpected(control_error(
              "Xray balancer override clear failed: " + result->stderr_text)));
          return;
        }
        const bool timed_out =
            !result && result.error().code == ErrorCode::timed_out;
        query_after_write(process_, std::move(target), std::move(balancer_tag),
                          std::nullopt, timed_out, std::move(completion));
      });
}

} // namespace xrm
