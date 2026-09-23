#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

void write_from_environment(const char* key, std::ostream& output) {
  if (const char* value = std::getenv(key)) output << value;
}

void write_repeated_from_environment(const char* key, std::ostream& output) {
  if (const char* value = std::getenv(key)) {
    const auto count = std::strtoull(value, nullptr, 10);
    for (std::uint64_t index = 0; index < count; ++index) output.put('x');
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* record = std::getenv("XRM_FAKE_CORE_ARGV")) {
    std::ofstream output(record, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < argc; ++index) {
      output << index << '\t' << argv[index] << '\n';
    }
  }
  if (const char* record_cwd = std::getenv("XRM_FAKE_CORE_CWD")) {
    std::ofstream output(record_cwd, std::ios::binary | std::ios::trunc);
    output << std::filesystem::current_path().string();
  }
  if (argc > 1 && std::string(argv[1]) == "version") {
    std::cout << "Xray 26.9.9 (Xray, Penetrates Everything.)\n";
  }
  write_from_environment("XRM_FAKE_CORE_STDOUT", std::cout);
  write_repeated_from_environment("XRM_FAKE_CORE_STDOUT_BYTES", std::cout);
  write_from_environment("XRM_FAKE_CORE_STDERR", std::cerr);
  write_repeated_from_environment("XRM_FAKE_CORE_STDERR_BYTES", std::cerr);
  std::cout.flush();
  std::cerr.flush();
  if (std::getenv("XRM_FAKE_CORE_HANG")) {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (const char* signal = std::getenv("XRM_FAKE_CORE_SIGNAL")) {
    if (std::string(signal) == "TERM") std::raise(SIGTERM);
  }
  if (const char* code = std::getenv("XRM_FAKE_CORE_EXIT")) {
    return std::atoi(code);
  }
  return 0;
}
