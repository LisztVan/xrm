#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (const char* record = std::getenv("XRM_FAKE_CORE_ARGV")) {
    std::ofstream output(record, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < argc; ++index) {
      output << index << '\t' << argv[index] << '\n';
    }
  }
  if (argc > 1 && std::string(argv[1]) == "version") {
    std::cout << "Xray 26.9.9 (Xray, Penetrates Everything.)\n";
  }
  if (const char* code = std::getenv("XRM_FAKE_CORE_EXIT")) {
    return std::atoi(code);
  }
  return 0;
}
