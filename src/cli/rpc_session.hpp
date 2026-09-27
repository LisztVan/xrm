/// @file
/// @brief Shares a scoped daemon RPC connection among CLI command handlers.

#pragma once

// Shared transport session; CLI handlers remain independent of TUI.
#include "client/rpc_session.hpp"
