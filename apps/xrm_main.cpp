/// @file
/// @brief Starts the XRM client and dispatches its CLI or TUI mode.

#include "cli/commands.hpp"

int main(int argc, char **argv) { return xrm::run_cli(argc, argv); }
