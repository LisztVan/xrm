/// @file
/// @brief Checks that xray/codecs/codec.hpp can be included independently.

#include "xray/codecs/codec.hpp"
static_assert(sizeof(xrm::DecodedNode) > 0);
