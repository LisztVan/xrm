#include "server/application.hpp"
#include "server/api_router.hpp"
#include "contract/methods.hpp"
#include "xray/process_adapter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <uv.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace xrm;
using namespace std::chrono_literals;

namespace {
std::string field(int number, std::string_view bytes) {
  return std::string{static_cast<char>((number << 3) | 2), static_cast<char>(bytes.size())} +
         std::string(bytes);
}
std::string valid_geoip() {
  const auto cidr = field(1, std::string{"\x7f\0\0\0", 4}) + std::string{"\x10\x08", 2};
  return field(1, field(1, "XRM-TEST") + field(2, cidr));
}
struct TempTree {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-p5-app-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempTree() { std::filesystem::create_directories(path); }
  ~TempTree() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
class HttpFixture {
 public:
  HttpFixture() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener_ >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener_, 4) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] {
      while (!stopped_) {
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) break;
        char request[512];
        (void)::recv(client, request, sizeof(request), 0);
        const auto body = valid_geoip();
        const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
        ::close(client);
      }
    });
  }
  ~HttpFixture() {
    stopped_ = true;
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    worker_.join();
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/geoip.dat"; }
 private:
  int listener_ = -1, port_ = 0;
  std::atomic<bool> stopped_{false};
  std::thread worker_;
};
}

TEST_CASE("P5 application geodata update uses real Xray then publishes only validated version") {
  const char* binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary)) SKIP("fixed Xray unavailable");
  TempTree temp;
  HttpFixture fixture;
  auto opened = Store::open(temp.path / "state.sqlite");
  REQUIRE(opened);
  REQUIRE((*opened)->put_core(CoreInstall{"xray", "fixed", binary, "26.9.9", false}));
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    SubscriptionService subscriptions(**opened, http);
    Application app(**opened, processes, http, loop, temp.path / "data", "test", "boot",
                    subscriptions);
    auto created = app.put_geodata_resource(GeodataResourceRecord{
        .id = "geo-test", .name = "loopback", .kind = GeoAssetKind::geoip,
        .file_name = "geoip.dat", .url = fixture.url()});
    REQUIRE(created);
    ApiRouter router(app);
    std::optional<RpcResponse> response;
    router.dispatch("tester", RpcRequest{1, 1, std::string(methods::geodata_update),
        Json{{"id", created->id}, {"core_id", "xray"}}},
        [&](RpcResponse value) { response = std::move(value); });
    REQUIRE(response);
    REQUIRE(response->ok);
    const auto operation_id = response->result->at("operation_id").get<std::string>();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      auto state = app.operations().get(operation_id);
      REQUIRE(state);
      if (state->at("status") != "running") {
        CHECK(state->at("status") == "succeeded");
        break;
      }
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      uv_run(&loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(1ms);
    }
    auto record = app.get_geodata_resource(created->id);
    REQUIRE(record);
    REQUIRE(record->current);
    CHECK(record->current->labels.contains("xrm-test"));
    CHECK(std::filesystem::exists(record->current->path));
    const auto local_file = temp.path / "local-geoip.dat";
    { std::ofstream output(local_file, std::ios::binary); output << valid_geoip(); }
    auto local = app.put_geodata_resource(GeodataResourceRecord{
        .id = "geo-local", .name = "local", .kind = GeoAssetKind::geoip,
        .file_name = "geoip.dat", .local_path = local_file});
    REQUIRE(local);
    auto local_operation = app.update_geodata_resource(local->id, "xray");
    REQUIRE(local_operation);
    const auto local_deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      auto state = app.operations().get(*local_operation);
      REQUIRE(state);
      if (state->at("status") != "running") {
        CHECK(state->at("status") == "succeeded");
        break;
      }
      REQUIRE(std::chrono::steady_clock::now() < local_deadline);
      uv_run(&loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(1ms);
    }
    auto imported = app.get_geodata_resource(local->id);
    REQUIRE(imported);
    REQUIRE(imported->current);
    CHECK(imported->current->labels.contains("xrm-test"));
    auto profile = app.profiles().create({"Home", "xray", "local", {}});
    REQUIRE(profile);
    profile->selected = Target{TargetKind::native_outbound, "direct"};
    auto profile_path = (*opened)->profile_path(profile->id);
    REQUIRE(profile_path);
    std::ifstream profile_file(*profile_path / "profile.json");
    const std::string profile_text(std::istreambuf_iterator<char>{profile_file}, {});
    REQUIRE(app.profiles().save(profile->id, ProfileEdit{{profile_text}, *profile}));
    REQUIRE(app.set_profile_geodata(profile->id, {{"geoip.dat", created->id}}));
    auto routing = app.profiles().module(profile->id, "routing");
    REQUIRE(routing);
    REQUIRE(routing->document);
    auto native = Json::parse(routing->document->source_text);
    native["routing"]["rules"].insert(native["routing"]["rules"].begin(),
        Json{{"type", "field"}, {"ip", Json::array({"geoip:xrm-test"})},
             {"outboundTag", "direct"}});
    REQUIRE(app.profiles().save_module(profile->id,
        ModuleEdit{"routing", {routing->document->source_text}, native.dump(2) + "\n"}));
    auto preview = app.render(profile->id);
    REQUIRE(preview);
    CHECK(std::filesystem::exists(preview->confdir / "geoip.dat"));
    const auto exported = temp.path / "export";
    REQUIRE(app.export_config(profile->id, "saved", exported));
    CHECK(std::filesystem::exists(exported / "geoip.dat"));
    std::optional<Result<ProcessResult>> checked;
    RunSpec spec{binary, exported, temp.path,
                 {{"XRAY_LOCATION_ASSET", exported.string()}}};
    auto check = processes.execute(check_command(spec), Milliseconds{10000},
        [&](Result<ProcessResult> result) { checked = std::move(result); });
    REQUIRE(check);
    while (!checked && std::chrono::steady_clock::now() < deadline + 10s) {
      uv_run(&loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(checked);
    REQUIRE(*checked);
    CHECK((*checked)->exit.exit_code == 0);
    bool drained = false;
    app.shutdown([&] { drained = true; });
    while (!drained) uv_run(&loop, UV_RUN_ONCE);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}
