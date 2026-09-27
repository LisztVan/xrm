#include "tui/components/style.hpp"
#include "tui/shell/workspace.hpp"
#include <algorithm>
namespace xrm::tui {
void Workspace::node_form(bool editing) {
  auto id = nodes_table_->selected_id();
  Json node = Json::object();
  if (editing) {
    auto r = call("node.get", {{"id", id}});
    if (!r) {
      notify(last_error_, ui::Tone::Error);
      return;
    }
    node = *r;
    if (node.contains("source_id") && !node["source_id"].is_null()) {
      notify(
          "Copy subscription node to Local before editing connection settings",
          ui::Tone::Warning);
      node_labels();
      return;
    }
    if (node.value("settings", Json::object()).value("kind", "") != "stream") {
      node_advanced();
      return;
    }
  }
  auto s = node.value("settings", Json::object()),
       p = s.value("protocol", Json::object()),
       ep = s.value("endpoint", Json::object()),
       tr = s.value("transport", Json::object()),
       sec = s.value("security", Json::object());
  std::vector<ui::Field> fields{
      text_field("Name", node.value("original_name", ""), true),
      select_field("Protocol", p.value("kind", "vless"),
                   {{"vless", "VLESS"},
                    {"vmess", "VMess"},
                    {"trojan", "Trojan"},
                    {"shadowsocks", "Shadowsocks"},
                    {"hysteria2", "Hysteria2"}}),
      text_field("Address", ep.value("address", ""), true),
      number_field("Port", ep.value("port", 443), 1, 65535),
      text_field("UUID / password / auth",
                 p.value("id", p.value("password", p.value("auth", ""))), true),
      text_field("SS encryption method", p.value("method", "")),
      select_field("Transport", tr.value("kind", "raw"),
                   {{"raw", "RAW"},
                    {"websocket", "WebSocket"},
                    {"xhttp", "XHTTP"},
                    {"grpc", "gRPC"},
                    {"httpupgrade", "HTTPUpgrade"},
                    {"hysteria", "Hysteria"}}),
      select_field("Security", sec.value("kind", "none"),
                   {{"none", "None"}, {"tls", "TLS"}, {"reality", "REALITY"}}),
      text_field("Server name",
                 sec.value("server_name", Json(nullptr)).is_string()
                     ? sec["server_name"].get<std::string>()
                     : ""),
      text_field("REALITY password", sec.value("password", ""))};
  fields[4].kind = ui::FieldKind::Secret;
  fields[9].kind = ui::FieldKind::Secret;
  form(editing ? "Edit node" : "Add node", fields,
       [this, id, node, s, p, tr, sec,
        editing](ui::Form &f) mutable -> std::optional<std::string> {
         if (editing) {
           auto current = call("node.get", {{"id", id}});
           if (!current)
             return current.error().message;
           if (current->value("settings", Json::object()) !=
               node.value("settings", Json::object()))
             return "Node changed outside this form; draft retained";
         }
         auto protocol = p;
         if (protocol.value("kind", "") != f.value(1))
           protocol = Json::object();
         protocol["kind"] = f.value(1);
         if (f.value(1) == "vless" || f.value(1) == "vmess") {
           protocol["id"] = f.value(4);
           if (!protocol.contains("level"))
             protocol["level"] = nullptr;
           if (f.value(1) == "vless") {
             if (!protocol.contains("flow"))
               protocol["flow"] = nullptr;
             if (!protocol.contains("encryption"))
               protocol["encryption"] = nullptr;
           } else if (!protocol.contains("security"))
             protocol["security"] = nullptr;
         } else if (f.value(1) == "shadowsocks") {
           protocol["method"] = f.value(5);
           protocol["password"] = f.value(4);
         } else
           protocol[f.value(1) == "trojan" ? "password" : "auth"] = f.value(4);
         auto transport = tr;
         if (transport.value("kind", "") != f.value(6))
           transport = Json::object();
         transport["kind"] = f.value(6);
         if (f.value(6) == "websocket") {
           if (!transport.contains("path"))
             transport["path"] = nullptr;
           if (!transport.contains("headers"))
             transport["headers"] = Json::object();
         }
         if (f.value(6) == "xhttp")
           for (auto k : {"host", "path", "mode", "extra"})
             if (!transport.contains(k))
               transport[k] = nullptr;
         if (f.value(6) == "grpc") {
           if (!transport.contains("service_name"))
             transport["service_name"] = nullptr;
           if (!transport.contains("authority"))
             transport["authority"] = nullptr;
           if (!transport.contains("multi_mode"))
             transport["multi_mode"] = false;
         }
         if (f.value(6) == "httpupgrade")
           for (auto k : {"host", "path"})
             if (!transport.contains(k))
               transport[k] = nullptr;
         if (f.value(6) == "hysteria" && !transport.contains("options"))
           transport["options"] = Json::object();
         auto security = sec;
         if (security.value("kind", "") != f.value(7))
           security = Json::object();
         security["kind"] = f.value(7);
         if (f.value(7) != "none") {
           security["server_name"] =
               f.value(8).empty() ? Json(nullptr) : Json(f.value(8));
           if (!security.contains("fingerprint"))
             security["fingerprint"] = nullptr;
           if (!security.contains("options"))
             security["options"] = Json::object();
         }
         if (f.value(7) == "tls" && !security.contains("alpn"))
           security["alpn"] = Json::array();
         if (f.value(7) == "reality") {
           security["password"] = f.value(9);
           for (auto k : {"short_id", "spider_x", "mldsa65_verify"})
             if (!security.contains(k))
               security[k] = nullptr;
         }
         auto settings = s;
         settings["kind"] = "stream";
         settings["endpoint"] = {{"address", f.value(2)},
                                 {"port", std::stoi(f.value(3))}};
         settings["protocol"] = protocol;
         settings["transport"] = transport;
         settings["security"] = security;
         for (auto k : {"sockopt", "finalmask", "mux"})
           if (!settings.contains(k))
             settings[k] = nullptr;
         Json params{{"name", f.value(0)}, {"settings", settings}};
         if (editing)
           params["id"] = id;
         auto r = call(editing ? "node.update" : "node.add", params);
         if (!r)
           return r.error().message;
         refresh();
         notify("Node " + f.value(0) + " saved; runtime unchanged",
                ui::Tone::Success);
         return {};
       });
}
void Workspace::node_advanced() {
  auto id = nodes_table_->selected_id();
  auto r = call("node.get", {{"id", id}});
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  auto node = *r;
  if (node.contains("source_id") && !node["source_id"].is_null()) {
    notify("Copy source node to Local before editing settings",
           ui::Tone::Warning);
    return;
  }
  auto content = ui::Field{"Node.settings (advanced JSON)",
                           node.value("settings", Json::object()).dump(2),
                           ui::FieldKind::Multiline};
  form("Advanced node " + node.value("original_name", ""),
       {text_field("Name", node.value("original_name", ""), true), content},
       [this, id, node](ui::Form &f) -> std::optional<std::string> {
         auto value = Json::parse(f.value(1), nullptr, false);
         if (!value.is_object())
           return "Invalid JSON object; draft retained";
         auto current = call("node.get", {{"id", id}});
         if (!current)
           return current.error().message;
         if (current->value("settings", Json::object()) !=
             node.value("settings", Json::object()))
           return "Node changed outside this form";
         auto saved =
             call("node.update",
                  {{"id", id}, {"name", f.value(0)}, {"settings", value}});
         if (!saved)
           return saved.error().message;
         refresh();
         notify("Node settings saved", ui::Tone::Success);
         return {};
       });
}
void Workspace::node_import() {
  form("Import node",
       {text_field("Name (optional for URI)"),
        select_field(
            "Format", "uri",
            {{"uri", "Share URI"}, {"outbound", "Native outbound JSON"}}),
        {"Value", "", ui::FieldKind::Multiline}},
       [this](ui::Form &f) -> std::optional<std::string> {
         Json p;
         if (f.value(1) == "uri")
           p["uri"] = f.value(2);
         else {
           auto value = Json::parse(f.value(2), nullptr, false);
           if (!value.is_object())
             return "Expected native outbound object";
           p["outbound"] = value;
         }
         if (!f.value(0).empty())
           p["name"] = f.value(0);
         auto r = call("node.add", p);
         if (!r)
           return r.error().message;
         refresh();
         notify("Node imported", ui::Tone::Success);
         return {};
       });
}
void Workspace::node_copy() {
  auto id = nodes_table_->selected_id();
  auto n = find_id(nodes_, id);
  form("Copy to Local",
       {text_field("Name", n.value("name", "") + " copy", true)},
       [this, id](ui::Form &f) -> std::optional<std::string> {
         auto r = call("node.copy", {{"id", id}, {"name", f.value(0)}});
         if (!r)
           return r.error().message;
         refresh();
         notify("Local copy saved", ui::Tone::Success);
         return {};
       });
}
void Workspace::node_labels() {
  auto id = nodes_table_->selected_id();
  auto r = call("node.get", {{"id", id}});
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  auto node = *r;
  auto alias = node.value("alias", Json(nullptr));
  form("Node display",
       {text_field("Alias", alias.is_string() ? alias.get<std::string>() : ""),
        {"Labels (one per line)",
         ui::JoinLines(node.value("labels", std::vector<std::string>{})),
         ui::FieldKind::Multiline}},
       [this, id](ui::Form &f) -> std::optional<std::string> {
         auto a = call("node.set_alias",
                       {{"id", id},
                        {"alias", f.value(0).empty() ? Json(nullptr)
                                                     : Json(f.value(0))}});
         if (!a)
           return a.error().message;
         auto b = call("node.set_labels",
                       {{"id", id}, {"labels", ui::SplitLines(f.value(1))}});
         if (!b)
           return "Alias saved; labels failed: " + b.error().message;
         refresh();
         notify("Node display saved", ui::Tone::Success);
         return {};
       });
}
void Workspace::node_test() {
  auto id = nodes_table_->selected_id();
  auto defaults = settings_.value("tests", Json::object());
  form(
      "Test " + find_id(nodes_, id).value("name", id),
      {select_field("Kind", "tcp",
                    {{"tcp", "TCP"},
                     {"icmp", "ICMP"},
                     {"url", "Proxy URL"},
                     {"download", "Bounded download"}}),
       text_field("Target URL", defaults.value("default_url", "")),
       number_field("Timeout ms", defaults.value("timeout_ms", 5000), 100,
                    60000),
       number_field("Max bytes", 1048576, 1, 33554432)},
      [this, id](ui::Form &f) -> std::optional<std::string> {
        Json p{{"node_id", id},
               {"kind", f.value(0)},
               {"timeout_ms", std::stoll(f.value(2))},
               {"max_body_bytes", std::stoll(f.value(3))}};
        if (!view_.empty())
          p["profile_id"] = view_;
        if (!f.value(1).empty())
          p["target_url"] = f.value(1);
        operation("node.test.start", p,
                  "Test " + find_id(nodes_, id).value("name", id));
        return {};
      },
      "Run test");
}
void Workspace::node_results() {
  auto r = call("node.test.results");
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  const auto id = nodes_table_->selected_id();
  detail_title_ = "Test · " + find_id(nodes_, id).value("name", id);
  detail_ = "No test result for this node";
  for (auto &result : *r)
    if (result.value("node_id", "") == id) {
      detail_ =
          result.value("kind", "") + " · " + result.value("status", "") +
          (result.value("stale", false) ? " · stale" : "") +
          "\nTarget: " + result.value("target", "") +
          "\nLatency: " + scalar(result.value("latency_ms", Json(nullptr))) +
          " ms\nReceived: " + scalar(result.value("bytes", Json(nullptr))) +
          " bytes\nRate: " +
          scalar(result.value("bytes_per_second", Json(nullptr))) + " B/s\n" +
          result.value("message", "");
      break;
    }
  full_detail();
}
void Workspace::cancel_node_tests() {
  bool found = false;
  for (auto &[id, label] : operations_)
    if (label.starts_with("Test ")) {
      found = true;
      auto r = call("node.test.cancel", {{"operation_id", id}});
      if (!r)
        notify(label + ": " + r.error().message, ui::Tone::Error);
      else
        notify(label + ": cancellation requested");
    }
  if (!found)
    notify("No active node tests in this session");
}
void Workspace::group_form(bool editing) {
  if (view_.empty())
    return;
  auto id = groups_table_->selected_id();
  auto g = editing ? find_id(groups_, id) : Json::object();
  auto m = g.value("members", Json::object());
  auto b = g.value("balancer", Json::object());
  std::vector<ui::Choice> members, sources;
  for (auto &n : nodes_)
    members.push_back({n.value("id", ""), n.value("name", "")});
  for (auto &s : subscriptions_)
    sources.push_back({s.value("id", ""), s.value("name", "")});
  auto selected = g.value("selected_node_id", Json(nullptr));
  auto node_choices = members;
  node_choices.insert(node_choices.begin(), {"", "Not selected"});
  std::vector<ui::Field> fields{
      text_field("Name", g.value("name", ""), true),
      select_field("Selection mode", g.value("selection_mode", "automatic"),
                   {{"automatic", "Automatic"}, {"manual", "Manual"}}),
      select_field("Strategy",
                   b.value("strategy", Json::object()).value("type", "random"),
                   {{"random", "Random"},
                    {"roundRobin", "Round robin"},
                    {"leastPing", "Lowest latency"},
                    {"leastLoad", "Least load (experimental)"}}),
      select_field("Member scope", m.value("kind", "explicit"),
                   {{"explicit", "Explicit nodes"},
                    {"filter", "Source and filter union"}}),
      {"Nodes",
       ui::JoinLines(m.value(
           m.value("kind", "") == "explicit" ? "node_ids" : "explicit_node_ids",
           std::vector<std::string>{})),
       ui::FieldKind::MultiSelect, members},
      {"Sources", ui::JoinLines(m.value("sources", std::vector<std::string>{})),
       ui::FieldKind::MultiSelect, sources},
      bool_field("Include local", m.value("include_local", false)),
      select_field("Manual member",
                   selected.is_string() ? selected.get<std::string>() : "",
                   node_choices),
      text_field(
          "Native balancer tag",
          b.value("tag",
                  "xrm-group-" + std::to_string(std::chrono::steady_clock::now()
                                                    .time_since_epoch()
                                                    .count())),
          true)};
  fields.push_back(
      {"Protocols",
       ui::JoinLines(m.value("protocols", std::vector<std::string>{})),
       ui::FieldKind::MultiSelect,
       {{"vless", "VLESS"},
        {"vmess", "VMess"},
        {"trojan", "Trojan"},
        {"shadowsocks", "Shadowsocks"},
        {"hysteria2", "Hysteria2"},
        {"raw", "Native"}}});
  fields.push_back(
      {"Name filters",
       ui::JoinLines(m.value("name_contains", std::vector<std::string>{})),
       ui::FieldKind::StringList});
  fields.push_back(
      {"Label filters",
       ui::JoinLines(m.value("labels", std::vector<std::string>{})),
       ui::FieldKind::StringList});
  auto profile = view_;
  form(editing ? "Edit Group" : "Add Group", std::move(fields),
       [this, profile, g, m, b,
        editing](ui::Form &f) mutable -> std::optional<std::string> {
         if (editing) {
           auto current = call("group.get", {{"profile_id", profile},
                                             {"group_id", g.value("id", "")}});
           if (!current)
             return current.error().message;
           if (*current != g)
             return "Group changed outside this form; draft retained";
         }
         auto group = g;
         group["id"] = g.value("id", "");
         group["name"] = f.value(0);
         group["selection_mode"] = f.value(1);
         auto ids = ui::SplitLines(f.value(4));
         auto members = m;
         if (f.value(3) == "explicit")
           members = {{"kind", "explicit"}, {"node_ids", ids}};
         else {
           members["kind"] = "filter";
           members["sources"] = ui::SplitLines(f.value(5));
           members["explicit_node_ids"] = ids;
           members["include_local"] = f.value(6) == "true";
           members["protocols"] = ui::SplitLines(f.value(9));
           members["name_contains"] = ui::SplitLines(f.value(10));
           members["labels"] = ui::SplitLines(f.value(11));
         }
         if (f.value(1) == "manual" && f.value(7).empty())
           return "Choose a manual member";
         group["members"] = members;
         group["selected_node_id"] =
             f.value(7).empty() ? Json(nullptr) : Json(f.value(7));
         auto balancer = b;
         balancer["tag"] = f.value(8);
         balancer["strategy"]["type"] = f.value(2);
         group["balancer"] = balancer;
         auto r =
             call("group.save", {{"profile_id", profile}, {"group", group}});
         if (!r)
           return r.error().message;
         refresh();
         notify("Group " + f.value(0) + " saved", ui::Tone::Success);
         return {};
       });
}
void Workspace::group_member() {
  auto id = groups_table_->selected_id();
  auto g = find_id(groups_, id);
  if (g.value("selection_mode", "") != "manual") {
    notify("Member selection requires a manual Group", ui::Tone::Warning);
    return;
  }
  std::vector<ui::Choice> choices;
  for (auto &n : nodes_)
    choices.push_back({n.value("id", ""), n.value("name", "")});
  auto p = view_;
  form("Select member", {select_field("Node", "", choices)},
       [this, p, id](ui::Form &f) -> std::optional<std::string> {
         auto r = call(
             "group.select",
             {{"profile_id", p}, {"group_id", id}, {"node_id", f.value(0)}});
         if (!r)
           return r.error().message;
         refresh();
         notify("Manual member saved", ui::Tone::Success);
         return {};
       });
}
} // namespace xrm::tui
