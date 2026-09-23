#pragma once

#include "model/common.hpp"

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace xrm {

struct Endpoint {
  std::string address;
  std::uint16_t port = 0;
};

struct VlessSettings {
  std::string id;
  std::optional<std::string> encryption;
  std::optional<std::string> flow;
  std::optional<std::uint32_t> level;
};

struct RawTransport {};
struct XHttpTransport {
  std::optional<std::string> host;
  std::optional<std::string> path;
  std::optional<std::string> mode;
  std::optional<Json> extra;
};
struct WebSocketTransport {
  std::optional<std::string> path;
  std::map<std::string, std::string> headers;
};
struct GrpcTransport {
  std::optional<std::string> service_name;
  std::optional<std::string> authority;
};
struct HttpUpgradeTransport {
  std::optional<std::string> host;
  std::optional<std::string> path;
};
using Transport = std::variant<RawTransport, XHttpTransport, WebSocketTransport,
                               GrpcTransport, HttpUpgradeTransport>;

struct NoSecurity {};
struct TlsSecurity {
  std::optional<std::string> server_name;
  std::optional<std::string> fingerprint;
  std::vector<std::string> alpn;
  Json options = Json::object();
};
struct RealitySecurity {
  std::optional<std::string> server_name;
  std::optional<std::string> fingerprint;
  std::string password;
  std::optional<std::string> short_id;
  std::optional<std::string> spider_x;
  Json options = Json::object();
};
using Security = std::variant<NoSecurity, TlsSecurity, RealitySecurity>;

struct StreamNode {
  Endpoint endpoint;
  VlessSettings protocol;
  Transport transport = RawTransport{};
  Security security = NoSecurity{};
  std::optional<Json> sockopt;
  std::optional<Json> finalmask;
  std::optional<Json> mux;
};
struct RawOutbound { Json value; };
using NodeSettings = std::variant<StreamNode, RawOutbound>;

struct Node {
  Id id;
  std::optional<Id> source_id;
  std::string original_name;
  std::optional<std::string> alias;
  std::vector<std::string> labels;
  bool enabled = true;
  bool source_missing = false;
  NodeSettings settings = RawOutbound{Json::object()};
  std::optional<std::string> upstream_id;
};

struct NodeChanges {
  std::vector<Node> upserts;
  std::vector<Id> erase_ids;
};

struct NodeQuery {
  std::optional<Id> source_id;
  std::optional<std::string> text;
  std::vector<std::string> labels;
  bool include_missing = false;
};

void to_json(Json& json, const Node& value);
void from_json(const Json& json, Node& value);

}  // namespace xrm
