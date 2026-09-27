// XRM: Declares subscription and node-link parsing.

#pragma once

#include "model/subscription.hpp"

#include <string_view>

namespace xrm {

Result<ParsedNode> parse_share_uri(std::string_view uri);
Result<ParsedNode> parse_native_outbound(std::string_view text,
                                         std::string_view fallback_name = {});
Result<ParseReport> parse_subscription(std::string_view body);

} // namespace xrm
