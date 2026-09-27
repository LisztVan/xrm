// XRM: Checks that the named public header compiles independently.

#include "xray/codecs/codec.hpp"
static_assert(sizeof(xrm::DecodedNode) > 0);
