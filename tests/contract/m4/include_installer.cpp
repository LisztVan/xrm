/// @file
/// @brief Checks that xray/installer.hpp can be included independently.

#include "xray/installer.hpp"
static_assert(sizeof(xrm::CoreRelease) > 0);
