#pragma once

#include "model/node.hpp"

#include <string_view>

namespace xrm {

// These URI entry points are implemented per protocol task; write_outbound is
// the single typed/native conversion boundary used by runtime assembly.
struct DecodedNode {
  Node node;
  std::vector<Notice> notices;
};
Result<DecodedNode> parse_vless_uri(std::string_view uri);
Result<DecodedNode> parse_vmess_uri(std::string_view uri);
Result<DecodedNode> parse_trojan_uri(std::string_view uri);
Result<DecodedNode> parse_shadowsocks_uri(std::string_view uri);
Result<DecodedNode> parse_hysteria2_uri(std::string_view uri);
Result<Node> parse_native_outbound(const Json& outbound, Id id,
                                  std::string display_name = {});
Result<Json> write_outbound(const Node& node, std::string_view tag);

}  // namespace xrm
