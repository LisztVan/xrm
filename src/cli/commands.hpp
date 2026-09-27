// XRM: Declares the client command-line entry point.

#pragma once

#include <string>

namespace xrm {

int run_cli(int argc, char **argv);

namespace cli {
int run_tui(const std::string &socket, const std::string &profile_id);
}

} // namespace xrm
