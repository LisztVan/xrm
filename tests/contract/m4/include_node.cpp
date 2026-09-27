// XRM: Checks that the named public header compiles independently.

#include "model/node.hpp"
static_assert(std::variant_size_v<xrm::ProtocolSettings> == 5);
static_assert(std::variant_size_v<xrm::Transport> == 6);
