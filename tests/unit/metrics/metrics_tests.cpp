#include "xray/metrics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <uv.h>

using namespace xrm;

TEST_CASE("metrics parser preserves absence and parses Xray expvar counters and observations",
          "[unit][MET-01]") {
  const Json source = {
      {"stats", {{"outbound", {{"proxy", {{"uplink", 120}, {"downlink", 400}}}}}}},
      {"observatory", {{"proxy", {{"alive", true}, {"delay", 18}}},
                       {"pending", {{"alive", false}}}}}};
  auto parsed = parse_metrics(source, "run-a", 1000);
  REQUIRE(parsed);
  REQUIRE(parsed->traffic.size() == 1);
  CHECK(parsed->traffic[0].scope == "outbound");
  CHECK(parsed->traffic[0].tag == "proxy");
  CHECK(parsed->traffic[0].uplink == 120);
  REQUIRE(parsed->observations.size() == 2);
  const auto proxy = std::find_if(parsed->observations.begin(), parsed->observations.end(),
                                  [](const auto& value) { return value.tag == "proxy"; });
  const auto pending = std::find_if(parsed->observations.begin(), parsed->observations.end(),
                                    [](const auto& value) { return value.tag == "pending"; });
  REQUIRE(proxy != parsed->observations.end());
  REQUIRE(pending != parsed->observations.end());
  CHECK(proxy->latency_ms == 18.0);
  CHECK_FALSE(pending->latency_ms.has_value());

  const Json absent = {{"stats", {{"outbound", {{"proxy", {{"uplink", nullptr}}}}}}}};
  auto missing = parse_metrics(absent, "run-a", 2000);
  REQUIRE(missing);
  REQUIRE(missing->traffic.size() == 1);
  CHECK_FALSE(missing->traffic[0].uplink.has_value());
}

TEST_CASE("traffic rates reject reset counters, run changes, and nonpositive elapsed",
          "[unit][MET-01]") {
  MetricsSample before{"run-a", 1000, {{"outbound", "p", 100, 100}}, {}};
  MetricsSample after{"run-a", 3000, {{"outbound", "p", 160, 80}}, {}};
  auto rates = compute_rates(before, after, std::chrono::seconds(2));
  REQUIRE(rates.size() == 1);
  CHECK(rates[0].uplink_bytes_per_second == 30.0);
  CHECK_FALSE(rates[0].downlink_bytes_per_second.has_value());

  after.run_id = "run-b";
  CHECK_FALSE(compute_rates(before, after, std::chrono::seconds(2))[0]
                  .uplink_bytes_per_second.has_value());
  after.run_id = "run-a";
  CHECK_FALSE(compute_rates(before, after, std::chrono::seconds(0))[0]
                  .uplink_bytes_per_second.has_value());
}

namespace {
struct LocalMetricsHttp {
  struct Client {
    uv_tcp_t stream{};
    LocalMetricsHttp* owner = nullptr;
    std::string request;
  };
  struct WriteRequest {
    uv_write_t request{};
    Client* client = nullptr;
    std::string response;
  };
  struct DelayedWrite {
    uv_timer_t timer{};
    WriteRequest* write = nullptr;
  };

  explicit LocalMetricsHttp(uv_loop_t& loop) : loop(loop) {
    uv_tcp_init(&loop, &server);
    server.data = this;
    sockaddr_in address{};
    uv_ip4_addr("127.0.0.1", 0, &address);
    uv_tcp_bind(&server, reinterpret_cast<const sockaddr*>(&address), 0);
    uv_listen(reinterpret_cast<uv_stream_t*>(&server), 8, on_connection);
    sockaddr_in actual{};
    int length = sizeof(actual);
    uv_tcp_getsockname(&server, reinterpret_cast<sockaddr*>(&actual), &length);
    port = ntohs(actual.sin_port);
  }

  static void alloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buffer) {
    const auto size = std::max<std::size_t>(suggested, 1024);
    buffer->base = static_cast<char*>(std::malloc(size));
    buffer->len = size;
  }

  static void close_client(uv_handle_t* handle) {
    delete static_cast<Client*>(handle->data);
  }

  static void on_written(uv_write_t* request, int) {
    auto* write = static_cast<WriteRequest*>(request->data);
    uv_close(reinterpret_cast<uv_handle_t*>(&write->client->stream), close_client);
    delete write;
  }

  static void on_delay_closed(uv_handle_t* handle) {
    delete static_cast<DelayedWrite*>(handle->data);
  }

  static void on_delay(uv_timer_t* timer) {
    auto* delayed = static_cast<DelayedWrite*>(timer->data);
    auto* write = delayed->write;
    auto* client = write->client;
    uv_close(reinterpret_cast<uv_handle_t*>(timer), on_delay_closed);
    uv_buf_t output = uv_buf_init(write->response.data(),
                                  static_cast<unsigned int>(write->response.size()));
    write->request.data = write;
    uv_write(&write->request, reinterpret_cast<uv_stream_t*>(&client->stream), &output, 1,
             on_written);
  }

  static void on_read(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
    auto* client = static_cast<Client*>(stream->data);
    if (count > 0) {
      client->request.append(buffer->base, static_cast<std::size_t>(count));
      if (client->request.find("\r\n\r\n") != std::string::npos) {
        auto* write = new WriteRequest;
        write->client = client;
        write->response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(client->owner->body.size()) +
                          "\r\nConnection: close\r\n\r\n" + client->owner->body;
        uv_read_stop(stream);
        auto* delayed = new DelayedWrite;
        delayed->write = write;
        uv_timer_init(&client->owner->loop, &delayed->timer);
        delayed->timer.data = delayed;
        uv_timer_start(&delayed->timer, on_delay, client->owner->delay_ms, 0);
      }
    } else if (count < 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(stream), close_client);
    }
    std::free(buffer->base);
  }

  static void on_connection(uv_stream_t* server_stream, int status) {
    if (status != 0) return;
    auto* owner = static_cast<LocalMetricsHttp*>(server_stream->data);
    auto* client = new Client;
    client->owner = owner;
    uv_tcp_init(&owner->loop, &client->stream);
    client->stream.data = client;
    if (uv_accept(server_stream, reinterpret_cast<uv_stream_t*>(&client->stream)) != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(&client->stream), close_client);
      return;
    }
    ++owner->requests;
    uv_read_start(reinterpret_cast<uv_stream_t*>(&client->stream), alloc, on_read);
  }

  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/debug/vars";
  }
  void close() { uv_close(reinterpret_cast<uv_handle_t*>(&server), nullptr); }

  uv_loop_t& loop;
  uv_tcp_t server{};
  unsigned port = 0;
  std::size_t requests = 0;
  std::uint64_t delay_ms = 0;
  std::string body = R"({"stats":{"outbound":{"node":{"uplink":5,"downlink":8}}},"observatory":{"node":{"alive":true,"delay":9}}})";
};

void run_for(uv_loop_t& loop, std::chrono::milliseconds duration) {
  const auto end = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < end) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
}  // namespace

TEST_CASE("MetricsClient shares one timer across viewers and stops without viewers",
          "[integration][MET-02]") {
  uv_loop_t loop;
  REQUIRE(uv_loop_init(&loop) == 0);
  LocalMetricsHttp fixture(loop);
  HttpClient http(loop);
  {
    MetricsClient client(loop, http);
    RunRecord record;
    record.run_id = "run-shared";
    client.attach(record, fixture.endpoint());
    std::size_t samples = 0;
    client.on_sample([&](const auto&) { ++samples; });
    client.set_viewer_count(2);
    for (int i = 0; i < 1000 && samples == 0; ++i) run_for(loop, std::chrono::milliseconds(1));
    REQUIRE(samples == 1);
    CHECK(fixture.requests == 1);
    CHECK(client.view().availability == MetricAvailability::available);
    client.set_viewer_count(1);
    run_for(loop, std::chrono::milliseconds(100));
    CHECK(fixture.requests == 1);
    client.set_viewer_count(0);
    run_for(loop, std::chrono::milliseconds(6100));
    CHECK(fixture.requests == 1);
    CHECK(client.view().availability == MetricAvailability::stale);
  }
  fixture.close();
  bool drained = false;
  http.shutdown([&] { drained = true; });
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(drained);
  REQUIRE(uv_loop_close(&loop) == 0);
}

TEST_CASE("a late response from a previous core run cannot replace the current sample",
          "[integration][MET-02]") {
  uv_loop_t loop;
  REQUIRE(uv_loop_init(&loop) == 0);
  LocalMetricsHttp fixture(loop);
  fixture.delay_ms = 100;
  HttpClient http(loop);
  {
    MetricsClient client(loop, http);
    RunRecord record;
    record.run_id = "run-old";
    client.attach(record, fixture.endpoint());
    std::size_t samples = 0;
    client.on_sample([&](const auto&) { ++samples; });
    client.set_viewer_count(1);
    for (int i = 0; i < 1000 && fixture.requests == 0; ++i) {
      run_for(loop, std::chrono::milliseconds(1));
    }
    REQUIRE(fixture.requests == 1);
    record.run_id = "run-new";
    client.attach(record, fixture.endpoint());
    for (int i = 0; i < 2600 && !client.latest(); ++i) {
      run_for(loop, std::chrono::milliseconds(1));
    }
    REQUIRE(client.latest());
    CHECK(client.latest()->run_id == "run-new");
    CHECK(samples == 1);
    CHECK(fixture.requests >= 2);
    client.stop();
  }
  fixture.close();
  bool drained = false;
  http.shutdown([&] { drained = true; });
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(drained);
  REQUIRE(uv_loop_close(&loop) == 0);
}

TEST_CASE("metrics without a running run or configured endpoint are not zero-valued samples",
          "[unit][MET-01]") {
  uv_loop_t loop;
  REQUIRE(uv_loop_init(&loop) == 0);
  HttpClient http(loop);
  {
    MetricsClient client(loop, http);
    CHECK(client.view().availability == MetricAvailability::unknown);
    RunRecord record;
    record.run_id = "process-only-run";
    client.attach(record, std::nullopt);
    CHECK(client.view().availability == MetricAvailability::unavailable);
    CHECK_FALSE(client.view().sample.has_value());
    CHECK(client.view().rates.empty());
  }
  bool drained = false;
  http.shutdown([&] { drained = true; });
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(drained);
  REQUIRE(uv_loop_close(&loop) == 0);
}
