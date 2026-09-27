/// @file
/// @brief Provides a controllable external editor for integration tests.

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  std::string mode = "unchanged";
  std::string marker;
  std::string draft;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--mode" && index + 1 < argc)
      mode = argv[++index];
    else if (arg == "--marker" && index + 1 < argc)
      marker = argv[++index];
    else
      draft = arg;
  }
  if (marker.empty() || draft.empty())
    return 90;
  {
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    output << draft;
    if (!output)
      return 91;
  }
  if (mode == "success" || mode == "output" || mode == "nonzero") {
    std::ofstream output(draft, std::ios::binary | std::ios::trunc);
    if (mode == "nonzero")
      output << "{\"log\":{\"loglevel\":\"debug\"}}\n";
    else if (mode == "invalid")
      output << "{ invalid JSON\n";
    else
      output << "{\"log\":{\"loglevel\":\"warning\"}}\n";
    if (!output)
      return 92;
  } else if (mode == "invalid") {
    std::ofstream output(draft, std::ios::binary | std::ios::trunc);
    output << "{ invalid JSON\n";
  }
  if (mode == "output")
    std::cout << "FAKE_EDITOR_TERMINAL_OUTPUT\n" << std::flush;
  if (mode == "nonzero")
    return 7;
  if (mode == "cancel")
    return 130;
  return 0;
}
