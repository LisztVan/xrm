#include <catch2/catch_test_macros.hpp>

#include "infra/http.hpp"
#include "infra/store.hpp"
#include "subscription/subscription_service.hpp"

#include <sqlite3.h>
#include <uv.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr std::string_view node_a =
    "vless://00000000-0000-4000-8000-000000000001@127.0.0.1:443?type=tcp&security=none#A";
constexpr std::string_view node_b =
    "vless://00000000-0000-4000-8000-000000000002@127.0.0.1:443?type=tcp&security=none#B";

struct TempDatabase {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-sub-store-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
  }
};

class LocalHttpFixture {
 public:
  LocalHttpFixture() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener_ >= 0);
    int reuse = 1;
    REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    port_ = ntohs(address.sin_port);
    REQUIRE(::listen(listener_, 16) == 0);
    worker_ = std::thread([this] { serve(); });
  }

  ~LocalHttpFixture() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      blocked_ = false;
    }
    condition_.notify_all();
    if (listener_ >= 0) {
      ::shutdown(listener_, SHUT_RDWR);
      ::close(listener_);
      listener_ = -1;
    }
    if (worker_.joinable()) worker_.join();
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/subscription"; }

  void respond(int status, std::string body, std::string extra_headers = {}) {
    std::lock_guard lock(mutex_);
    status_ = status;
    body_ = std::move(body);
    extra_headers_ = std::move(extra_headers);
    blocked_ = false;
    condition_.notify_all();
  }

  void enqueue_response(int status, std::string body, std::string extra_headers = {}) {
    std::lock_guard lock(mutex_);
    responses_.push_back({status, std::move(body), std::move(extra_headers)});
  }

  void block_next() {
    std::lock_guard lock(mutex_);
    blocked_ = true;
    release_blocked_ = false;
  }

  bool wait_for_request(int expected, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] { return requests_ >= expected || stopping_; }) && requests_ >= expected;
  }

  void release() {
    std::lock_guard lock(mutex_);
    release_blocked_ = true;
    condition_.notify_all();
  }

  int request_count() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  bool saw_if_none_match() const {
    std::lock_guard lock(mutex_);
    return saw_if_none_match_;
  }

  int conditional_request_count() const {
    std::lock_guard lock(mutex_);
    return conditional_requests_;
  }

 private:
  void serve() {
    for (;;) {
      int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) return;
      std::string request;
      char buffer[1024];
      while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const auto got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<std::size_t>(got));
      }
      int status;
      std::string body, headers;
      {
        std::unique_lock lock(mutex_);
        ++requests_;
        const auto lowered = [&] {
          std::string value = request;
          for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          return value;
        }();
        if (lowered.find("if-none-match:") != std::string::npos) {
          saw_if_none_match_ = true;
          ++conditional_requests_;
        }
        condition_.notify_all();
        condition_.wait(lock, [&] { return !blocked_ || release_blocked_ || stopping_; });
        if (stopping_) { ::close(client); return; }
        if (!responses_.empty()) {
          status = responses_.front().status;
          body = std::move(responses_.front().body);
          headers = std::move(responses_.front().headers);
          responses_.pop_front();
        } else {
          status = status_;
          body = body_;
          headers = extra_headers_;
        }
        blocked_ = false;
        release_blocked_ = false;
      }
      const char* phrase = status == 200 ? "OK" : status == 304 ? "Not Modified" : "Service Unavailable";
      const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + phrase + "\r\n" +
          "Content-Length: " + std::to_string(body.size()) + "\r\n" + headers +
          "Connection: close\r\n\r\n" + body;
      (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
      ::close(client);
    }
  }

  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  bool blocked_ = false;
  bool release_blocked_ = false;
  bool saw_if_none_match_ = false;
  int requests_ = 0;
  int status_ = 200;
  std::string body_ = std::string(node_a);
  std::string extra_headers_ = "ETag: \"rev-a\"\r\n";
  struct Response { int status; std::string body, headers; };
  std::deque<Response> responses_;
  int conditional_requests_ = 0;
};

struct Loop {
  uv_loop_t value{};
  Loop() { REQUIRE(uv_loop_init(&value) == 0); }
  ~Loop() {
    uv_run(&value, UV_RUN_DEFAULT);
    (void)uv_loop_close(&value);
  }
};

template <class Predicate>
bool pump_until(uv_loop_t& loop, Predicate done, std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(1ms);
  }
  uv_run(&loop, UV_RUN_NOWAIT);
  return done();
}

xrm::Subscription source(std::string id, std::string url) {
  xrm::Subscription value;
  value.id = std::move(id);
  value.name = "fixture";
  value.url = std::move(url);
  value.auto_refresh = false;
  return value;
}

xrm::Result<xrm::BatchRefreshResult> refresh_sync(xrm::SubscriptionService& service,
    uv_loop_t& loop, std::vector<xrm::Id> ids) {
  std::optional<xrm::Result<xrm::BatchRefreshResult>> done;
  auto accepted = service.refresh(std::move(ids), [&](auto result) { done.emplace(std::move(result)); });
  if (!accepted) return std::unexpected(accepted.error());
  if (!pump_until(loop, [&] { return done.has_value(); }))
    return std::unexpected(xrm::AppError{xrm::ErrorCode::timed_out, "fixture refresh timed out", std::nullopt, std::nullopt});
  return std::move(*done);
}

}  // namespace

TEST_CASE("DB-02 source commits atomically and rejects stale settings revisions", "[DB-02]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  auto& store = **opened;
  auto subscription = source("source-A", "http://127.0.0.1/sub");
  REQUIRE(store.put_source(subscription));
  const auto initial = store.get_source("source-A");
  REQUIRE(initial);

  xrm::SourceCommit commit;
  commit.expected_settings_revision = initial->settings_revision;
  commit.state.last_result = "success";
  xrm::Node good;
  good.id = "good";
  good.source_id = "source-A";
  good.upstream_id = "provider-line-14";
  good.original_name = "good";
  good.settings = xrm::RawOutbound{xrm::Json{{"protocol", "freedom"}, {"settings", xrm::Json::object()}}};
  xrm::Node rejected = good;
  rejected.id = "reject";
  rejected.original_name = "reject";
  commit.changes.upserts = {good, rejected};

  sqlite3* sabotage = nullptr;
  REQUIRE(sqlite3_open(database.path.c_str(), &sabotage) == SQLITE_OK);
  char* message = nullptr;
  const auto trigger = "CREATE TRIGGER reject_source_node BEFORE INSERT ON nodes "
      "WHEN NEW.id='reject' BEGIN SELECT RAISE(ABORT,'fixture failure'); END;";
  const int trigger_status = sqlite3_exec(sabotage, trigger, nullptr, nullptr, &message);
  sqlite3_free(message);
  REQUIRE(trigger_status == SQLITE_OK);
  sqlite3_close(sabotage);

  CHECK_FALSE(store.commit_source("source-A", commit));
  CHECK_FALSE(store.get_node("good"));
  const auto state = store.get_source_state("source-A");
  REQUIRE(state);
  CHECK(state->last_result.empty());

  REQUIRE(sqlite3_open(database.path.c_str(), &sabotage) == SQLITE_OK);
  message = nullptr;
  const int drop_status = sqlite3_exec(sabotage, "DROP TRIGGER reject_source_node", nullptr, nullptr, &message);
  sqlite3_free(message);
  REQUIRE(drop_status == SQLITE_OK);
  sqlite3_close(sabotage);
  commit.changes.upserts = {good};
  REQUIRE(store.commit_source("source-A", commit));
  const auto stored_node = store.get_node("good");
  REQUIRE(stored_node);
  CHECK(stored_node->upstream_id == "provider-line-14");

  xrm::Node manual;
  manual.id = "manual-collision";
  manual.original_name = "manual";
  manual.settings = xrm::RawOutbound{xrm::Json{{"protocol", "freedom"}}};
  REQUIRE(store.put_node(manual));
  xrm::Node collision = good;
  collision.id = manual.id;
  collision.original_name = "provider overwrite";
  xrm::SourceCommit collision_commit = commit;
  collision_commit.changes.upserts = {collision};
  CHECK_FALSE(store.commit_source("source-A", collision_commit));
  const auto preserved_manual = store.get_node("manual-collision");
  REQUIRE(preserved_manual);
  CHECK_FALSE(preserved_manual->source_id);
  CHECK(preserved_manual->original_name == "manual");

  xrm::SourceCommit stale = commit;
  stale.changes.upserts.clear();
  auto edited = *initial;
  edited.url = "http://127.0.0.1/new-url";
  REQUIRE(store.put_source(edited));
  const auto changed_source = store.get_source("source-A");
  REQUIRE(changed_source);
  CHECK(changed_source->settings_revision > initial->settings_revision);
  CHECK_FALSE(store.commit_source("source-A", stale));
  CHECK(store.get_node("good"));
}

TEST_CASE("SUB-02 refresh updates inventory only and same-source requests coalesce", "[SUB-02][SUB-06]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  LocalHttpFixture fixture;
  REQUIRE((*opened)->put_source(source("source-A", fixture.url())));
  Loop loop;
  xrm::HttpClient http(loop.value);
  xrm::SubscriptionService service(**opened, http);
  int first_done = 0, second_done = 0;
  bool callbacks_ok = true;
  REQUIRE(service.refresh({"source-A"}, [&](auto result) { callbacks_ok = callbacks_ok && result.has_value(); ++first_done; }));
  REQUIRE(service.refresh({"source-A"}, [&](auto result) { callbacks_ok = callbacks_ok && result.has_value(); ++second_done; }));
  REQUIRE(fixture.wait_for_request(1));
  REQUIRE(pump_until(loop.value, [&] { return first_done == 1 && second_done == 1; }));
  CHECK(fixture.request_count() == 1);
  CHECK(callbacks_ok);
  const auto first = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "source-A"}, {0, 20});
  REQUIRE(first);
  REQUIRE(first->items.size() == 1);
  CHECK(first->items.front().original_name == "A");
  CHECK(first->items.front().source_id == "source-A");
  const auto state = (*opened)->get_source_state("source-A");
  REQUIRE(state);
  CHECK(state->last_result == "success");
  CHECK(state->etag == "\"rev-a\"");
  service.stop_schedule();
  http.shutdown([] {});
}

TEST_CASE("SUB-04 identical HTTP payload is idempotent across ten refreshes and a service restart", "[SUB-04]") {
  TempDatabase database;
  LocalHttpFixture fixture;
  fixture.respond(200, std::string(node_a) + "\n" + std::string(node_a) + "\n" +
                       std::string(node_b), "");
  {
    auto store = xrm::Store::open(database.path);
    REQUIRE(store);
    REQUIRE((*store)->put_source(source("source-A", fixture.url())));
  }

  std::vector<xrm::Id> first_ids;
  for (int session = 0; session < 2; ++session) {
    auto store = xrm::Store::open(database.path);
    REQUIRE(store);
    Loop loop;
    xrm::HttpClient http(loop.value);
    xrm::SubscriptionService service(**store, http);
    for (int attempt = session * 5; attempt < (session + 1) * 5; ++attempt) {
      const auto refresh = refresh_sync(service, loop.value, {"source-A"});
      REQUIRE(refresh);
      REQUIRE(refresh->failed.empty());
      REQUIRE(refresh->completed.size() == 1);
      const auto& report = refresh->completed.front();
      CHECK(report.added == (attempt == 0 ? 2 : 0));
      CHECK(report.updated == 0);
      CHECK(report.missing == 0);
      CHECK_FALSE(report.partial);

      const auto inventory = (*store)->list_nodes(
          xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
      REQUIRE(inventory);
      REQUIRE(inventory->total == 2);
      REQUIRE(inventory->items.size() == 2);
      std::vector<xrm::Id> ids;
      for (const auto& node : inventory->items) {
        CHECK_FALSE(node.source_missing);
        ids.push_back(node.id);
      }
      std::sort(ids.begin(), ids.end());
      if (attempt == 0) {
        first_ids = ids;
        const auto original_a = std::find_if(inventory->items.begin(), inventory->items.end(),
            [](const auto& node) { return node.original_name == "A"; });
        REQUIRE(original_a != inventory->items.end());
        auto user_edited = *original_a;
        user_edited.alias = "My A";
        user_edited.labels = {"favorite"};
        user_edited.enabled = false;
        REQUIRE((*store)->put_node(user_edited));
        // A historical duplicate removed through the same scoped Store transaction
        // must not be recreated by later identical refreshes or service restart.
        const auto original_b = std::find_if(inventory->items.begin(), inventory->items.end(),
            [](const auto& node) { return node.original_name == "B"; });
        REQUIRE(original_b != inventory->items.end());
        auto redundant_history = *original_b;
        redundant_history.id = "old-duplicate-B";
        redundant_history.source_missing = true;
        REQUIRE((*store)->put_node(redundant_history));
        const auto expected_state = (*store)->get_source_state("source-A");
        REQUIRE(expected_state);
        const auto before_cleanup = (*store)->list_nodes(
            xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
        REQUIRE(before_cleanup);
        REQUIRE(before_cleanup->total == 3);
        REQUIRE((*store)->prune_history("source-A", *expected_state, before_cleanup->items,
                                        {redundant_history.id}));
        const auto cleaned = (*store)->list_nodes(
            xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
        REQUIRE(cleaned);
        CHECK(cleaned->total == 2);
      } else {
        CHECK(ids == first_ids);
        const auto edited_a = std::find_if(inventory->items.begin(), inventory->items.end(),
            [](const auto& node) { return node.original_name == "A"; });
        REQUIRE(edited_a != inventory->items.end());
        CHECK(edited_a->alias == "My A");
        CHECK(edited_a->labels == std::vector<std::string>{"favorite"});
        CHECK_FALSE(edited_a->enabled);
      }
    }
    service.stop_schedule();
    http.shutdown([] {});
  }
  CHECK(fixture.request_count() == 10);
}

TEST_CASE("SUB-03 partial input updates valid nodes, while empty, unsupported-only and HTML retain inventory", "[SUB-03]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  LocalHttpFixture fixture;
  REQUIRE((*opened)->put_source(source("source-A", fixture.url())));
  Loop loop;
  xrm::HttpClient http(loop.value);
  xrm::SubscriptionService service(**opened, http);
  auto first = refresh_sync(service, loop.value, {"source-A"});
  REQUIRE(first);
  REQUIRE(first->completed.size() == 1);
  const auto old = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
  REQUIRE(old);
  REQUIRE(old->items.size() == 1);
  const auto old_id = old->items.front().id;

  fixture.respond(200, std::string(node_b) + "\nnot-a-node\nsocks://opaque");
  auto partial = refresh_sync(service, loop.value, {"source-A"});
  REQUIRE(partial);
  REQUIRE(partial->completed.size() == 1);
  CHECK(partial->completed.front().partial);
  CHECK(partial->completed.front().malformed == 1);
  CHECK(partial->completed.front().unsupported == 1);
  const auto partial_state = (*opened)->get_source_state("source-A");
  REQUIRE(partial_state);
  CHECK(partial_state->last_result == "partial");
  CHECK(partial_state->last_malformed_count == 1);
  CHECK(partial_state->last_unsupported_count == 1);
  auto after_partial = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
  REQUIRE(after_partial);
  CHECK(std::any_of(after_partial->items.begin(), after_partial->items.end(),
                    [&](const xrm::Node& node) { return node.id == old_id && !node.source_missing; }));

  for (const auto& body : {std::string(""), std::string("socks://opaque"),
                           std::string("<!doctype html><html>login</html>")}) {
    fixture.respond(200, body);
    auto rejected = refresh_sync(service, loop.value, {"source-A"});
    REQUIRE(rejected);
    REQUIRE(rejected->completed.empty());
    REQUIRE(rejected->failed.size() == 1);
    auto retained = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
    REQUIRE(retained);
    CHECK(std::any_of(retained->items.begin(), retained->items.end(),
                      [&](const xrm::Node& node) { return node.id == old_id && !node.source_missing; }));
  }
  fixture.respond(200,
      "vless://00000000-0000-4000-8000-000000000009@node.example:443?security=tls&insecure=maybe");
  auto malformed_vless = refresh_sync(service, loop.value, {"source-A"});
  REQUIRE(malformed_vless);
  REQUIRE(malformed_vless->failed.size() == 1);
  CHECK(malformed_vless->failed.front().second.message.find("insecure must be true, false, 1 or 0") !=
        std::string::npos);
  CHECK(malformed_vless->failed.front().second.message.find("node.example") == std::string::npos);
  CHECK(malformed_vless->failed.front().second.message.find("maybe") == std::string::npos);
  const auto retained_after_malformed = (*opened)->list_nodes(
      xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
  REQUIRE(retained_after_malformed);
  CHECK(std::any_of(retained_after_malformed->items.begin(), retained_after_malformed->items.end(),
                    [&](const xrm::Node& node) { return node.id == old_id && !node.source_missing; }));

  fixture.respond(503, "upstream unavailable");
  auto unavailable = refresh_sync(service, loop.value, {"source-A"});
  REQUIRE(unavailable);
  REQUIRE(unavailable->completed.empty());
  REQUIRE(unavailable->failed.size() == 1);
  auto retained_after_http_failure = (*opened)->list_nodes(
      xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
  REQUIRE(retained_after_http_failure);
  CHECK(std::any_of(retained_after_http_failure->items.begin(), retained_after_http_failure->items.end(),
                    [&](const xrm::Node& node) { return node.id == old_id && !node.source_missing; }));
  service.stop_schedule();
  http.shutdown([] {});
}

TEST_CASE("SUB-05 an old response cannot commit after its source changes or is deleted", "[SUB-05]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  LocalHttpFixture fixture;
  REQUIRE((*opened)->put_source(source("source-A", fixture.url())));
  Loop loop;
  xrm::HttpClient http(loop.value);
  xrm::SubscriptionService service(**opened, http);
  int done = 0;
  std::optional<xrm::Result<xrm::BatchRefreshResult>> result;
  fixture.block_next();
  REQUIRE(service.refresh({"source-A"}, [&](auto value) { result.emplace(std::move(value)); ++done; }));
  REQUIRE(fixture.wait_for_request(1));
  auto edited = (*opened)->get_source("source-A");
  REQUIRE(edited);
  edited->url = fixture.url() + "?revision=2";
  REQUIRE((*opened)->put_source(*edited));
  fixture.release();
  REQUIRE(pump_until(loop.value, [&] { return done == 1; }));
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((*result)->completed.empty());
  CHECK((*result)->failed.size() == 1);
  auto no_nodes = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "source-A", .include_missing = true}, {0, 20});
  REQUIRE(no_nodes);
  CHECK(no_nodes->items.empty());

  fixture.block_next();
  result.reset();
  done = 0;
  REQUIRE(service.refresh({"source-A"}, [&](auto value) { result.emplace(std::move(value)); ++done; }));
  REQUIRE(fixture.wait_for_request(2));
  REQUIRE((*opened)->remove_source("source-A", false));
  fixture.release();
  REQUIRE(pump_until(loop.value, [&] { return done == 1; }));
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((*result)->failed.size() == 1);
  CHECK_FALSE((*opened)->get_source("source-A"));
  service.stop_schedule();
  http.shutdown([] {});
}

TEST_CASE("SUB-06 timers honor pause and refresh only node inventory", "[SUB-06]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  LocalHttpFixture fixture;
  auto paused = source("paused", fixture.url());
  paused.auto_refresh = false;
  paused.interval = 1s;
  auto scheduled = source("scheduled", fixture.url());
  scheduled.auto_refresh = true;
  scheduled.interval = 1s;
  REQUIRE((*opened)->put_source(paused));
  REQUIRE((*opened)->put_source(scheduled));
  Loop loop;
  xrm::HttpClient http(loop.value);
  xrm::SubscriptionService service(**opened, http);
  service.start_schedule(loop.value);
  REQUIRE(pump_until(loop.value, [&] { return fixture.request_count() >= 1; }, 3s));
  REQUIRE(pump_until(loop.value, [&] {
    auto nodes = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "scheduled"}, {0, 10});
    return nodes && !nodes->items.empty();
  }, 3s));
  CHECK(fixture.request_count() == 1);
  const auto paused_nodes = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "paused"}, {0, 10});
  REQUIRE(paused_nodes);
  CHECK(paused_nodes->items.empty());
  service.stop_schedule();
  http.shutdown([] {});
}

TEST_CASE("SUB-06 a cached 304 is not modified; an uncached 304 gets one unconditional retry", "[SUB-06]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  LocalHttpFixture fixture;
  REQUIRE((*opened)->put_source(source("cached", fixture.url())));
  Loop loop;
  xrm::HttpClient http(loop.value);
  xrm::SubscriptionService service(**opened, http);
  REQUIRE(refresh_sync(service, loop.value, {"cached"}));
  fixture.respond(304, "");
  auto cached = refresh_sync(service, loop.value, {"cached"});
  REQUIRE(cached);
  REQUIRE(cached->completed.size() == 1);
  CHECK(cached->completed.front().changed == false);
  CHECK((*opened)->get_source_state("cached")->last_result == "not_modified");
  CHECK(fixture.saw_if_none_match());

  REQUIRE((*opened)->put_source(source("uncached", fixture.url())));
  fixture.enqueue_response(304, "");
  fixture.enqueue_response(200, std::string(node_b));
  const auto before = fixture.request_count();
  const auto conditional_before = fixture.conditional_request_count();
  // The first 304 is deliberately not a cache hit; the service retries without
  // conditional request headers exactly once.
  std::optional<xrm::Result<xrm::BatchRefreshResult>> result;
  int done = 0;
  REQUIRE(service.refresh({"uncached"}, [&](auto value) { result.emplace(std::move(value)); ++done; }));
  REQUIRE(fixture.wait_for_request(before + 1));
  REQUIRE(pump_until(loop.value, [&] { return done == 1; }));
  REQUIRE(result);
  REQUIRE(*result);
  REQUIRE((*result)->completed.size() == 1);
  CHECK(fixture.request_count() == before + 2);
  CHECK(fixture.conditional_request_count() == conditional_before);
  service.stop_schedule();
  http.shutdown([] {});
}

TEST_CASE("SUB-06 batch refresh returns per-source success and failure", "[SUB-06]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path);
  REQUIRE(opened);
  LocalHttpFixture good_fixture;
  LocalHttpFixture bad_fixture;
  bad_fixture.respond(503, "offline");
  REQUIRE((*opened)->put_source(source("good-source", good_fixture.url())));
  REQUIRE((*opened)->put_source(source("bad-source", bad_fixture.url())));
  Loop loop;
  xrm::HttpClient http(loop.value);
  xrm::SubscriptionService service(**opened, http);
  auto result = refresh_sync(service, loop.value, {"good-source", "bad-source"});
  REQUIRE(result);
  REQUIRE(result->completed.size() == 1);
  CHECK(result->completed.front().source_id == "good-source");
  REQUIRE(result->failed.size() == 1);
  CHECK(result->failed.front().first == "bad-source");
  const auto good_nodes = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "good-source"}, {0, 10});
  const auto bad_nodes = (*opened)->list_nodes(xrm::NodeQuery{.source_id = "bad-source"}, {0, 10});
  REQUIRE(good_nodes);
  REQUIRE(bad_nodes);
  CHECK(good_nodes->items.size() == 1);
  CHECK(bad_nodes->items.empty());
  http.shutdown([] {});
}
