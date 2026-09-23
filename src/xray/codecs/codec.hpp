#pragma once

#include "model/node.hpp"

#include <string_view>

namespace xrm {

// M1 supports typed VLESS streams and complete native Xray outbound objects only.
Result<Node> parse_vless_uri(std::string_view uri);
Result<Node> parse_native_outbound(const Json& outbound, Id id,
                                  std::string display_name = {});
Result<Json> write_outbound(const Node& node, std::string_view tag);

}  // namespace xrm
