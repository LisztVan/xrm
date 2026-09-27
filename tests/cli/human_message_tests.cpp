#include "client/human_message.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Human endpoint errors retain diagnostics without guessing server state", "[human-message]") {
  const xrm::AppError original{xrm::ErrorCode::io_error,
      "连接xrmd失败: no such file or directory", {}, {}};
  const auto message = xrm::human::error(original, "Save module", "Home / dns");
  CHECK(message.title == "Save module — Home / dns");
  CHECK(message.reason.find("endpoint") != std::string::npos);
  CHECK(message.reason.find("not running") == std::string::npos);
  CHECK(message.next == xrm::human::Recovery::endpoint);
  CHECK(message.diagnostic == original.message);
  CHECK(original.message == "连接xrmd失败: no such file or directory");
}

TEST_CASE("Human errors distinguish missing core and source conflict", "[human-message]") {
  auto core = xrm::human::error({xrm::ErrorCode::not_found, "核心不存在", "core-id", {}}, "Connect", "Home");
  CHECK(core.next == xrm::human::Recovery::core);
  CHECK(core.recovery.find("register") != std::string::npos);
  auto unset = xrm::human::error({xrm::ErrorCode::not_found, "核心不存在", "", {}}, "Connect", "Home");
  CHECK(unset.reason == "No Xray core is selected for this Profile.");
  CHECK(unset.diagnostic == "核心不存在");
  auto source = xrm::human::error({xrm::ErrorCode::conflict, "文件在读取后已被外部修改", "dns.json", "重新读取文件后再保存"}, "Save", "dns");
  CHECK(source.next == xrm::human::Recovery::source);
  CHECK(source.recovery.find("Keep your draft") != std::string::npos);
  CHECK(source.diagnostic.find("dns.json") != std::string::npos);
  CHECK(source.diagnostic.find("重新读取文件后再保存") != std::string::npos);
}

TEST_CASE("Unknown diagnostic detail is preserved and not translated into invented cause", "[human-message]") {
  auto message = xrm::human::error({xrm::ErrorCode::io_error, "new backend detail: E7", "target", {}}, "Read");
  CHECK(message.next == xrm::human::Recovery::source);
  CHECK(message.reason.find("could not be read or written") != std::string::npos);
  CHECK(message.diagnostic == "new backend detail: E7\nSubject: target");
  auto timeout = xrm::human::error({xrm::ErrorCode::result_unknown, "opaque failure", {}, {}}, "Save");
  CHECK(timeout.next == xrm::human::Recovery::operation);
  CHECK(timeout.recovery.find("before retrying") != std::string::npos);
}

TEST_CASE("Core registration and saved config checks have different recoveries", "[human-message]") {
  const xrm::AppError failure{xrm::ErrorCode::core_check_failed,
      "version输出未标识Xray核心", {}, {}};
  const auto registration = xrm::human::error(failure, "core register", "fixture", "core.register");
  CHECK(registration.reason.find("executable") != std::string::npos);
  CHECK(registration.recovery.find("binary path") != std::string::npos);
  CHECK(registration.next == xrm::human::Recovery::core);
  CHECK(registration.diagnostic == failure.message);
  const auto config = xrm::human::error(failure, "config check", "fixture", "config.check");
  CHECK(config.reason.find("configuration") != std::string::npos);
  CHECK(config.next == xrm::human::Recovery::source);
}

TEST_CASE("Stopped runtime lookup does not claim the Profile is missing", "[human-message]") {
  const xrm::AppError missing{xrm::ErrorCode::not_found,
      "当前没有运行配置", {}, {}};
  const auto message = xrm::human::error(missing, "config show", "Profile Lab Home", "config.files");
  CHECK(message.reason.find("No Xray process is running") != std::string::npos);
  CHECK(message.recovery.find("saved Source") != std::string::npos);
  CHECK(message.next == xrm::human::Recovery::runtime);
  CHECK(message.diagnostic == missing.message);
}

TEST_CASE("Subscription download failure is not presented as missing source", "[human-message]") {
  const auto failed = xrm::human::error({xrm::ErrorCode::network_error,
      "订阅服务器返回HTTP 404", {}, {}}, "sub update", "Fixture", "subscription.refresh");
  CHECK(failed.reason.find("HTTP") != std::string::npos);
  CHECK(failed.reason.find("404") != std::string::npos);
  CHECK(failed.recovery.find("source") != std::string::npos);
  CHECK(failed.reason.find("not found") == std::string::npos);
}

TEST_CASE("Ambiguous subscription names retain the available IDs in human output", "[human-message]") {
  const auto message = xrm::human::error({xrm::ErrorCode::conflict,
      "Subscription name is ambiguous. Use one source ID: id-one, id-two", {}, {}},
      "sub show", "duplicate", "subscription.get");
  CHECK(message.reason.find("id-one") != std::string::npos);
  CHECK(message.reason.find("id-two") != std::string::npos);
  CHECK(message.recovery.find("exact source ID") != std::string::npos);
}

TEST_CASE("Safe native JSON position stays visible without printing the source", "[human-message]") {
  const auto message = xrm::human::error({xrm::ErrorCode::invalid_argument,
      "Invalid native module JSON at byte 17", {}, {}}, "config edit", "dns", "module.save");
  CHECK(message.reason.find("byte 17") != std::string::npos);
  CHECK(message.recovery.find("no source file was saved") != std::string::npos);
}
