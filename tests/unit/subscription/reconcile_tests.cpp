#include "subscription/reconcile.hpp"
#include "subscription/parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace {
xrm::ParsedNode vless(std::string name, std::string endpoint, std::string credential,
                      std::optional<std::string> upstream_id = std::nullopt) {
  xrm::StreamNode settings;
  settings.endpoint = {std::move(endpoint), 443};
  auto& protocol = std::get<xrm::VlessSettings>(settings.protocol);
  protocol.id = std::move(credential);
  protocol.encryption = "none";
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
  REQUIRE(result->changes.upserts.size() == 1);
  const auto fresh = std::find_if(result->changes.upserts.begin(), result->changes.upserts.end(),
                                  [](const xrm::Node& node) { return node.id == "fresh"; });
  REQUIRE(fresh != result->changes.upserts.end());
  CHECK(result->report.added == 1);
  CHECK(result->report.missing == 1);
  CHECK(result->changes.erase_ids == std::vector<xrm::Id>{"existing"});
  CHECK(fresh->source_id == source.id);
}

TEST_CASE("SUB-04 equivalent entries and historical duplicate records do not generate new identities") {
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
  CHECK(result->report.added == 0);
  CHECK(result->report.updated == 0);
  CHECK(result->report.missing == 1);
  CHECK(result->changes.upserts.empty());
  CHECK(result->changes.erase_ids == std::vector<xrm::Id>{"old-2"});

  parsed.structurally_complete = false;
  auto partial = xrm::reconcile({source, {old1, old2}, parsed}, [] { return "another"; });
  REQUIRE(partial.has_value());
  CHECK(partial->report.missing == 0);
  CHECK(std::none_of(partial->changes.upserts.begin(), partial->changes.upserts.end(),
                     [](const xrm::Node& node) { return node.source_missing; }));
}

TEST_CASE("SUB-04 a repeated source payload allocates one ID and is stable for ten refreshes") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {
      vless("A", "same.example", "same-secret"),
      vless("A", "same.example", "same-secret"),
      vless("unique", "other.example", "other-secret")};
  std::vector<xrm::Node> stored;
  std::size_t generated = 0;
  for (int iteration = 0; iteration < 10; ++iteration) {
    std::swap(parsed.nodes[0], parsed.nodes[1]);
    auto result = xrm::reconcile({source, stored, parsed}, [&] {
      return "id-" + std::to_string(++generated);
    });
    REQUIRE(result);
    CHECK(result->report.added == (iteration == 0 ? 2 : 0));
    CHECK(result->report.updated == 0);
    CHECK(result->report.missing == 0);
    for (auto& node : result->changes.upserts) {
      const auto found = std::find_if(stored.begin(), stored.end(),
                                    [&](const auto& old) { return old.id == node.id; });
      if (found == stored.end()) stored.push_back(node);
      else *found = node;
    }
    REQUIRE(stored.size() == 2);
    CHECK(generated == 2);
  }
  CHECK(std::any_of(stored.begin(), stored.end(), [](const auto& node) {
    return node.original_name == "A";
  }));
}

TEST_CASE("SUB-04 a source with repeated connection rows retains one identity across ten refreshes") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {
      vless("剩余流量", "info.example", "shared-secret"),
      vless("套餐到期", "info.example", "shared-secret"),
      vless("官网", "info.example", "shared-secret"),
      vless("公告", "info.example", "shared-secret")};
  for (int i = 0; i < 45; ++i)
    parsed.nodes.push_back(vless("exit-" + std::to_string(i),
                                 "exit-" + std::to_string(i) + ".example", "shared-secret"));
  std::vector<xrm::Node> stored;
  std::vector<xrm::Id> first_ids;
  std::size_t generated = 0;
  for (int iteration = 0; iteration < 10; ++iteration) {
    std::rotate(parsed.nodes.begin(), parsed.nodes.begin() + 1, parsed.nodes.end());
    auto result = xrm::reconcile({source, stored, parsed}, [&] {
      return "info-" + std::to_string(++generated);
    });
    REQUIRE(result);
    CHECK(result->report.added == (iteration == 0 ? 46 : 0));
    CHECK(result->report.updated == 0);
    CHECK(result->report.missing == 0);
    for (const auto& node : result->changes.upserts) {
      const auto found = std::find_if(stored.begin(), stored.end(),
                                    [&](const auto& old) { return old.id == node.id; });
      if (found == stored.end()) stored.push_back(node);
      else *found = node;
    }
    REQUIRE(stored.size() == 46);
    std::vector<xrm::Id> ids;
    for (const auto& node : stored) ids.push_back(node.id);
    std::sort(ids.begin(), ids.end());
    if (iteration == 0) first_ids = ids;
    else CHECK(ids == first_ids);
  }
  CHECK(generated == 46);
}

TEST_CASE("SUB-04 equivalent missing history rows retain one and remove redundant records") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {
      vless("剩余流量", "info.example", "shared-secret"),
      vless("套餐到期", "info.example", "shared-secret"),
      vless("官网", "info.example", "shared-secret"),
      vless("公告", "info.example", "shared-secret")};
  std::vector<xrm::Node> old_nodes;
  for (std::size_t i = 0; i < parsed.nodes.size(); ++i) {
    xrm::Node old;
    old.id = "old-" + std::to_string(i);
    old.source_id = source.id;
    old.original_name = parsed.nodes[i].name;
    old.settings = parsed.nodes[i].settings;
    old.source_missing = true;
    old_nodes.push_back(std::move(old));
  }
  auto result = xrm::reconcile({source, old_nodes, parsed}, [] { return "wrong-new-id"; });
  REQUIRE(result);
  CHECK(result->report.added == 0);
  CHECK(result->report.updated == 1);
  CHECK(result->report.missing == 3);
  REQUIRE(result->changes.upserts.size() == 1);
  CHECK(result->changes.upserts.front().id == "old-0");
  CHECK_FALSE(result->changes.upserts.front().source_missing);
  CHECK(result->changes.erase_ids.size() == 3);
}

TEST_CASE("SUB-04 exact source-missing identity is restored instead of replaced") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "selected-node";
  old.source_id = source.id;
  old.original_name = "before";
  old.alias = "my alias";
  old.labels = {"favorite"};
  old.enabled = false;
  old.source_missing = true;
  old.settings = vless("before", "same.example", "same-secret").settings;
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("after", "same.example", "same-secret")};
  auto result = xrm::reconcile({source, {old}, parsed}, [] { return "replacement"; });
  REQUIRE(result);
  CHECK(result->report.added == 0);
  CHECK(result->report.updated == 1);
  REQUIRE(result->changes.upserts.size() == 1);
  CHECK(result->changes.upserts.front().id == old.id);
  CHECK_FALSE(result->changes.upserts.front().source_missing);
  CHECK(result->changes.upserts.front().alias == old.alias);
  CHECK(result->changes.upserts.front().labels == old.labels);
  CHECK_FALSE(result->changes.upserts.front().enabled);
}

TEST_CASE("SUB-04 URI syntax changes do not change canonical connection identity") {
  xrm::Subscription source;
  source.id = "source-A";
  const auto old_report = xrm::parse_subscription(
      "vless://00000000-0000-4000-8000-000000000001@same.example:443?"
      "security=none&type=tcp&encryption=none#Old");
  REQUIRE(old_report);
  REQUIRE(old_report->nodes.size() == 1);
  xrm::Node old;
  old.id = "stable-id";
  old.source_id = source.id;
  old.original_name = "Old";
  old.settings = old_report->nodes.front().settings;

  const auto same_report = xrm::parse_subscription(
      "vless://00000000-0000-4000-8000-000000000001@same.example:443?"
      "type=raw&security=none#Old");
  REQUIRE(same_report);
  auto result = xrm::reconcile({source, {old}, *same_report}, [] { return "new-id"; });
  REQUIRE(result);
  CHECK(result->report.added == 0);
  CHECK(result->report.updated == 0);
  CHECK(result->report.missing == 0);
  CHECK(result->changes.upserts.empty());
}

TEST_CASE("SUB-04 a shared credential or upstream row does not merge distinct exits") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "old-exit";
  old.source_id = source.id;
  old.upstream_id = "provider-row";
  old.original_name = "same name";
  old.settings = vless("same name", "exit-a.example", "shared-uuid").settings;
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {
      vless("same name", "exit-b.example", "shared-uuid", "provider-row"),
      vless("same name", "exit-a.example", "shared-uuid", "provider-row")};
  auto result = xrm::reconcile({source, {old}, parsed}, [] { return "new-exit"; });
  REQUIRE(result);
  CHECK(result->report.added == 1);
  CHECK(result->report.updated == 0);
  CHECK(result->report.missing == 0);
  REQUIRE(result->changes.upserts.size() == 1);
  CHECK(result->changes.upserts.front().id == "new-exit");
  CHECK(std::get<xrm::StreamNode>(result->changes.upserts.front().settings).endpoint.address ==
        "exit-b.example");
}

TEST_CASE("SUB-03 partial parse accepts valid entries without marking absent nodes missing") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "old-node";
  old.source_id = source.id;
  old.original_name = "no longer listed";
  old.settings = vless("no longer listed", "old.example", "old-secret").settings;

  const auto parsed = xrm::parse_subscription(
      "vless://00000000-0000-4000-8000-000000000001@new.example:443#new\n"
      "socks://opaque\n"
      "vless://malformed");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->nodes.size() == 1);
  REQUIRE(parsed->issues.size() == 2);
  CHECK_FALSE(parsed->structurally_complete);

  auto result = xrm::reconcile({source, {old}, *parsed}, [] { return "new-node"; });
  REQUIRE(result.has_value());
  CHECK(result->report.added == 1);
  CHECK(result->report.malformed == 1);
  CHECK(result->report.unsupported == 1);
  CHECK(result->report.partial);
  CHECK(result->report.missing == 0);
  REQUIRE(result->changes.upserts.size() == 1);
  CHECK(result->changes.upserts.front().id == "new-node");

  const auto html = xrm::parse_subscription("<html><form>login</form></html>");
  REQUIRE(html.has_value());
  CHECK(html->nodes.empty());
  auto empty = xrm::reconcile({source, {old}, *html}, [] { return "unused"; });
  REQUIRE(empty.has_value());
  CHECK(empty->changes.upserts.empty());
  CHECK(empty->report.missing == 0);
}

TEST_CASE("SUB-04 upstream row ID cannot overwrite changed connection identity") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "stable-id";
  old.source_id = source.id;
  old.upstream_id = "source-row-7";
  old.original_name = "before";
  old.settings = vless("before", "old.example", "old-secret").settings;

  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("after", "new.example", "new-secret", "source-row-7")};
  auto result = xrm::reconcile({source, {old}, parsed}, [] { return "unexpected-new"; });
  REQUIRE(result.has_value());
  CHECK(result->report.added == 1);
  CHECK(result->report.updated == 0);
  CHECK(result->report.missing == 1);
  REQUIRE(result->changes.upserts.size() == 1);
  CHECK(result->changes.upserts.front().id == "unexpected-new");
  CHECK(result->changes.erase_ids == std::vector<xrm::Id>{old.id});
  CHECK(result->changes.upserts.front().upstream_id == old.upstream_id);
}

TEST_CASE("SUB-04 same endpoint and credential with changed transport or security is not the same connection identity") {
  xrm::Subscription source;
  source.id = "source-A";
  for (const auto [change_transport, change_security] :
       std::array<std::pair<bool, bool>, 2>{{{true, false}, {false, true}}}) {
    xrm::Node old;
    old.id = "old-vless";
    old.source_id = source.id;
    old.original_name = "same name";
    auto old_settings = vless("same name", "same.example", "same-credential").settings;
    auto& old_stream = std::get<xrm::StreamNode>(old_settings);
    old_stream.transport = xrm::WebSocketTransport{"/old-path", {}};
    xrm::TlsSecurity old_tls;
    old_tls.server_name = "old-sni.example";
    old_stream.security = old_tls;
    old.settings = old_settings;

    auto changed = vless("same name", "same.example", "same-credential");
    auto& changed_stream = std::get<xrm::StreamNode>(changed.settings);
    changed_stream.transport = xrm::WebSocketTransport{
        change_transport ? "/new-path" : "/old-path", {}};
    xrm::TlsSecurity changed_tls;
    changed_tls.server_name = change_security ? "new-sni.example" : "old-sni.example";
    changed_stream.security = changed_tls;
    xrm::ParseReport parsed;
    parsed.structurally_complete = true;
    parsed.nodes = {std::move(changed)};

    auto result = xrm::reconcile({source, {old}, parsed}, [] { return "new-vless"; });
    REQUIRE(result.has_value());
    CHECK(result->report.added == 1);
    CHECK(result->report.missing == 1);
    REQUIRE(result->changes.upserts.size() == 1);
    CHECK(std::any_of(result->changes.upserts.begin(), result->changes.upserts.end(),
                      [](const xrm::Node& node) { return node.id == "new-vless"; }));
    CHECK(result->changes.erase_ids == std::vector<xrm::Id>{"old-vless"});
  }
}

TEST_CASE("P2 complete refresh physically removes absent source rows; partial refresh does not") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "old-a";
  old.source_id = source.id;
  old.original_name = "A";
  old.settings = vless("A", "a.example", "secret-a").settings;
  xrm::ParseReport replacement;
  replacement.structurally_complete = true;
  replacement.nodes = {vless("B", "b.example", "secret-b")};
  auto complete = xrm::reconcile({source, {old}, replacement}, [] { return "new-b"; });
  REQUIRE(complete);
  CHECK(complete->report.missing == 1);
  CHECK(complete->changes.erase_ids == std::vector<xrm::Id>{"old-a"});
  CHECK(std::none_of(complete->changes.upserts.begin(), complete->changes.upserts.end(),
                     [](const xrm::Node& node) { return node.source_missing; }));

  replacement.issues.push_back({1, xrm::IssueKind::malformed, "bad", "bad row"});
  auto partial = xrm::reconcile({source, {old}, replacement}, [] { return "new-b"; });
  REQUIRE(partial);
  CHECK(partial->report.missing == 0);
  CHECK(partial->changes.erase_ids.empty());
}

TEST_CASE("P2 equivalent source rows with different names collapse to one connection") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("First", "same.example", "same-secret"),
                  vless("Renamed", "same.example", "same-secret")};
  std::size_t created = 0;
  auto first = xrm::reconcile({source, {}, parsed}, [&] { return "id-" + std::to_string(++created); });
  REQUIRE(first);
  CHECK(first->report.added == 1);
  REQUIRE(first->changes.upserts.size() == 1);
  CHECK(created == 1);
  auto second = xrm::reconcile({source, first->changes.upserts, parsed},
                               [&] { return "id-" + std::to_string(++created); });
  REQUIRE(second);
  CHECK(second->report.added == 0);
  CHECK(second->report.updated == 0);
  CHECK(second->changes.erase_ids.empty());
  CHECK(created == 1);
}

TEST_CASE("P2 raw outbound display tag does not trigger a connection update") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node old;
  old.id = "stable-raw";
  old.source_id = source.id;
  old.original_name = "Current";
  old.settings = xrm::RawOutbound{xrm::Json{{"tag", "provider-old"},
      {"protocol", "freedom"}, {"settings", xrm::Json::object()}}};
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes.push_back({std::nullopt, "Current", xrm::RawOutbound{xrm::Json{
      {"tag", "provider-new"}, {"protocol", "freedom"},
      {"settings", xrm::Json::object()}}}});
  auto result = xrm::reconcile({source, {old}, parsed}, [] { return "unexpected-id"; });
  REQUIRE(result);
  CHECK(result->report.added == 0);
  CHECK(result->report.updated == 0);
  CHECK(result->changes.upserts.empty());
}

TEST_CASE("P2 equivalent historical rows with conflicting user attributes require review") {
  xrm::Subscription source;
  source.id = "source-A";
  xrm::Node first;
  first.id = "old-a";
  first.source_id = source.id;
  first.original_name = "A";
  first.alias = "My A";
  first.settings = vless("A", "same.example", "secret").settings;
  auto second = first;
  second.id = "old-b";
  second.alias = "My B";
  xrm::ParseReport parsed;
  parsed.structurally_complete = true;
  parsed.nodes = {vless("A", "same.example", "secret")};
  auto result = xrm::reconcile({source, {first, second}, parsed}, [] { return "new"; });
  REQUIRE(result);
  CHECK(result->report.added == 0);
  CHECK(result->changes.erase_ids.empty());
  CHECK(std::any_of(result->report.notices.begin(), result->report.notices.end(),
      [](const xrm::Notice& notice) { return notice.code == "duplicate_conflict"; }));
}
