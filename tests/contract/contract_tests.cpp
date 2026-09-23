#include <catch2/catch_test_macros.hpp>

#include "client/rpc_client.hpp"
#include "contract/dtos.hpp"
#include "contract/wire.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
#include "contract/methods.hpp"
#include "model/profile.hpp"
#include "server/rpc_server.hpp"
#include "xray/runtime_assembler.hpp"

TEST_CASE("C0 headers are independently usable", "[CT-01]") {
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::Node>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::CoreInstall>);
  STATIC_REQUIRE(std::is_default_constructible_v<xrm::RpcRequest>);
  STATIC_REQUIRE(std::is_move_constructible_v<xrm::Store> == false);
}

TEST_CASE("C0 DTOs and errors have stable JSON representations", "[CT-02]") {
  const xrm::NodeSummary summary{"node-1", "example", {"test"}, true, false, "vless"};
  const auto json = xrm::Json(summary);
  REQUIRE(json.at("id") == "node-1");
  REQUIRE(json.at("name") == "example");
  REQUIRE_FALSE(json.contains("password"));
  REQUIRE(json.get<xrm::NodeSummary>().protocol == "vless");

  const xrm::AppError source{xrm::ErrorCode::conflict, "changed", "policy", "reload"};
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
                                   std::declval<const xrm::BuildInput&>())),
                               xrm::Result<xrm::PreparedFiles>>);
  REQUIRE(xrm::methods::profile_create == "profile.create");
  REQUIRE(xrm::methods::module_list == "module.list");
  REQUIRE(xrm::methods::connection_connect == "connection.connect");
  REQUIRE(xrm::methods::core_stop == "core.stop");
}
