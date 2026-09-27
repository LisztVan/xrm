// XRM: Checks that the named public header compiles independently.

#include "xray/installer.hpp"
static_assert(sizeof(xrm::CoreRelease) > 0);
