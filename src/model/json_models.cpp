#include "model/node.hpp"
#include "model/runtime.hpp"
#include "model/subscription.hpp"
#include "xray/installer.hpp"

#include <stdexcept>

namespace xrm {
namespace {

std::string target_kind_name(TargetKind kind) {
  switch (kind) {
    case TargetKind::node: return "node";
    case TargetKind::group: return "group";
    case TargetKind::native_outbound: return "native_outbound";
    case TargetKind::native_balancer: return "native_balancer";
  }
  return "node";
}

TargetKind target_kind_from_name(const std::string& value) {
  if (value == "node") return TargetKind::node;
  if (value == "group") return TargetKind::group;
  if (value == "native_outbound") return TargetKind::native_outbound;
  if (value == "native_balancer") return TargetKind::native_balancer;
  throw std::invalid_argument("unknown runtime target kind");
}

Json optional_string(const std::optional<std::string>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json settings_json(const NodeSettings& settings) {
  if (const auto* raw = std::get_if<RawOutbound>(&settings))
    return Json{{"kind", "raw"}, {"value", raw->value}};
  const auto& stream = std::get<StreamNode>(settings);
  Json protocol;
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, VlessSettings>)
      protocol = Json{{"kind", "vless"}, {"id", value.id},
                      {"encryption", optional_string(value.encryption)},
                      {"flow", optional_string(value.flow)},
                      {"level", value.level ? Json(*value.level) : Json(nullptr)}};
    else if constexpr (std::is_same_v<T, VmessSettings>)
      protocol = Json{{"kind", "vmess"}, {"id", value.id},
                      {"security", optional_string(value.security)},
                      {"level", value.level ? Json(*value.level) : Json(nullptr)}};
    else if constexpr (std::is_same_v<T, TrojanSettings>)
      protocol = Json{{"kind", "trojan"}, {"password", value.password}};
    else if constexpr (std::is_same_v<T, ShadowsocksSettings>)
      protocol = Json{{"kind", "shadowsocks"}, {"method", value.method},
                      {"password", value.password}};
    else protocol = Json{{"kind", "hysteria2"}, {"auth", value.auth}};
  }, stream.protocol);
  Json transport;
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, RawTransport>) transport = Json{{"kind", "raw"}};
    else if constexpr (std::is_same_v<T, XHttpTransport>)
      transport = Json{{"kind", "xhttp"}, {"host", optional_string(value.host)},
                       {"path", optional_string(value.path)}, {"mode", optional_string(value.mode)},
                       {"extra", value.extra ? *value.extra : Json(nullptr)}};
    else if constexpr (std::is_same_v<T, WebSocketTransport>)
      transport = Json{{"kind", "websocket"}, {"path", optional_string(value.path)}, {"headers", value.headers}};
    else if constexpr (std::is_same_v<T, GrpcTransport>)
      transport = Json{{"kind", "grpc"}, {"service_name", optional_string(value.service_name)},
                       {"authority", optional_string(value.authority)}, {"multi_mode", value.multi_mode}};
    else if constexpr (std::is_same_v<T, HttpUpgradeTransport>)
      transport = Json{{"kind", "httpupgrade"}, {"host", optional_string(value.host)},
                       {"path", optional_string(value.path)}};
    else transport = Json{{"kind", "hysteria"}, {"options", value.options}};
  }, stream.transport);
  Json security;
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, NoSecurity>) security = Json{{"kind", "none"}};
    else if constexpr (std::is_same_v<T, TlsSecurity>)
      security = Json{{"kind", "tls"}, {"server_name", optional_string(value.server_name)},
                      {"fingerprint", optional_string(value.fingerprint)}, {"alpn", value.alpn}, {"options", value.options}};
    else security = Json{{"kind", "reality"}, {"server_name", optional_string(value.server_name)},
                         {"fingerprint", optional_string(value.fingerprint)}, {"password", value.password},
                         {"short_id", optional_string(value.short_id)}, {"spider_x", optional_string(value.spider_x)},
                         {"mldsa65_verify", optional_string(value.mldsa65_verify)},
                         {"options", value.options}};
  }, stream.security);
  return Json{{"kind", "stream"},
              {"endpoint", Json{{"address", stream.endpoint.address}, {"port", stream.endpoint.port}}},
              {"protocol", std::move(protocol)}, {"transport", std::move(transport)},
              {"security", std::move(security)},
              {"sockopt", stream.sockopt ? *stream.sockopt : Json(nullptr)},
              {"finalmask", stream.finalmask ? *stream.finalmask : Json(nullptr)},
              {"mux", stream.mux ? *stream.mux : Json(nullptr)}};
}

std::optional<std::string> read_optional_string(const Json& value, const char* key) {
  const auto& field = value.at(key);
  if (field.is_null()) return std::nullopt;
  return field.get<std::string>();
}

NodeSettings settings_from_json(const Json& json) {
  const auto kind = json.at("kind").get<std::string>();
  if (kind == "raw") return RawOutbound{json.at("value")};
  if (kind != "stream") throw std::invalid_argument("unknown node settings kind");
  const auto& p = json.at("protocol");
  const auto protocol_kind = p.at("kind").get<std::string>();
  const auto& e = json.at("endpoint");
  StreamNode stream;
  stream.endpoint = {e.at("address").get<std::string>(), e.at("port").get<std::uint16_t>()};
  if (protocol_kind == "vless") {
    VlessSettings value;
    value.id = p.at("id").get<std::string>();
    value.encryption = read_optional_string(p, "encryption");
    value.flow = read_optional_string(p, "flow");
    if (!p.at("level").is_null()) value.level = p.at("level").get<std::uint32_t>();
    stream.protocol = std::move(value);
  } else if (protocol_kind == "vmess") {
    VmessSettings value;
    value.id = p.at("id").get<std::string>();
    value.security = read_optional_string(p, "security");
    if (!p.at("level").is_null()) value.level = p.at("level").get<std::uint32_t>();
    stream.protocol = std::move(value);
  } else if (protocol_kind == "trojan") stream.protocol = TrojanSettings{p.at("password").get<std::string>()};
  else if (protocol_kind == "shadowsocks")
    stream.protocol = ShadowsocksSettings{p.at("method").get<std::string>(), p.at("password").get<std::string>()};
  else if (protocol_kind == "hysteria2") stream.protocol = Hysteria2Settings{p.at("auth").get<std::string>()};
  else throw std::invalid_argument("unsupported node protocol kind");
  const auto& t = json.at("transport");
  const auto tk = t.at("kind").get<std::string>();
  if (tk == "raw") stream.transport = RawTransport{};
  else if (tk == "xhttp") stream.transport = XHttpTransport{read_optional_string(t, "host"), read_optional_string(t, "path"), read_optional_string(t, "mode"), t.at("extra").is_null() ? std::nullopt : std::optional<Json>(t.at("extra"))};
  else if (tk == "websocket") stream.transport = WebSocketTransport{read_optional_string(t, "path"), t.at("headers").get<std::map<std::string, std::string>>()};
  else if (tk == "grpc") stream.transport = GrpcTransport{read_optional_string(t, "service_name"), read_optional_string(t, "authority"), t.value("multi_mode", false)};
    else if (tk == "httpupgrade") stream.transport = HttpUpgradeTransport{read_optional_string(t, "host"), read_optional_string(t, "path")};
    else if (tk == "hysteria") stream.transport = HysteriaTransport{t.at("options")};
  else throw std::invalid_argument("unknown transport kind");
  const auto& s = json.at("security");
  const auto sk = s.at("kind").get<std::string>();
  if (sk == "none") stream.security = NoSecurity{};
  else if (sk == "tls") stream.security = TlsSecurity{read_optional_string(s, "server_name"), read_optional_string(s, "fingerprint"), s.at("alpn").get<std::vector<std::string>>(), s.at("options")};
  else if (sk == "reality") stream.security = RealitySecurity{read_optional_string(s, "server_name"), read_optional_string(s, "fingerprint"), s.at("password").get<std::string>(), read_optional_string(s, "short_id"), read_optional_string(s, "spider_x"), s.at("options"), s.contains("mldsa65_verify") ? read_optional_string(s, "mldsa65_verify") : std::nullopt};
  else throw std::invalid_argument("unknown security kind");
  for (const auto key : {"sockopt", "finalmask", "mux"}) {
    const auto& field = json.at(key);
    if (field.is_null()) continue;
    if (key == std::string_view("sockopt")) stream.sockopt = field;
    else if (key == std::string_view("finalmask")) stream.finalmask = field;
    else stream.mux = field;
  }
  return stream;
}

}  // namespace

void to_json(Json& json, const Node& value) {
  json = Json{{"id", value.id}, {"source_id", value.source_id ? Json(*value.source_id) : Json(nullptr)},
              {"upstream_id", optional_string(value.upstream_id)},
              {"original_name", value.original_name}, {"alias", optional_string(value.alias)},
              {"labels", value.labels}, {"enabled", value.enabled},
              {"source_missing", value.source_missing}, {"settings", settings_json(value.settings)}};
}

void from_json(const Json& json, Node& value) {
  if (!json.is_object()) throw Json::type_error::create(302, "node must be an object", &json);
  value.id = json.at("id").get<std::string>();
  value.source_id = json.at("source_id").is_null() ? std::nullopt : std::optional<Id>(json.at("source_id").get<std::string>());
  value.upstream_id = json.contains("upstream_id")
                          ? read_optional_string(json, "upstream_id")
                          : std::nullopt;
  value.original_name = json.at("original_name").get<std::string>();
  value.alias = read_optional_string(json, "alias");
  value.labels = json.at("labels").get<std::vector<std::string>>();
  value.enabled = json.at("enabled").get<bool>();
  value.source_missing = json.at("source_missing").get<bool>();
  value.settings = settings_from_json(json.at("settings"));
}

void to_json(Json& json, const Subscription& value) {
  json = Json{{"id", value.id}, {"name", value.name}, {"url", value.url},
              {"user_agent", value.user_agent}, {"headers", value.headers},
              {"auto_refresh", value.auto_refresh}, {"interval_seconds", value.interval.count()},
              {"route", Json{{"proxy_url", optional_string(value.route.proxy_url)}}},
              {"info_name_prefixes", value.info_name_prefixes ? Json(*value.info_name_prefixes) : Json(nullptr)},
              {"settings_revision", value.settings_revision}};
}

void from_json(const Json& json, Subscription& value) {
  value.id = json.at("id").get<Id>();
  value.name = json.at("name").get<std::string>();
  value.url = json.at("url").get<std::string>();
  value.user_agent = json.at("user_agent").get<std::string>();
  value.headers = json.at("headers").get<std::map<std::string, std::string>>();
  value.auto_refresh = json.at("auto_refresh").get<bool>();
  value.interval = std::chrono::seconds(json.at("interval_seconds").get<std::int64_t>());
  value.route.proxy_url = read_optional_string(json.at("route"), "proxy_url");
  value.info_name_prefixes = json.contains("info_name_prefixes") &&
      !json.at("info_name_prefixes").is_null()
      ? std::optional<std::vector<std::string>>(json.at("info_name_prefixes").get<std::vector<std::string>>())
      : std::nullopt;
  value.settings_revision = json.at("settings_revision").get<std::uint64_t>();
}

void to_json(Json& json, const SubscriptionState& value) {
  json = Json{{"last_attempt", value.last_attempt ? Json(*value.last_attempt) : Json(nullptr)},
              {"last_success", value.last_success ? Json(*value.last_success) : Json(nullptr)},
              {"etag", optional_string(value.etag)},
              {"last_modified", optional_string(value.last_modified)},
              {"last_result", value.last_result},
              {"reported_usage", optional_string(value.reported_usage)},
              {"reported_notices", value.reported_notices},
              {"last_unsupported_count", value.last_unsupported_count},
              {"last_malformed_count", value.last_malformed_count}};
}

void from_json(const Json& json, SubscriptionState& value) {
  if (!json.at("last_attempt").is_null()) value.last_attempt = json.at("last_attempt").get<UnixMillis>();
  if (!json.at("last_success").is_null()) value.last_success = json.at("last_success").get<UnixMillis>();
  value.etag = read_optional_string(json, "etag");
  value.last_modified = read_optional_string(json, "last_modified");
  value.last_result = json.at("last_result").get<std::string>();
  value.reported_usage = read_optional_string(json, "reported_usage");
  value.reported_notices = json.value("reported_notices", std::vector<std::string>{});
  value.last_unsupported_count = json.value("last_unsupported_count", std::size_t{0});
  value.last_malformed_count = json.value("last_malformed_count", std::size_t{0});
}

void to_json(Json& json, const ExplicitMembers& value) {
  json = Json{{"kind", "explicit"}, {"node_ids", value.node_ids}};
}
void from_json(const Json& json, ExplicitMembers& value) {
  value.node_ids = json.at("node_ids").get<std::vector<Id>>();
}
void to_json(Json& json, const NodeFilter& value) {
  json = Json{{"kind", "filter"}, {"sources", value.sources},
              {"explicit_node_ids", value.explicit_node_ids}, {"include_local", value.include_local},
              {"protocols", value.protocols}, {"name_contains", value.name_contains},
              {"labels", value.labels}};
}
void from_json(const Json& json, NodeFilter& value) {
  value.sources = json.value("sources", std::vector<Id>{});
  value.explicit_node_ids = json.value("explicit_node_ids", std::vector<Id>{});
  value.include_local = json.value("include_local", false);
  value.protocols = json.value("protocols", std::vector<std::string>{});
  value.name_contains = json.value("name_contains", std::vector<std::string>{});
  value.labels = json.value("labels", std::vector<std::string>{});
}
void to_json(Json& json, const NodeSelector& value) {
  std::visit([&](const auto& selector) { to_json(json, selector); }, value);
}
void from_json(const Json& json, NodeSelector& value) {
  const auto kind = json.at("kind").get<std::string>();
  if (kind == "explicit") value = json.get<ExplicitMembers>();
  else if (kind == "filter") value = json.get<NodeFilter>();
  else throw std::invalid_argument("unknown node selector kind");
}
void to_json(Json& json, const GroupBinding& value) {
  json = Json{{"id", value.id}, {"name", value.name}, {"members", value.members},
              {"selection_mode", value.selection_mode == SelectionMode::manual ? "manual" : "automatic"},
              {"selected_node_id", value.selected_node_id ? Json(*value.selected_node_id) : Json(nullptr)},
              {"balancer", value.balancer}};
}
void from_json(const Json& json, GroupBinding& value) {
  value.id = json.at("id").get<Id>();
  value.name = json.at("name").get<std::string>();
  value.members = json.at("members").get<NodeSelector>();
  const auto mode = json.at("selection_mode").get<std::string>();
  if (mode != "automatic" && mode != "manual") throw std::invalid_argument("unknown group selection mode");
  value.selection_mode = mode == "manual" ? SelectionMode::manual : SelectionMode::automatic;
  value.selected_node_id = json.at("selected_node_id").is_null()
      ? std::nullopt : std::optional<Id>(json.at("selected_node_id").get<Id>());
  value.balancer = json.at("balancer");
}
void to_json(Json& json, const ObservationBinding& value) {
  json = Json{{"append_used_group_members", value.append_used_group_members},
              {"additional_node_ids", value.additional_node_ids}};
}
void from_json(const Json& json, ObservationBinding& value) {
  value.append_used_group_members = json.value("append_used_group_members", false);
  value.additional_node_ids = json.value("additional_node_ids", std::vector<Id>{});
}
void to_json(Json& json, const ManagerBindings& value) {
  json = Json{{"groups", value.groups}, {"observation", value.observation}};
}
void from_json(const Json& json, ManagerBindings& value) {
  value.groups = json.value("groups", std::vector<GroupBinding>{});
  value.observation = json.value("observation", ObservationBinding{});
}

void to_json(Json& json, const RuntimePersistent& value) {
  auto encode_run = [](const RunRecord& run) {
    Json nodes = Json::array();
    for (const auto& node : run.index.nodes)
      nodes.push_back(Json{{"id", node.id}, {"tag", node.tag}, {"display_name", node.display_name}});
    Json groups = Json::array();
    for (const auto& group : run.index.groups)
      groups.push_back(Json{{"id", group.id}, {"tag", group.tag},
          {"candidate_ids", group.candidate_ids},
          {"selected_node_id", group.selected_node_id ? Json(*group.selected_node_id) : Json(nullptr)}});
    Json applied = nullptr;
    if (run.applied_selection)
      applied = Json{{"kind", target_kind_name(run.applied_selection->kind)},
                     {"id_or_tag", run.applied_selection->id_or_tag}};
    return Json{{"run_id", run.run_id}, {"profile_id", run.profile_id}, {"core_id", run.core_id},
                {"spec", Json{{"binary", run.spec.binary.string()}, {"confdir", run.spec.confdir.string()},
                               {"cwd", run.spec.cwd.string()}, {"environment", run.spec.environment}}},
                {"applied_selection", applied}, {"index", Json{{"nodes", nodes},
                    {"groups", groups},
                    {"static_outbound_tags", run.index.static_outbound_tags},
                    {"static_balancer_tags", run.index.static_balancer_tags}}},
                {"api_endpoint", optional_string(run.api_endpoint)},
                {"metrics_endpoint", optional_string(run.metrics_endpoint)},
                {"started_at", run.started_at}};
  };
  json = Json{{"desired_running", value.desired_running},
              {"active", value.active ? encode_run(*value.active) : Json(nullptr)},
              {"previous", value.previous ? encode_run(*value.previous) : Json(nullptr)}};
}

void from_json(const Json& json, RuntimePersistent& value) {
  auto decode_run = [](const Json& run) {
    RunRecord record;
    record.run_id = run.at("run_id").get<std::string>();
    record.profile_id = run.at("profile_id").get<std::string>();
    record.core_id = run.at("core_id").get<std::string>();
    const auto& spec = run.at("spec");
    record.spec = RunSpec{spec.at("binary").get<std::string>(), spec.at("confdir").get<std::string>(),
                          spec.at("cwd").get<std::string>(),
                          spec.at("environment").get<std::map<std::string, std::string>>()};
    if (!run.at("applied_selection").is_null()) {
      const auto& target = run.at("applied_selection");
      record.applied_selection = Target{target_kind_from_name(target.at("kind").get<std::string>()),
                                        target.at("id_or_tag").get<std::string>()};
    }
    const auto& index = run.at("index");
    for (const auto& node : index.at("nodes"))
      record.index.nodes.push_back(NodeRuntimeEntry{node.at("id").get<std::string>(),
          node.at("tag").get<std::string>(), node.at("display_name").get<std::string>()});
    if (index.contains("groups"))
      for (const auto& group : index.at("groups"))
        record.index.groups.push_back(GroupRuntimeEntry{group.at("id").get<Id>(),
            group.at("tag").get<std::string>(), group.value("candidate_ids", std::vector<Id>{}),
            group.at("selected_node_id").is_null() ? std::nullopt
              : std::optional<Id>(group.at("selected_node_id").get<Id>())});
    record.index.static_outbound_tags = index.at("static_outbound_tags").get<std::vector<std::string>>();
    record.index.static_balancer_tags = index.at("static_balancer_tags").get<std::vector<std::string>>();
    if (run.contains("api_endpoint")) record.api_endpoint = read_optional_string(run, "api_endpoint");
    if (run.contains("metrics_endpoint")) record.metrics_endpoint = read_optional_string(run, "metrics_endpoint");
    record.started_at = run.at("started_at").get<UnixMillis>();
    return record;
  };
  value.desired_running = json.at("desired_running").get<bool>();
  value.active = json.at("active").is_null() ? std::nullopt : std::optional<RunRecord>(decode_run(json.at("active")));
  value.previous = json.at("previous").is_null() ? std::nullopt : std::optional<RunRecord>(decode_run(json.at("previous")));
}

void to_json(Json& json, const ReleaseAsset& value) {
  json = Json{{"name", value.name}, {"url", value.url},
              {"platform", value.platform}, {"architecture", value.architecture},
              {"sha256", optional_string(value.sha256)}};
}
void from_json(const Json& json, ReleaseAsset& value) {
  value.name = json.at("name").get<std::string>();
  value.url = json.at("url").get<std::string>();
  value.platform = json.at("platform").get<std::string>();
  value.architecture = json.at("architecture").get<std::string>();
  value.sha256 = read_optional_string(json, "sha256");
}
void to_json(Json& json, const CoreRelease& value) {
  json = Json{{"tag", value.tag}, {"published_at", value.published_at},
              {"prerelease", value.prerelease}, {"assets", value.assets}};
}
void from_json(const Json& json, CoreRelease& value) {
  value.tag = json.at("tag").get<std::string>();
  value.published_at = json.at("published_at").get<UnixMillis>();
  value.prerelease = json.at("prerelease").get<bool>();
  value.assets = json.at("assets").get<std::vector<ReleaseAsset>>();
}
void to_json(Json& json, const InstallRequest& value) {
  json = Json{{"release", value.release}, {"asset_name", value.asset_name},
              {"allow_unverified_checksum", value.allow_unverified_checksum}};
}
void from_json(const Json& json, InstallRequest& value) {
  value.release = json.at("release").get<CoreRelease>();
  value.asset_name = json.at("asset_name").get<std::string>();
  value.allow_unverified_checksum = json.value("allow_unverified_checksum", false);
}

}  // namespace xrm
