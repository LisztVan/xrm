// XRM: Tests contract in the compile-time and wire contracts suite.

#include <catch2/catch_test_macros.hpp>

#include "client/rpc_client.hpp"
#include "contract/dtos.hpp"
#include "contract/methods.hpp"
#include "contract/wire.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
#include "model/profile.hpp"
#include "model/subscription.hpp"
#include "server/rpc_server.hpp"
#include "xray/runtime_assembler.hpp"

TEST_CASE("C0 headers are independently usable", "[CT-01]") {
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::Node>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::CoreInstall>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::RpcRequest>);
  STATIC_REQUIRE(std::is_move_constructible_v<xrm::Store> == false);
}

TEST_CASE("C0 DTOs and errors have stable JSON representations", "[CT-02]") {
  const xrm::NodeSummary summary{"node-1", "example", {"test"},
                                 true,     false,     "vless"};
  const auto json = xrm::Json(summary);
  REQUIRE(json.at("id") == "node-1");
  REQUIRE(json.at("name") == "example");
  REQUIRE_FALSE(json.contains("password"));
  REQUIRE(json.get<xrm::NodeSummary>().protocol == "vless");

  const xrm::AppError source{xrm::ErrorCode::conflict, "changed", "policy",
                             "reload"};
  const auto decoded = xrm::error_from_json(xrm::error_to_json(source));
  REQUIRE(decoded);
  REQUIRE(decoded->code == source.code);
  REQUIRE(decoded->subject == source.subject);
  REQUIRE_FALSE(xrm::error_code_from_string("999"));
}

TEST_CASE("C0 request decoder validates the M0 JSON line envelope", "[CT-02]") {
  const auto request = xrm::decode_request(
      R"({"v":1,"id":17,"method":"node.list","params":{}})");
  REQUIRE(request);
  REQUIRE(request->id == 17);
  REQUIRE(request->method == "node.list");
  REQUIRE_FALSE(xrm::decode_request(
      R"({"v":1,"id":17,"method":"node.list","params":null})"));
}

TEST_CASE("C1 freezes current M1 profile and runtime surfaces", "[CT-01]") {
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::StreamNode>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::ProfileSettings>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::ModuleDocument>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::OutputFile>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::PreparedFiles>);
  STATIC_REQUIRE(std::is_same_v<decltype(xrm::assemble(
                                    std::declval<const xrm::BuildInput &>())),
                                xrm::Result<xrm::PreparedFiles>>);
  REQUIRE(xrm::methods::profile_create == "profile.create");
  REQUIRE(xrm::methods::module_list == "module.list");
  REQUIRE(xrm::methods::connection_connect == "connection.connect");
  REQUIRE(xrm::methods::core_stop == "core.stop");
  REQUIRE(xrm::methods::config_apply == "config.apply");
  REQUIRE(xrm::methods::config_dump == "config.dump");
  REQUIRE(xrm::methods::config_files == "config.files");
  REQUIRE(xrm::methods::config_export == "config.export");
}

TEST_CASE("C2 subscription DTO summary does not disclose its URL", "[CT-02]") {
  const xrm::SubscriptionSummary summary{
      "source-1", "example", true, "success", 100, 200, 4, 1, 2};
  const auto encoded = xrm::Json(summary);
  REQUIRE(encoded.at("id") == "source-1");
  REQUIRE(encoded.at("name") == "example");
  REQUIRE_FALSE(encoded.contains("url"));
  REQUIRE_FALSE(encoded.contains("headers"));
  REQUIRE(encoded.get<xrm::SubscriptionSummary>().unsupported_count == 1);
  REQUIRE(xrm::methods::subscription_refresh == "subscription.refresh");
  REQUIRE(xrm::methods::node_copy == "node.copy");

  xrm::Subscription source;
  source.id = "source-1";
  source.name = "example";
  source.url = "https://user:secret@example.invalid/sub?id=hidden";
  source.headers.emplace("Authorization", "Bearer hidden");
  source.settings_revision = 3;
  source.info_name_prefixes = std::vector<std::string>{"剩余流量："};
  const auto source_copy = xrm::Json(source).get<xrm::Subscription>();
  REQUIRE(source_copy.url == source.url);
  REQUIRE(source_copy.headers == source.headers);
  REQUIRE(source_copy.settings_revision == 3);
  REQUIRE(source_copy.info_name_prefixes == source.info_name_prefixes);

  xrm::Node node;
  node.id = "node-1";
  node.source_id = "source-1";
  node.upstream_id = "line-4";
  node.original_name = "sample";
  const auto decoded_node = xrm::Json(node).get<xrm::Node>();
  REQUIRE(decoded_node.upstream_id == "line-4");

  xrm::SubscriptionState state;
  state.last_result = "partial";
  state.last_unsupported_count = 2;
  state.last_malformed_count = 1;
  state.reported_notices = {"剩余流量：1GB"};
  const auto decoded_state = xrm::Json(state).get<xrm::SubscriptionState>();
  REQUIRE(decoded_state.last_result == "partial");
  REQUIRE(decoded_state.last_unsupported_count == 2);
  REQUIRE(decoded_state.last_malformed_count == 1);
  REQUIRE(decoded_state.reported_notices == state.reported_notices);
  REQUIRE_FALSE(decoded_state.last_error);
  state.last_error = "HTTP 503";
  state.last_error_stage = "http";
  const auto failed_state = xrm::Json(state).get<xrm::SubscriptionState>();
  REQUIRE(failed_state.last_error == "HTTP 503");
  REQUIRE(failed_state.last_error_stage == "http");
  xrm::SubscriptionSummary failed_summary = summary;
  failed_summary.last_error = failed_state.last_error;
  failed_summary.last_error_stage = failed_state.last_error_stage;
  const auto public_summary = xrm::Json(failed_summary);
  REQUIRE(public_summary.at("last_error") == "HTTP 503");
  REQUIRE_FALSE(public_summary.contains("url"));
}
