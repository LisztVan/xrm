/// @file
/// @brief Checks that profile/forms.hpp can be included independently.

#include "profile/forms.hpp"
static_assert(sizeof(xrm::ListEdit) > 0);
