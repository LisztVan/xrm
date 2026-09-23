#include "subscription/reconcile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace {
xrm::ParsedNode vless(std::string name, std::string endpoint, std::string credential,
                      std::optional<std::string> upstream_id = std::nullopt) {
  xrm::StreamNode settings;
  settings.endpoint = {std::move(endpoint), 443};
  settings.protocol.id = std::move(credential);
  settings.protocol.encryption = "none";
  return {std::move(upstream_id), std::move(name), std::move(settings)};
}
}

TEST_CASE("SUB-04 source reconciliation is stable across reordering and display rename") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node first;
  first.id = "node-1";
  first.source_id = source.id;
  first.original_name = "old label";
  first.alias = "my alias";
  first.labels = {"favorite"};
  first.enabled = false;
  first.settings = vless("old label", "one.example", "secret-1").settings;
  xrm::Node second;
  second.id = "node-2";
  second.source_id = source.id;
  second.original_name = "second";
  second.settings = vless("second", "two.example", "secret-2").settings;

  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("renamed", "one.example", "secret-1"),
                  vless("second", "two.example", "secret-2")};
  std::size_t generated = 0;
  auto result = xrm::reconcile({source, {second, first}, parsed}, [&] {
    return "new-" + std::to_string(++generated);
  });
  REQUIRE(result.has_value());
  CHECK(result->report.added == 0);
  CHECK(result->report.updated == 1);
  REQUIRE(result->changes.upserts.size() == 1);
  const auto it = std::find_if(result->changes.upserts.begin(), result->changes.upserts.end(),
                               [](const xrm::Node& node) { return node.id == "node-1"; });
  REQUIRE(it != result->changes.upserts.end());
  CHECK(it->original_name == "renamed");
  CHECK(it->alias == "my alias");
  CHECK(it->labels == std::vector<std::string>{"favorite"});
  CHECK_FALSE(it->enabled);
  CHECK(generated == 0);
}

TEST_CASE("SUB-04 same-name identity changes and cross-source nodes are never merged") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "existing";
  old.source_id = source.id;
  old.original_name = "same";
  old.settings = vless("same", "one.example", "old-secret").settings;
  xrm::Node other_source = old;
  other_source.id = "other-source-node";
  other_source.source_id = "source-B";

  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("same", "one.example", "new-secret")};
  auto result = xrm::reconcile({source, {old, other_source}, parsed}, [] { return "fresh"; });
  REQUIRE(result.has_value());
  REQUIRE(result->changes.upserts.size() == 2);
  const auto fresh = std::find_if(result->changes.upserts.begin(), result->changes.upserts.end(),
                                  [](const xrm::Node& node) { return node.id == "fresh"; });
  REQUIRE(fresh != result->changes.upserts.end());
  CHECK(result->report.added == 1);
  CHECK(result->report.missing == 1);
  CHECK(result->changes.erase_ids.empty());
  CHECK(fresh->source_id == source.id);
}

TEST_CASE("SUB-04 duplicate identities are treated as ambiguous and complete refresh marks only source nodes missing") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old1;
  old1.id = "old-1";
  old1.source_id = source.id;
  old1.original_name = "duplicate";
  old1.settings = vless("duplicate", "same.example", "same-secret").settings;
  xrm::Node old2 = old1;
  old2.id = "old-2";
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("duplicate", "same.example", "same-secret")};
  auto result = xrm::reconcile({source, {old1, old2}, parsed}, [] { return "new"; });
  REQUIRE(result.has_value());
  CHECK(result->report.added == 1);
  CHECK(result->report.missing == 2);
  REQUIRE(result->changes.upserts.size() == 3);
  CHECK(std::count_if(result->changes.upserts.begin(), result->changes.upserts.end(),
                      [](const xrm::Node& node) { return node.source_missing; }) == 2);

  parsed.structurally_complete = false;
  auto partial = xrm::reconcile({source, {old1, old2}, parsed}, [] { return "another"; });
  REQUIRE(partial.has_value());
  CHECK(partial->report.missing == 0);
  CHECK(std::none_of(partial->changes.upserts.begin(), partial->changes.upserts.end(),
                     [](const xrm::Node& node) { return node.source_missing; }));
}
