// XRM: Maps supported native Xray object fields to structured Visual forms.
// fields_for() describes only fields owned by Visual; update_object() edits
// those paths in a copy of the original JSON so unknown fields survive.

#include "tui/forms/native_form.hpp"
#include "tui/components/style.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xrm::tui::forms {
namespace {
using Json = nlohmann::json;
using namespace ftxui;
using ui::Choice;
using ui::Field;
using ui::FieldKind;
std::string escape_token(std::string key);

std::string object_title(const std::string &kind) {
  static const std::vector<std::pair<std::string, std::string>> titles{
      {"dns", "DNS query and cache"},
      {"dns-server", "DNS server"},
      {"routing", "Routing strategy"},
      {"routing-rule", "Routing rule"},
      {"balancer", "Native balancer"},
      {"log", "Xray logging"},
      {"api", "Xray API"},
      {"policy-system", "System traffic counters"},
      {"policy-level", "User level policy"},
      {"metrics", "Metrics endpoint"},
      {"observatory", "Connection observatory"},
      {"burstObservatory", "Burst observatory"},
      {"fakedns", "Fake DNS pool"},
      {"geodata", "Geodata reload"},
      {"asset", "Geodata asset"},
      {"version", "Xray version constraint"},
      {"inbound", "Inbound listener"},
      {"outbound", "Static outbound"},
      {"sniffing", "Inbound sniffing"},
      {"mux", "Multiplexing and XUDP"},
      {"stream", "Transport"},
      {"tls", "TLS security"},
      {"reality", "REALITY security"},
      {"sockopt", "Socket options"}};
  const auto found =
      std::find_if(titles.begin(), titles.end(),
                   [&](const auto &entry) { return entry.first == kind; });
  return found == titles.end() ? kind : found->second;
}

enum class ValueKind {
  Text,
  Secret,
  Integer,
  Port,
  Boolean,
  Lines,
  Choice,
  Choices,
  Advanced
};
struct NativeField {
  std::string path, label;
  ValueKind kind = ValueKind::Text;
  std::vector<Choice> choices;
  std::string hint;
  std::optional<long long> min, max;
};
std::vector<Choice> choices(std::initializer_list<const char *> values) {
  std::vector<Choice> out;
  for (auto value : values)
    out.push_back({value, value});
  return out;
}
NativeField text_field(std::string path, std::string label = {},
                       std::string hint = {}) {
  return {path,
          label.empty() ? path.substr(1) : std::move(label),
          ValueKind::Text,
          {},
          std::move(hint),
          {},
          {}};
}
NativeField integer(std::string path, std::optional<long long> min = {},
                    std::optional<long long> max = {}, std::string hint = {}) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Integer;
  out.min = min;
  out.max = max;
  out.hint = std::move(hint);
  return out;
}
NativeField boolean(std::string path) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Boolean;
  return out;
}
NativeField lines(std::string path, std::string hint = {}) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Lines;
  out.hint = std::move(hint);
  return out;
}
NativeField select(std::string path, std::vector<Choice> values,
                   std::string hint = {}) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Choice;
  out.choices = std::move(values);
  out.hint = std::move(hint);
  return out;
}
NativeField multi(std::string path, std::vector<Choice> values,
                  std::string hint = {}) {
  auto out = select(std::move(path), std::move(values), std::move(hint));
  out.kind = ValueKind::Choices;
  return out;
}
NativeField advanced(std::string path) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Advanced;
  out.label += " (advanced JSON)";
  return out;
}
NativeField secret(std::string path) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Secret;
  return out;
}
NativeField port(std::string path) {
  auto out = text_field(std::move(path));
  out.kind = ValueKind::Port;
  out.hint =
      "Native number, port/range string or env:NAME; existing type preserved";
  return out;
}
const Json *at(const Json &object, const std::string &path) {
  try {
    const Json::json_pointer ptr(path);
    if (object.contains(ptr))
      return &object.at(ptr);
  } catch (const Json::exception &) {
  }
  return nullptr;
}
void erase_at(Json &object, const std::string &path) {
  const auto slash = path.rfind('/');
  const Json::json_pointer parent(path.substr(0, slash));
  if (!object.contains(parent))
    return;
  auto &target = object.at(parent);
  if (target.is_object())
    target.erase(Json::json_pointer(path).back());
}
std::string value_text(const Json *value, ValueKind kind) {
  if (!value)
    return {};
  if ((kind == ValueKind::Lines || kind == ValueKind::Choices) &&
      value->is_array()) {
    std::vector<std::string> values;
    for (const auto &item : *value) {
      if (!item.is_string())
        return value->dump();
      values.push_back(item.get<std::string>());
    }
    return ui::JoinLines(values);
  }
  return value->is_string() ? value->get<std::string>() : value->dump();
}
bool compatible(const Json *value, ValueKind kind) {
  if (!value)
    return true;
  switch (kind) {
  case ValueKind::Text:
  case ValueKind::Secret:
  case ValueKind::Choice:
    return value->is_string();
  case ValueKind::Integer:
    if (value->is_number_unsigned())
      return value->get<std::uint64_t>() <=
             static_cast<std::uint64_t>(std::numeric_limits<long long>::max());
    return value->is_number_integer() || value->is_number_unsigned();
  case ValueKind::Boolean:
    return value->is_boolean();
  case ValueKind::Port:
    return value->is_string() || value->is_number_integer();
  case ValueKind::Lines:
  case ValueKind::Choices:
    return (value->is_string() && value->get_ref<const std::string &>().find(
                                      '\n') == std::string::npos) ||
           (value->is_array() &&
            std::all_of(value->begin(), value->end(), [](const Json &v) {
              return v.is_string() &&
                     !v.get_ref<const std::string &>().empty() &&
                     v.get_ref<const std::string &>().find('\n') ==
                         std::string::npos;
            }));
  case ValueKind::Advanced:
    return true;
  }
  return false;
}
std::vector<Choice> target_strategies() {
  return choices({"AsIs", "UseIP", "UseIPv6v4", "UseIPv6", "UseIPv4v6",
                  "UseIPv4", "ForceIP", "ForceIPv6v4", "ForceIPv6",
                  "ForceIPv4v6", "ForceIPv4"});
}
std::vector<NativeField> fields_for(const std::string &kind,
                                    const NativeFormOptions &options) {
  // Paths are relative to the native object being edited, not to an XRM
  // replacement schema. Advanced fields remain available in module JSON.
  const auto dns_strategy =
      choices({"UseIP", "UseIPv4", "UseIPv6", "UseSystem"});
  if (kind == "dns")
    return {select("/queryStrategy", dns_strategy, "Unset: UseIP"),
            text_field("/clientIp"),
            text_field("/tag"),
            boolean("/disableCache"),
            boolean("/serveStale"),
            integer("/serveExpiredTTL", 0, {},
                    "Seconds; effective with caching and serveStale"),
            boolean("/disableFallback"),
            boolean("/disableFallbackIfMatch"),
            boolean("/enableParallelQuery"),
            boolean("/useSystemHosts")};
  if (kind == "dns-server")
    return {
        text_field("/address", "Address", "IP, DNS URL, localhost or fakedns"),
        integer("/port", 0, 65535, "Default 53; URL port applies for DOH/DOQL"),
        lines("/domains", "Priority domain matches"),
        lines("/expectedIPs", "Keep matching responses"),
        lines("/unexpectedIPs", "Exclude matching responses"),
        select("/queryStrategy", dns_strategy, "Unset: inherit DNS strategy"),
        boolean("/skipFallback"),
        boolean("/finalQuery"),
        integer("/timeoutMs", 0, {}, "Milliseconds; default 4000"),
        text_field("/clientIp"),
        text_field("/tag"),
        boolean("/disableCache"),
        boolean("/serveStale"),
        integer("/serveExpiredTTL", 0)};
  if (kind == "routing")
    return {select("/domainStrategy",
                   choices({"AsIs", "IPIfNonMatch", "IPOnDemand"}),
                   "Unset: AsIs")};
  if (kind == "routing-rule")
    return {text_field("/ruleTag", "Rule label",
                       "Optional native ruleTag; not a match condition"),
            lines("/domain", "Match: domains (any entry)"),
            lines("/ip", "Match: destination IPs (any entry)"),
            select("/network", choices({"tcp", "udp", "tcp,udp"})),
            select("/outboundTag", options.outbound_tags,
                   "Target: choose an outbound or a balancer"),
            select("/balancerTag", options.balancer_tags,
                   "Alternative target; leave outboundTag unset"),
            port("/port"),
            lines("/sourceIP"),
            port("/sourcePort"),
            lines("/localIP"),
            port("/localPort"),
            lines("/user"),
            multi("/inboundTag", options.inbound_tags),
            multi("/protocol", choices({"http", "tls", "quic", "bittorrent"}),
                  "Requires inbound sniffing"),
            lines("/process"),
            port("/vlessRoute"),
            advanced("/attrs"),
            advanced("/webhook")};
  if (kind == "balancer")
    return {text_field("/tag"),
            lines("/selector", "Outbound tag prefixes, not inventory IDs"),
            select("/fallbackTag", options.outbound_tags),
            select("/strategy/type",
                   choices({"random", "roundRobin", "leastPing", "leastLoad"}),
                   "leastPing/leastLoad require observatory coverage"),
            advanced("/strategy/settings")};
  if (kind == "log")
    return {text_field("/access", "Access destination",
                       "Path; empty = stdout; none disables"),
            text_field("/error", "Error destination"),
            select("/loglevel",
                   choices({"debug", "info", "warning", "error", "none"}),
                   "Unset: warning"),
            boolean("/dnsLog"),
            text_field("/maskAddress", "IP masking",
                       "quarter, half, full, or native custom mask")};
  if (kind == "api")
    return {text_field("/tag"), text_field("/listen", "Listen address:port"),
            multi("/services",
                  choices({"HandlerService", "LoggerService", "StatsService",
                           "RoutingService", "ReflectionService",
                           "ObservatoryService"}))};
  if (kind == "policy-system")
    return {boolean("/statsInboundUplink"), boolean("/statsInboundDownlink"),
            boolean("/statsOutboundUplink"), boolean("/statsOutboundDownlink")};
  if (kind == "policy-level")
    return {
        integer("/handshake", 0, {}, "Seconds; default 60"),
        integer("/connIdle", 0, {}, "Seconds; default 300"),
        integer("/uplinkOnly", 0, {}, "Seconds; default 1"),
        integer("/downlinkOnly", 0, {}, "Seconds; default 1"),
        integer("/bufferSize", {}, {}, "KB; default depends on architecture"),
        boolean("/statsUserUplink"),
        boolean("/statsUserDownlink"),
        boolean("/statsUserOnline")};
  if (kind == "metrics")
    return {text_field("/tag"),
            text_field("/listen", "Listen address:port",
                       "Either tag or listen is needed by Xray")};
  if (kind == "observatory")
    return {lines("/subjectSelector", "Outbound tag prefixes"),
            text_field("/probeUrl", "Probe URL"),
            text_field("/probeInterval", "Probe interval",
                       "Duration, e.g. 10s, 2h45m"),
            boolean("/enableConcurrency")};
  if (kind == "burstObservatory")
    return {
        lines("/subjectSelector", "Outbound tag prefixes"),
        text_field("/pingConfig/destination", "Probe URL"),
        text_field("/pingConfig/connectivity", "Local connectivity URL"),
        text_field("/pingConfig/interval", "Mean interval",
                   "Duration; core minimum 10s, default 1m"),
        integer("/pingConfig/sampling", 0),
        text_field("/pingConfig/timeout", "Timeout", "Duration; default 5s"),
        text_field("/pingConfig/httpMethod", "HTTP method",
                   "Default HEAD; extension methods allowed")};
  if (kind == "fakedns")
    return {text_field("/ipPool", "Fake IP pool (CIDR)"),
            integer("/poolSize", 0, {}, "Number of cached mappings")};
  if (kind == "geodata")
    return {
        text_field("/cron", "Reload schedule",
                   "Five-part cron; unset disables schedule"),
        select("/outbound", options.outbound_tags, "Unset follows routing")};
  if (kind == "asset")
    return {text_field("/url", "HTTPS download URL"),
            text_field("/file", "Existing asset filename",
                       "Relative file within Xray asset directory")};
  if (kind == "version")
    return {text_field("/min", "Minimum Xray version",
                       "x.y.z; unset or empty imposes no limit"),
            text_field("/max", "Maximum Xray version")};
  if (kind == "inbound")
    return {text_field("/tag"),
            select("/protocol",
                   choices({"tunnel", "http", "shadowsocks", "socks", "trojan",
                            "vless", "vmess", "wireguard", "hysteria", "tun"})),
            text_field("/listen"), port("/port")};
  if (kind == "outbound")
    return {text_field("/tag"),
            select("/protocol",
                   choices({"blackhole", "dns", "freedom", "http", "loopback",
                            "shadowsocks", "socks", "trojan", "vless", "vmess",
                            "wireguard", "hysteria"})),
            text_field("/sendThrough"),
            select("/targetStrategy", target_strategies(),
                   "Not used by freedom; freedom uses sockopt.domainStrategy")};
  if (kind == "sniffing")
    return {boolean("/enabled"),
            multi("/destOverride", choices({"http", "tls", "quic", "fakedns"})),
            boolean("/metadataOnly"),
            boolean("/routeOnly"),
            lines("/domainsExcluded"),
            lines("/ipsExcluded")};
  if (kind == "mux")
    return {boolean("/enabled"),
            integer("/concurrency", -32768, 32767,
                    "0 defaults to 8; negative disables TCP mux"),
            integer("/xudpConcurrency", -32768, 32767,
                    "0 shares TCP mux; negative uses native UDP"),
            select("/xudpProxyUDP443", choices({"reject", "allow", "skip"}),
                   "Unset: reject")};
  if (kind == "stream")
    return {select("/method",
                   choices({"raw", "xhttp", "mkcp", "grpc", "websocket",
                            "httpupgrade", "hysteria"}),
                   "Legacy network alias is preserved"),
            select("/security", choices({"none", "tls", "reality"})),
            text_field("/address"), integer("/port", 0, 65535)};
  if (kind == "tls")
    return {text_field("/serverName"),
            text_field("/verifyPeerCertByName"),
            lines("/alpn"),
            text_field("/fingerprint", "Client fingerprint",
                       "Native uTLS name; empty disables"),
            text_field("/minVersion"),
            text_field("/maxVersion"),
            boolean("/rejectUnknownSni"),
            boolean("/disableSystemRoot"),
            boolean("/enableSessionResumption"),
            text_field("/pinnedPeerCertSha256"),
            lines("/curvePreferences"),
            secret("/echServerKeys"),
            text_field("/echConfigList"),
            text_field("/cipherSuites"),
            text_field("/masterKeyLog")};
  if (kind == "reality")
    return {text_field("/serverName"),
            text_field("/fingerprint"),
            secret("/password"),
            text_field("/shortId"),
            text_field("/spiderX"),
            text_field("/mldsa65Verify"),
            text_field("/target", "Inbound target"),
            lines("/serverNames"),
            secret("/privateKey"),
            lines("/shortIds"),
            integer("/xver", 0, 2),
            text_field("/minClientVer"),
            text_field("/maxClientVer"),
            integer("/maxTimeDiff", 0),
            secret("/mldsa65Seed"),
            boolean("/show")};
  if (kind == "sockopt")
    return {integer("/mark"),
            select("/tproxy", choices({"off", "redirect", "tproxy"})),
            select("/domainStrategy", target_strategies()),
            select("/dialerProxy", options.outbound_tags),
            text_field("/interface"),
            text_field("/tcpCongestion"),
            integer("/tcpKeepAliveIdle"),
            integer("/tcpKeepAliveInterval"),
            integer("/tcpUserTimeout"),
            integer("/tcpMaxSeg"),
            integer("/tcpWindowClamp"),
            boolean("/acceptProxyProtocol"),
            boolean("/v6only"),
            boolean("/tcpMptcp"),
            advanced("/tcpFastOpen"),
            advanced("/happyEyeballs"),
            advanced("/customSockopt")};
  return {};
}

// No supported field is rewritten merely because a form was opened. Empty,
// omitted, explicit false/zero and unknown native value types remain distinct.
std::vector<Field> make_fields(const Json &value,
                               const std::vector<NativeField> &specs) {
  std::vector<Field> out;
  for (const auto &spec : specs) {
    const auto *current = at(value, spec.path);
    Field field{spec.label, value_text(current, spec.kind)};
    field.hint = spec.hint;
    field.optional =
        spec.kind != ValueKind::Choice && spec.kind != ValueKind::Boolean;
    field.present = current != nullptr;
    if (!compatible(current, spec.kind)) {
      field.readonly = true;
      field.hint =
          "Existing native type/range preserved; use module JSON to edit";
    }
    switch (spec.kind) {
    case ValueKind::Text:
      break;
    case ValueKind::Secret:
      field.kind = FieldKind::Secret;
      break;
    case ValueKind::Integer:
      field.kind = FieldKind::Number;
      field.validate = ui::Integer(spec.min, spec.max, false);
      break;
    case ValueKind::Port:
      break;
    case ValueKind::Boolean:
      field.kind = FieldKind::OptionalCheckbox;
      break;
    case ValueKind::Lines:
      field.kind = FieldKind::StringList;
      break;
    case ValueKind::Choice:
      field.kind = FieldKind::Select;
      field.choices = {{"", "Unset / use core default"}};
      field.choices.insert(field.choices.end(), spec.choices.begin(),
                           spec.choices.end());
      break;
    case ValueKind::Choices:
      field.kind = FieldKind::MultiSelect;
      field.choices = spec.choices;
      break;
    case ValueKind::Advanced:
      field.kind = FieldKind::Multiline;
      field.value = current ? current->dump(2) : "";
      field.validate =
          [](const std::string &input) -> std::optional<std::string> {
        try {
          const auto parsed = Json::parse(input);
          (void)parsed;
          return {};
        } catch (const Json::exception &e) {
          return e.what();
        }
      };
      break;
    }
    if (field.readonly) {
      field.kind = FieldKind::Text;
      field.validate = {};
    }
    if ((spec.kind == ValueKind::Choices) && current) {
      for (const auto &item : ui::SplitLines(field.value)) {
        if (std::none_of(field.choices.begin(), field.choices.end(),
                         [&](const Choice &c) { return c.value == item; }))
          field.choices.push_back({item, item + " (existing native value)"});
      }
    }
    out.push_back(std::move(field));
  }
  return out;
}
std::optional<std::string> update_object(Json &output,
                                         const std::vector<NativeField> &specs,
                                         const std::vector<Field> &baseline,
                                         const ui::Form &form) {
  // Compare value and presence independently. Writing every visible field
  // would turn absent/defaulted fields into explicit native values.
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if ((form.value(i) == baseline[i].value &&
         (!baseline[i].optional || form.present(i) == baseline[i].present)) ||
        baseline[i].readonly)
      continue;
    const auto &spec = specs[i];
    const auto &input = form.value(i);
    if ((baseline[i].optional && !form.present(i)) ||
        (!baseline[i].optional && input.empty())) {
      erase_at(output, spec.path);
      continue;
    }
    Json value;
    switch (spec.kind) {
    case ValueKind::Text:
    case ValueKind::Secret:
    case ValueKind::Choice:
      value = input;
      break;
    case ValueKind::Port: {
      const auto *previous = at(output, spec.path);
      long long number{};
      const auto [end, ec] =
          std::from_chars(input.data(), input.data() + input.size(), number);
      if ((!previous || previous->is_number_integer()) && ec == std::errc{} &&
          end == input.data() + input.size())
        value = number;
      else
        value = input;
      break;
    }
    case ValueKind::Integer: {
      long long number{};
      const auto [end, ec] =
          std::from_chars(input.data(), input.data() + input.size(), number);
      if (ec != std::errc{} || end != input.data() + input.size())
        return spec.label + ": expected integer";
      value = number;
      break;
    }
    case ValueKind::Boolean:
      value = input == "true";
      break;
    case ValueKind::Lines:
    case ValueKind::Choices: {
      auto entries = ui::SplitLines(input);
      const auto *previous = at(output, spec.path);
      value = previous && previous->is_string() && entries.size() == 1
                  ? Json(entries.front())
                  : Json(entries);
      break;
    }
    case ValueKind::Advanced:
      value = Json::parse(input);
      break;
    }
    output[Json::json_pointer(spec.path)] = std::move(value);
  }
  return {};
}

std::string native_context(const std::string &module, const Json &object,
                           const std::vector<NativeField> &specs) {
  std::string result = "Add to draft: " + module +
                       " draft. Module Save: Source. Connect / Apply: runtime. "
                       "Cancel: discard this object's unsubmitted changes.";
  std::vector<std::string> extra;
  const auto visit = [&](auto &&self, const Json &value,
                         const std::string &path) -> void {
    const auto exact =
        std::find_if(specs.begin(), specs.end(), [&](const NativeField &spec) {
          return spec.path == path;
        });
    if (exact != specs.end()) {
      if (exact->kind == ValueKind::Advanced ||
          !compatible(&value, exact->kind))
        extra.push_back(path);
      return;
    }
    const bool parent =
        std::any_of(specs.begin(), specs.end(), [&](const NativeField &spec) {
          return spec.path.starts_with(path + "/");
        });
    if (parent && value.is_object()) {
      for (auto it = value.begin(); it != value.end(); ++it)
        self(self, it.value(), path + "/" + escape_token(it.key()));
    } else {
      extra.push_back(path);
    }
  };
  if (object.is_object())
    for (auto it = object.begin(); it != object.end(); ++it)
      visit(visit, it.value(), "/" + escape_token(it.key()));
  if (!extra.empty()) {
    result += " Additional paths retained: ";
    for (std::size_t i = 0; i < extra.size(); ++i) {
      if (i)
        result += ", ";
      result += extra[i];
    }
    result += ". Edit in the matching native section or JSON.";
  }
  return result;
}

void guarded_cancel(const std::shared_ptr<std::weak_ptr<ui::Form>> &weak,
                    const NativeFormOptions &options,
                    const std::function<void()> &cancel) {
  auto form = weak->lock();
  if (!form || !form->dirty() || options.readonly) {
    if (cancel)
      cancel();
    return;
  }
  if (!options.open_dialog)
    return;
  auto close = options.close_dialog;
  auto save = ui::style::button("Add to draft", [weak, close] {
    if (close)
      close();
    if (auto current = weak->lock())
      current->submit();
  });
  auto discard = ui::style::button("Discard", [close, cancel] {
    if (close)
      close();
    if (cancel)
      cancel();
  });
  auto keep = ui::style::button("Continue editing", [close] {
    if (close)
      close();
  });
  auto buttons = Container::Horizontal({save, discard, keep});
  const auto title = form->title();
  options.open_dialog(Renderer(buttons, [buttons, title] {
    return vbox({ui::style::heading("Unsaved " + title),
                 paragraph("Add to draft keeps these object changes in the "
                           "module draft. Discard closes this form without "
                           "changing the parent draft. Source and runtime "
                           "files remain unchanged."),
                 separator(), buttons->Render()});
  }));
}
} // namespace

std::vector<std::string> NativeFormKinds() {
  return {"dns",          "dns-server", "routing",     "routing-rule",
          "balancer",     "log",        "api",         "policy-system",
          "policy-level", "metrics",    "observatory", "burstObservatory",
          "fakedns",      "geodata",    "asset",       "version",
          "inbound",      "outbound",   "sniffing",    "mux",
          "stream",       "tls",        "reality",     "sockopt"};
}
std::shared_ptr<ui::Form>
MakeObjectForm(const std::string &kind, const Json &input,
               const NativeFormOptions &options,
               std::function<std::optional<std::string>(const Json &)> accept,
               std::function<void()> cancel, bool view_only) {
  // Child-form Save updates the parent module draft via accept(); it does not
  // call module.save or publish a runtime confdir.
  const bool is_string_server = kind == "dns-server" && input.is_string();
  Json original = is_string_server ? Json{{"address", input}} : input;
  if (original.is_null())
    original = Json::object();
  auto specs = fields_for(kind, options);
  // Retain spelling of native aliases already present; Go's JSON reader accepts
  // case aliases, but emitting a second writable spelling is ambiguous.
  const std::vector<std::pair<std::string, std::string>> aliases{
      {"/clientIp", "/clientIP"},
      {"/expectedIPs", "/expectIPs"},
      {"/domain", "/domains"},
      {"/sourceIP", "/source"},
      {"/method", "/network"},
      {"/probeUrl", "/probeURL"},
      {"/tcpCongestion", "/tcpcongestion"},
      {"/v6only", "/V6Only"},
      {"/target", "/dest"},
      {"/password", "/publicKey"}};
  for (auto &spec : specs) {
    for (const auto &[native, alias] : aliases) {
      if (spec.path == native && !at(original, native) && at(original, alias))
        spec.path = alias;
    }
  }
  auto fields = make_fields(original, specs);
  if (kind == "dns-server") {
    for (std::size_t i = 0; i < specs.size(); ++i) {
      if (i == 0)
        fields[i].section = "Server address";
      else if (specs[i].path == "/domains")
        fields[i].section = "Matching and response filters";
      else if (specs[i].path == "/skipFallback")
        fields[i].section = "Fallback and cache";
      else if (specs[i].path == "/clientIp")
        fields[i].section = "Advanced native fields";
    }
  }
  if (kind == "routing-rule") {
    for (std::size_t i = 0; i < specs.size(); ++i) {
      const auto &path = specs[i].path;
      if (path == "/ruleTag")
        fields[i].section = "Match conditions -> Target";
      else if (path == "/outboundTag")
        fields[i].section = "Target - choose outbound or balancer";
      else if (path == "/port")
        fields[i].section = "Additional match conditions (AND)";
      else if (path == "/attrs")
        fields[i].section = "Advanced native fields";
    }
  }
  if (kind == "dns-server")
    fields.push_back({"Native representation",
                      is_string_server ? "string" : "object",
                      FieldKind::Select,
                      {{"string", "Address only (string)"},
                       {"object", "Server settings (object)"}}});
  if (kind == "dns-server" && !fields.empty()) {
    fields[0].optional = false;
    fields[0].validate =
        [](const std::string &value) -> std::optional<std::string> {
      return value.empty()
                 ? std::optional<std::string>("DNS address is required")
                 : std::nullopt;
    };
  }
  auto weak = std::make_shared<std::weak_ptr<ui::Form>>();
  auto result = std::make_shared<ui::Form>(
      object_title(kind), fields,
      [kind, original, specs, fields, input, is_string_server,
       accept = std::move(accept), readonly = options.readonly || view_only](
          ui::Form &form) -> std::optional<std::string> {
        if (readonly)
          return "Read only: enable this Profile's module Edit first";
        Json output = original;
        try {
          if (auto error = update_object(output, specs, fields, form))
            return error;
          if (kind == "routing-rule") {
            const auto outbound = output.value("outboundTag", std::string{});
            const auto balancer = output.value("balancerTag", std::string{});
            if (outbound.empty() && balancer.empty()) {
              form.set_field_error(
                  4,
                  form.fields()[4].choices.empty() &&
                          form.fields()[5].choices.empty()
                      ? "No outbound or balancer exists. Add an outbound in "
                        "Modules or a Group in Nodes, then choose it here."
                      : "Choose an outbound or balancer",
                  {5});
              return "Target: select an outbound or balancer";
            }
            const bool target_changed =
                output.value("outboundTag", Json{}) !=
                    original.value("outboundTag", Json{}) ||
                output.value("balancerTag", Json{}) !=
                    original.value("balancerTag", Json{});
            if (target_changed && !outbound.empty() && !balancer.empty()) {
              form.set_field_error(
                  4, "Choose only one target: outbound or balancer", {5});
              return "Target: choose outboundTag or balancerTag, not both";
            }
          }
          // A plain server stays plain unless the user explicitly adds object
          // fields.
          if (kind == "dns-server" && form.value(specs.size()) == "string") {
            if (!output.is_object() || output.size() != 1 ||
                !output.contains("address"))
              return "Address-only representation cannot contain object "
                     "fields; choose Server settings";
            output = output["address"];
          }
          if (!input.is_object() && !input.is_null() && !is_string_server &&
              output == original)
            output = input;
          return accept ? accept(output) : std::optional<std::string>{};
        } catch (const Json::exception &e) {
          return e.what();
        }
      },
      [weak, options, cancel] { guarded_cancel(weak, options, cancel); },
      options.readonly || view_only, "Add to draft");
  *weak = result;
  result->set_context(native_context(options.module, original, specs) +
                      (kind == "routing-rule"
                           ? " Matches combine with AND. Tab to Target: "
                             "outboundTag / balancerTag. "
                             "ruleTag is optional."
                           : ""));
  return result;
}

namespace {
struct Section {
  std::string id, label, path, kind;
  enum class Mode {
    Object,
    Array,
    Map,
    Advanced,
    EmptyObject
  } mode = Mode::Object;
};
std::string escape_token(std::string key) {
  std::string out;
  for (const char c : key) {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out += c;
  }
  return out;
}
// Preview only known presentation fields. Unknown native data remains in the
// shared draft and is available in JSON; it is never flattened into a dump.
std::string preview_value(const Json *value) {
  if (!value)
    return "Not set";
  if (value->is_string())
    return value->get<std::string>();
  if (value->is_array()) {
    std::string result;
    std::size_t shown = 0;
    for (const auto &item : *value) {
      if (shown == 2) {
        result += " +" + std::to_string(value->size() - shown);
        break;
      }
      if (shown++)
        result += ", ";
      result += item.is_primitive() ? preview_value(&item) : "Object";
    }
    return result.empty() ? "Empty list" : result;
  }
  if (value->is_object())
    return std::to_string(value->size()) + " fields";
  return value->dump();
}
std::string native_value(const Json &value, const std::string &path,
                         const std::string &fallback = "Not set") {
  const auto *field = at(value, path);
  return field ? preview_value(field) : fallback;
}
std::string rule_match(const Json &value) {
  std::string result;
  for (const auto *key :
       {"domain", "ip", "port", "network", "sourceIP", "source", "inboundTag",
        "protocol", "process", "user", "localIP", "sourcePort", "localPort",
        "vlessRoute", "attrs"}) {
    const auto *field = at(value, std::string("/") + key);
    if (!field)
      continue;
    if (!result.empty())
      result += " · ";
    result += std::string(key) + " " + preview_value(field);
  }
  return result.empty() ? "No match fields set" : result;
}
std::vector<ui::Column> collection_columns(const Section &section) {
  if (section.kind == "dns-server")
    return {{"#", 3}, {"Address", 29}, {"Port", 7}, {"Strategy", 16}};
  if (section.kind == "routing-rule")
    return {{"#", 3}, {"Rule / match", 36}, {"Target", 21}};
  if (section.kind == "balancer")
    return {{"#", 3}, {"Tag", 18}, {"Strategy", 14}, {"Selector", 22}};
  if (section.kind == "inbound" || section.kind == "outbound")
    return {
        {"#", 3},
        {"Tag", 22},
        {"Protocol", 15},
        {section.kind == "inbound" ? "Listen / port" : "Target strategy", 22}};
  if (section.kind == "host")
    return {{"Domain match", 31}, {"Addresses", 31}};
  if (section.kind == "environment")
    return {{"Variable", 36}, {"Value", 24}};
  if (section.kind == "policy-level")
    return {
        {"Level", 8}, {"Handshake", 13}, {"Idle timeout", 15}, {"Buffer", 18}};
  if (section.kind == "asset")
    return {{"#", 3}, {"File", 22}, {"URL", 36}};
  return {{"#", 3}, {"IP pool", 35}, {"Pool size", 18}};
}
std::vector<std::string> collection_cells(const Section &section,
                                          const std::string &key,
                                          const Json &value) {
  if (section.kind == "dns-server")
    return {key,
            value.is_string() ? preview_value(&value)
                              : native_value(value, "/address"),
            native_value(value, "/port", "Default"),
            native_value(value, "/queryStrategy", "Inherit")};
  if (section.kind == "routing-rule")
    return {key, native_value(value, "/ruleTag", rule_match(value)),
            native_value(value, "/outboundTag",
                         native_value(value, "/balancerTag", "No target"))};
  if (section.kind == "balancer")
    return {key, native_value(value, "/tag"),
            native_value(value, "/strategy/type", "Default"),
            native_value(value, "/selector")};
  if (section.kind == "inbound" || section.kind == "outbound")
    return {key, native_value(value, "/tag"), native_value(value, "/protocol"),
            section.kind == "inbound"
                ? native_value(value, "/listen", "Default") + ":" +
                      native_value(value, "/port")
                : native_value(value, "/targetStrategy", "Default")};
  if (section.kind == "environment")
    return {key, "Configured"};
  if (section.kind == "host")
    return {key, preview_value(&value)};
  if (section.kind == "policy-level")
    return {key, native_value(value, "/handshake", "Default"),
            native_value(value, "/connIdle", "Default"),
            native_value(value, "/bufferSize", "Default")};
  if (section.kind == "asset")
    return {key, native_value(value, "/file"), native_value(value, "/url")};
  return {key, native_value(value, "/ipPool"),
          native_value(value, "/poolSize")};
}
class NativeEditor final : public ComponentBase,
                           public std::enable_shared_from_this<NativeEditor> {
  // Sections navigate native objects; edits are applied to options_.draft.
  // The editor must not own a second authoritative copy of module Source.
public:
  explicit NativeEditor(NativeFormOptions options)
      : options_(std::move(options)) {
    build_sections();
    std::vector<ui::TreeNode> nodes;
    for (const auto &section : sections_)
      nodes.push_back({section.id, section.label, {}});
    if (options_.module == "routing" && !options_.bound_groups.empty()) {
      ui::TreeNode bindings{
          "bindings", "Dynamic groups (bindings; read only)", {}};
      for (auto &group : options_.bound_groups)
        bindings.children.push_back(
            {"binding/" + group.value, group.label + " → " + group.value, {}});
      nodes.push_back(std::move(bindings));
    }
    tree_ = std::make_shared<ui::SectionTree>(
        std::move(nodes), [this](const std::string &id) {
          if (id == "bindings" || id.starts_with("binding/")) {
            if (options_.open_groups)
              options_.open_groups();
            return;
          }
          auto it = std::find_if(
              sections_.begin(), sections_.end(),
              [&](const Section &section) { return section.id == id; });
          if (it != sections_.end())
            open_section(*it);
        });
    Add(tree_);
  }
  Element OnRender() override {
    using namespace ui::style;
    const auto selected = tree_->selected_id();
    const auto section = std::find_if(
        sections_.begin(), sections_.end(),
        [&](const Section &candidate) { return candidate.id == selected; });
    Elements detail;
    if (section != sections_.end()) {
      detail.push_back(
          heading(section->label, options_.readonly ? "Read only" : "Draft"));
      std::string path = section->path.substr(1);
      std::replace(path.begin(), path.end(), '/', '.');
      detail.push_back(text(path) | ink(Role::Muted));
      detail.push_back(separator() | ink(Role::Subtle));
      append_preview(detail, *section);
    } else {
      detail.push_back(heading("Dynamic groups", "Bindings"));
      detail.push_back(
          paragraph("Managed in Nodes / Groups. These bindings "
                    "generate native routing.balancers when applied.") |
          ink(Role::Muted));
      for (const auto &group : options_.bound_groups)
        detail.push_back(key_value(group.label, group.value));
    }
    auto preview = vbox(std::move(detail)) | flex;
    Element layout;
    if (box_.x_max - box_.x_min + 1 >= 110) {
      layout =
          hbox({tree_->Render() | size(WIDTH, EQUAL, 31), text("  "), preview});
    } else {
      const auto navigation_rows = static_cast<int>(sections_.size()) +
                                   (options_.bound_groups.empty() ? 0 : 1);
      layout = vbox({tree_->Render() |
                         size(HEIGHT, EQUAL, std::clamp(navigation_rows, 1, 4)),
                     text(""), preview});
    }
    return layout | flex | reflect(box_);
  }

private:
  void append_preview(Elements &detail, const Section &section) const {
    using namespace ui::style;
    const auto root = read();
    const auto *value = at(root, section.path);
    if (!value) {
      detail.push_back(text("Not configured") | bold | ink(Role::Muted));
      detail.push_back(paragraph("No fields are present in this section of "
                                 "the current draft.") |
                       ink(Role::Muted));
      return;
    }
    if (section.mode == Section::Mode::EmptyObject && value->is_object()) {
      detail.push_back(text("Stats object present") | bold);
      detail.push_back(paragraph("No native parameters. Source inclusion is "
                                 "managed independently.") |
                       ink(Role::Muted));
      return;
    }
    if ((section.mode == Section::Mode::Array && value->is_array()) ||
        (section.mode == Section::Mode::Map && value->is_object())) {
      if (value->empty()) {
        detail.push_back(text("No entries") | ink(Role::Muted));
        return;
      }
      const int available = std::max(3, box_.y_max - box_.y_min - 8);
      const auto count = std::min(
          value->size(),
          static_cast<std::size_t>(section.kind == "dns-server" ||
                                           section.kind == "routing-rule"
                                       ? std::max(1, available / 3)
                                       : std::max(1, available)));
      std::size_t index = 0;
      for (auto it = value->begin(); it != value->end() && index < count;
           ++it, ++index) {
        const auto label = section.mode == Section::Mode::Map
                               ? it.key()
                               : std::to_string(index + 1);
        const auto cells = collection_cells(section, label, it.value());
        if (section.kind == "dns-server") {
          detail.push_back(hbox(
              {text(label + "  ") | ink(Role::Muted), text(cells[1]) | bold}));
          detail.push_back(
              hbox({text("   port ") | ink(Role::Muted), text(cells[2]),
                    text("    strategy ") | ink(Role::Muted), text(cells[3])}));
          detail.push_back(text(""));
        } else if (section.kind == "routing-rule") {
          detail.push_back(
              hbox({text(label + "  ") | ink(Role::Muted),
                    text(native_value(it.value(), "/ruleTag", "Routing rule")) |
                        bold,
                    text("   to ") | ink(Role::Muted),
                    text(cells[2]) | ink(Role::Accent)}));
          detail.push_back(hbox({text("   match ") | ink(Role::Muted),
                                 text(rule_match(it.value()))}));
          detail.push_back(text(""));
        } else {
          std::string description;
          for (std::size_t column = 1; column < cells.size(); ++column) {
            if (!description.empty())
              description += " · ";
            description += cells[column];
          }
          detail.push_back(key_value(label, description, 16));
        }
      }
      if (count < value->size())
        detail.push_back(text("+ " + std::to_string(value->size() - count) +
                              " more in collection") |
                         ink(Role::Muted));
      return;
    }
    if (!value->is_object()) {
      detail.push_back(text("Native " + std::string(value->type_name())) |
                       ink(Role::Warning));
      detail.push_back(text("Preserved in JSON") | ink(Role::Muted));
      return;
    }
    const auto specs = fields_for(section.kind, options_);
    std::size_t shown = 0;
    for (const auto &spec : specs) {
      const auto *field = at(*value, spec.path);
      if (!field)
        continue;
      detail.push_back(key_value(
          spec.label,
          spec.kind == ValueKind::Secret ? "Configured" : preview_value(field),
          24));
      ++shown;
    }
    if (!shown)
      detail.push_back(text(value->empty()
                                ? "No explicit settings"
                                : "Advanced native fields configured") |
                       ink(Role::Muted));
  }
  Json read() const {
    if (!options_.draft || options_.draft->empty())
      return Json::object();
    return Json::parse(*options_.draft, nullptr, true, true);
  }
  std::optional<std::string>
  modify(const std::function<void(Json &)> &mutation) {
    // Parse, mutate, and replace the one shared JSON draft only when content
    // actually changes. Child dialog cancellation never touches this string.
    if (options_.readonly)
      return "Read only: enable this Profile's module Edit first";
    if (!options_.draft)
      return "Module draft is unavailable";
    try {
      auto root = read();
      const auto original = root;
      mutation(root);
      if (root == original)
        return {};
      *options_.draft = root.dump(2) + "\n";
      if (options_.changed)
        options_.changed();
      return {};
    } catch (const std::exception &e) {
      return e.what();
    }
  }
  void build_sections() {
    // These paths describe native object ownership. For example sniffing is
    // under an inbound, and mux is under an outbound, not a new root module.
    const std::string &key = options_.module;
    const std::string path = "/" + key;
    auto object = [&](std::string label, std::string p, std::string kind) {
      sections_.push_back({std::to_string(sections_.size()), std::move(label),
                           std::move(p), std::move(kind)});
    };
    auto array = [&](std::string label, std::string p, std::string kind) {
      sections_.push_back({std::to_string(sections_.size()), std::move(label),
                           std::move(p), std::move(kind),
                           Section::Mode::Array});
    };
    auto map = [&](std::string label, std::string p, std::string kind) {
      sections_.push_back({std::to_string(sections_.size()), std::move(label),
                           std::move(p), std::move(kind), Section::Mode::Map});
    };
    if (key == "dns") {
      array("DNS servers", path + "/servers", "dns-server");
      object("DNS query and cache settings", path, "dns");
      map("DNS hosts", path + "/hosts", "host");
    } else if (key == "routing") {
      array("Routing rules (ordered)", path + "/rules", "routing-rule");
      array("Native balancers", path + "/balancers", "balancer");
      object("Domain strategy", path, "routing");
    } else if (key == "policy") {
      map("User levels", path + "/levels", "policy-level");
      object("System traffic counters", path + "/system", "policy-system");
    } else if (key == "inbounds")
      array("Inbound listeners", path, "inbound");
    else if (key == "outbounds")
      array("Static outbounds (ordered)", path, "outbound");
    else if (key == "env")
      map("Xray process environment", path, "environment");
    else if (key == "fakedns") {
      const auto root = read();
      const auto *value = at(root, path);
      if (value && value->is_object())
        object("Fake DNS pool (native object)", path, "fakedns");
      else
        array("Fake DNS pools", path, "fakedns");
    } else if (key == "geodata") {
      object("Core reload schedule", path, "geodata");
      array("Core download assets", path + "/assets", "asset");
    } else if (key == "stats")
      sections_.push_back({"0", "Stats object (no native parameters)", path,
                           key, Section::Mode::EmptyObject});
    else
      object(object_title(key), path, key);
  }
  void show_error(std::string error) {
    auto content = std::make_shared<std::string>(std::move(error));
    auto text = ui::ReadOnlyText(content.get());
    options_.open_dialog(
        Renderer(text, [text, content] { return text->Render(); }));
  }
  void open_section(const Section &section) {
    if (!options_.open_dialog)
      return;
    if (section.mode == Section::Mode::Array ||
        section.mode == Section::Mode::Map) {
      open_collection(section);
      return;
    }
    const auto root = read();
    const auto *current = at(root, section.path);
    if (section.mode == Section::Mode::EmptyObject) {
      auto button = ui::style::button(
          "Keep empty stats object in draft", [this, section] {
            if (auto error = modify([&](Json &document) {
                  if (!at(document, section.path))
                    document[Json::json_pointer(section.path)] = Json::object();
                })) {
              show_error(*error);
              return;
            }
            options_.close_dialog();
          });
      auto close = ui::style::button("Close", options_.close_dialog);
      auto controls = options_.readonly ? Container::Vertical({close})
                                        : Container::Vertical({button, close});
      options_.open_dialog(Renderer(controls, [controls] {
        return vbox({text("stats has no native parameters."),
                     text("Source inclusion is managed separately."),
                     controls->Render()});
      }));
      return;
    }
    if (current && !current->is_object()) {
      open_advanced(section.path, section.label, *current);
      return;
    }
    auto self = std::static_pointer_cast<NativeEditor>(shared_from_this());
    auto form = MakeObjectForm(
        section.kind, current ? *current : Json::object(), options_,
        [self, section](const Json &object) -> std::optional<std::string> {
          if (auto error = self->modify([&](Json &document) {
                document[Json::json_pointer(section.path)] = object;
              }))
            return error;
          self->options_.close_dialog();
          return {};
        },
        options_.close_dialog);
    form->set_context(form->context() + " Native path " + section.path + ".");
    options_.open_dialog(form);
  }
  void open_advanced(const std::string &path, const std::string &title,
                     const Json &original, bool view = false) {
    auto self = std::static_pointer_cast<NativeEditor>(shared_from_this());
    Field field{title + " — advanced native JSON", original.dump(2),
                FieldKind::Multiline};
    field.validate =
        [](const std::string &input) -> std::optional<std::string> {
      try {
        const auto parsed = Json::parse(input, nullptr, true, true);
        (void)parsed;
        return {};
      } catch (const Json::exception &e) {
        return e.what();
      }
    };
    auto weak = std::make_shared<std::weak_ptr<ui::Form>>();
    auto form = std::make_shared<ui::Form>(
        title, std::vector<Field>{field},
        [self, path, view](ui::Form &form) -> std::optional<std::string> {
          if (view)
            return "Read only";
          const auto parsed = Json::parse(form.value(0), nullptr, true, true);
          if (auto error = self->modify(
                  [&](Json &root) { root[Json::json_pointer(path)] = parsed; }))
            return error;
          self->options_.close_dialog();
          return {};
        },
        [weak, options = options_] {
          guarded_cancel(weak, options, options.close_dialog);
        },
        options_.readonly || view, "Add to draft");
    *weak = form;
    form->set_context(native_context(options_.module, Json::object(), {}) +
                      " Native path " + path + ".");
    options_.open_dialog(form);
  }

  // Each collection uses the authoritative wrapper draft. Object forms copy
  // only the selected value until accepted, so child cancellation is isolated.
  void open_collection(const Section &section) {
    const auto root = read();
    if (const auto *value = at(root, section.path);
        value &&
        ((section.mode == Section::Mode::Map && !value->is_object()) ||
         (section.mode == Section::Mode::Array && !value->is_array()))) {
      open_advanced(section.path, section.label + " (existing native type)",
                    *value);
      return;
    }
    auto self = std::static_pointer_cast<NativeEditor>(shared_from_this());
    auto holder = std::make_shared<std::weak_ptr<ui::CollectionEditor>>();
    auto refresh = [self, section, holder] {
      auto list = holder->lock();
      if (!list)
        return;
      std::vector<ui::Row> rows;
      const auto root = self->read();
      const auto *value = at(root, section.path);
      if (value) {
        if (section.mode == Section::Mode::Map && value->is_object()) {
          for (auto it = value->begin(); it != value->end(); ++it)
            rows.push_back(
                {it.key(), collection_cells(section, it.key(), it.value())});
        } else if (value->is_array()) {
          for (std::size_t i = 0; i < value->size(); ++i)
            rows.push_back({std::to_string(i),
                            collection_cells(section, std::to_string(i + 1),
                                             value->at(i))});
        }
      }
      list->set_rows(std::move(rows));
    };
    ui::CollectionActions actions;
    actions.add = [self, section, refresh] {
      self->edit_entry(section, {}, false, refresh);
    };
    actions.edit = [self, section, refresh](const std::string &id) {
      self->edit_entry(section, id, false, refresh);
    };
    actions.view = [self, section, refresh](const std::string &id) {
      self->edit_entry(section, id, true, refresh);
    };
    actions.copy = [self, section, refresh](const std::string &id) {
      if (section.mode == Section::Mode::Map) {
        self->edit_entry(section, id, false, refresh, true);
        return;
      }
      if (auto error = self->modify([&](Json &root) {
            auto &array = root.at(Json::json_pointer(section.path));
            const auto index = std::stoul(id);
            Json copy = array.at(index);
            array.insert(array.begin() +
                             static_cast<Json::difference_type>(index + 1),
                         std::move(copy));
          }))
        self->show_error(*error);
      refresh();
    };
    actions.remove = [self, section, refresh](const std::string &id) {
      const auto current = self->read();
      const auto *container = at(current, section.path);
      std::string identity =
          section.mode == Section::Mode::Map
              ? id
              : "entry " + std::to_string(std::stoul(id) + 1);
      if (container) {
        const auto &entry = section.mode == Section::Mode::Map
                                ? container->at(id)
                                : container->at(std::stoul(id));
        const auto cells = collection_cells(section, id, entry);
        if (!cells.empty())
          identity += " · " + ui::Clip(cells.front(), 72);
      }
      self->options_.open_dialog(ui::Confirm(
          "Remove native entry",
          section.label + ": " + identity +
              ". Removes this entry from the module draft only. "
              "Source and runtime files remain unchanged.",
          [self, section, refresh, id] {
            if (auto error = self->modify([&](Json &root) {
                  auto &target = root.at(Json::json_pointer(section.path));
                  if (section.mode == Section::Mode::Map)
                    target.erase(id);
                  else
                    target.erase(
                        target.begin() +
                        static_cast<Json::difference_type>(std::stoul(id)));
                })) {
              self->show_error(*error);
              return;
            }
            self->options_.close_dialog();
            refresh();
          },
          self->options_.close_dialog));
    };
    if (section.mode != Section::Mode::Map)
      actions.move = [self, section, refresh, holder](const std::string &id,
                                                      int delta) {
        std::size_t next{};
        if (auto error = self->modify([&](Json &root) {
              auto &array = root.at(Json::json_pointer(section.path));
              const auto index = static_cast<long long>(std::stoll(id));
              const auto destination = index + delta;
              next = static_cast<std::size_t>(index);
              if (destination < 0 ||
                  destination >= static_cast<long long>(array.size()))
                return;
              std::swap(array.at(static_cast<std::size_t>(index)),
                        array.at(static_cast<std::size_t>(destination)));
              next = static_cast<std::size_t>(destination);
            }))
          self->show_error(*error);
        refresh();
        if (auto collection = holder->lock())
          collection->table()->select_id(std::to_string(next));
      };
    auto collection = std::make_shared<ui::CollectionEditor>(
        section.label, collection_columns(section), std::move(actions),
        options_.readonly);
    *holder = collection;
    refresh();
    auto close = ui::style::button("Done", options_.close_dialog);
    auto body = Container::Vertical({collection, close});
    options_.open_dialog(Renderer(body, [body, collection, close] {
      return vbox({collection->Render() | flex, close->Render()});
    }));
  }

  void edit_entry(const Section &section, std::optional<std::string> id,
                  bool view, std::function<void()> refresh, bool copy = false) {
    const auto root = read();
    const auto *container = at(root, section.path);
    Json original = Json::object();
    if (id && container)
      original = section.mode == Section::Mode::Map
                     ? container->at(*id)
                     : container->at(std::stoul(*id));
    auto self = std::static_pointer_cast<NativeEditor>(shared_from_this());
    auto accept = [self, section, id,
                   refresh](const Json &value) -> std::optional<std::string> {
      if (auto error = self->modify([&](Json &root) {
            auto &array = root[Json::json_pointer(section.path)];
            if (array.is_null())
              array = Json::array();
            if (id)
              array.at(std::stoul(*id)) = value;
            else
              array.push_back(value);
          }))
        return error;
      self->options_.close_dialog();
      refresh();
      return {};
    };
    if (section.mode == Section::Mode::Map) {
      edit_map_entry(section, id, original, view, copy, std::move(refresh));
      return;
    }
    if ((section.kind == "inbound" || section.kind == "outbound") && id) {
      open_connection(section, *id, original, view, std::move(refresh));
      return;
    }
    auto form = MakeObjectForm(section.kind, original, options_,
                               std::move(accept), options_.close_dialog, view);
    form->set_context(form->context() + " Native path " + section.path + "/" +
                      id.value_or("new") + ".");
    options_.open_dialog(form);
  }

  void edit_map_entry(const Section &section, std::optional<std::string> id,
                      const Json &original, bool view, bool copy,
                      std::function<void()> refresh) {
    auto self = std::static_pointer_cast<NativeEditor>(shared_from_this());
    std::vector<Field> fields;
    fields.push_back({section.kind == "host"           ? "Domain match"
                      : section.kind == "policy-level" ? "User level"
                                                       : "Environment name",
                      copy ? "" : id.value_or("")});
    fields[0].validate =
        [](const std::string &name) -> std::optional<std::string> {
      return name.empty() ? std::optional<std::string>("A key is required")
                          : std::nullopt;
    };
    std::vector<NativeField> specs;
    if (section.kind == "policy-level") {
      specs = fields_for("policy-level", options_);
      auto child =
          make_fields(original.is_object() ? original : Json::object(), specs);
      fields.insert(fields.end(), child.begin(), child.end());
      fields[0].validate =
          ui::Integer(0, std::numeric_limits<std::uint32_t>::max(), false);
    } else if (section.kind == "host") {
      fields.push_back({"Native representation",
                        original.is_array() ? "array" : "string",
                        FieldKind::Select,
                        {{"string", "One address (string)"},
                         {"array", "Address list (array)"}}});
      fields.push_back({"Addresses",
                        original.is_string() || original.is_array()
                            ? value_text(&original, ValueKind::Lines)
                            : "",
                        FieldKind::StringList});
      if (id && !compatible(&original, ValueKind::Lines)) {
        fields.back().readonly = true;
        fields.back().hint = "Existing type or empty array entries preserved; "
                             "use module JSON to change";
      }
    } else {
      fields.push_back(
          {"Value", original.is_string() ? original.get<std::string>() : ""});
      if (id && !original.is_string()) {
        fields.back().readonly = true;
        fields.back().hint =
            "Existing native type preserved; use module JSON to change";
      }
    }
    const auto baseline = fields;
    auto weak = std::make_shared<std::weak_ptr<ui::Form>>();
    auto form = std::make_shared<ui::Form>(
        section.label, fields,
        [self, section, id, original, baseline, specs, view, copy,
         refresh](ui::Form &form) -> std::optional<std::string> {
          if (view)
            return "Read only";
          const auto key = form.value(0);
          Json value;
          if (section.kind == "host") {
            auto entries = ui::SplitLines(form.value(2));
            if (form.value(1) == "string") {
              if (entries.size() != 1)
                return "One-address hosts mapping requires exactly one address";
              value = entries.front();
            } else
              value = entries;
            if (form.value(1) == baseline[1].value &&
                form.value(2) == baseline[2].value && id)
              value = original;
          } else if (section.kind == "policy-level") {
            // Offset the fixed native fields using a temporary value view. The
            // form itself remains one multi-field form with a single submit.
            std::vector<Field> child_fields(baseline.begin() + 1,
                                            baseline.end());
            ui::Form values("", child_fields, {}, {});
            for (std::size_t i = 0; i < child_fields.size(); ++i) {
              values.set_value(i, form.value(i + 1));
              values.set_present(i, form.present(i + 1));
            }
            value = original.is_object() ? original : Json::object();
            if (auto error = update_object(value, specs, child_fields, values))
              return error;
          } else
            value = id && form.value(1) == baseline[1].value
                        ? original
                        : Json(form.value(1));
          const auto root = self->read();
          const auto *target = at(root, section.path);
          if (target && target->contains(key) && (!id || key != *id || copy))
            return "An entry with this key already exists";
          if (auto error = self->modify([&](Json &root) {
                auto &map = root[Json::json_pointer(section.path)];
                if (map.is_null())
                  map = Json::object();
                if (id && !copy && key != *id)
                  map.erase(*id);
                map[key] = value;
              }))
            return error;
          self->options_.close_dialog();
          refresh();
          return {};
        },
        [weak, options = options_] {
          guarded_cancel(weak, options, options.close_dialog);
        },
        options_.readonly || view, "Add to draft");
    *weak = form;
    form->set_context(native_context(options_.module, original, specs) +
                      " Native map " + section.path + ".");
    options_.open_dialog(form);
  }

  void open_connection(const Section &section, const std::string &id,
                       const Json &, bool view, std::function<void()> refresh) {
    // One inbound/outbound object is split into readable subsections while
    // each form still edits its original nested native path.
    auto self = std::static_pointer_cast<NativeEditor>(shared_from_this());
    const auto base = section.path + "/" + id;
    struct Part {
      std::string title, suffix, kind;
      bool advanced = false;
    };
    std::vector<Part> parts{
        {"Basic connection", "", section.kind},
        {"Protocol settings (advanced JSON)", "/settings", "", true},
        {"Transport", "/streamSettings", "stream"},
        {"TLS", "/streamSettings/tlsSettings", "tls"},
        {"REALITY", "/streamSettings/realitySettings", "reality"},
        {"Socket options", "/streamSettings/sockopt", "sockopt"},
        {"FinalMask (advanced JSON)", "/streamSettings/finalmask", "", true},
        {"Transport settings (advanced JSON)", "/streamSettings", "", true}};
    if (section.kind == "inbound")
      parts.push_back({"Sniffing", "/sniffing", "sniffing"});
    else
      parts.push_back({"Mux / XUDP", "/mux", "mux"});
    std::vector<ui::TreeNode> nodes;
    for (std::size_t i = 0; i < parts.size(); ++i)
      nodes.push_back({std::to_string(i), parts[i].title, {}});
    auto tree = std::make_shared<ui::SectionTree>(
        std::move(nodes),
        [self, base, parts, view, refresh](const std::string &id) {
          const auto &part = parts.at(std::stoul(id));
          const auto path = base + part.suffix;
          const auto root = self->read();
          const auto *value = at(root, path);
          if (part.advanced) {
            self->open_advanced(path, part.title,
                                value ? *value : Json::object(), view);
            return;
          }
          auto form = MakeObjectForm(
              part.kind, value ? *value : Json::object(), self->options_,
              [self, path, refresh,
               view](const Json &changed) -> std::optional<std::string> {
                if (view)
                  return "Read only";
                if (auto error = self->modify([&](Json &root) {
                      root[Json::json_pointer(path)] = changed;
                    }))
                  return error;
                self->options_.close_dialog();
                refresh();
                return {};
              },
              self->options_.close_dialog, view);
          form->set_context(form->context() + " Native path " + path + ".");
          self->options_.open_dialog(form);
        });
    auto close = ui::style::button("Done", options_.close_dialog);
    auto body = Container::Vertical({tree, close});
    options_.open_dialog(Renderer(body, [tree, close] {
      return vbox({tree->Render() | flex, close->Render()});
    }));
  }
  NativeFormOptions options_;
  std::vector<Section> sections_;
  std::shared_ptr<ui::SectionTree> tree_;
  Box box_;
};
} // namespace

Component MakeNativeForm(NativeFormOptions options) {
  try {
    return std::make_shared<NativeEditor>(std::move(options));
  } catch (const Json::exception &e) {
    const std::string message =
        std::string("Native Visual needs valid JSON: ") + e.what();
    return Renderer([message] { return paragraph(message); });
  }
}
} // namespace xrm::tui::forms
