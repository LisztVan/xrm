#include <cstdlib>
#include <csignal>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

void write_from_environment(const char* key, std::ostream& output) {
  if (const char* value = std::getenv(key)) output << value;
}

void write_hex_from_environment(const char* key, std::ostream& output) {
  const char* value = std::getenv(key);
  if (!value) return;
  const std::string hex(value);
  if (hex.size() % 2) std::exit(92);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    unsigned int byte = 0;
    const auto parsed = std::from_chars(hex.data() + i, hex.data() + i + 2, byte, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + i + 2) std::exit(92);
    output.put(static_cast<char>(byte));
  }
}

void write_repeated_from_environment(const char* key, std::ostream& output) {
  if (const char* value = std::getenv(key)) {
    auto count = std::strtoull(value, nullptr, 10);
    const std::string chunk(64 * 1024, 'x');
    while (count > 0) {
      const auto bytes = static_cast<std::streamsize>(
          std::min<std::uint64_t>(count, chunk.size()));
      output.write(chunk.data(), bytes);
      count -= static_cast<std::uint64_t>(bytes);
    }
  }
}

int wait_at_check_barrier() {
  const char* path = std::getenv("XRM_FAKE_CORE_CHECK_BARRIER_SOCKET");
  if (!path) return 0;
  if (std::char_traits<char>::length(path) >= sizeof(sockaddr_un{}.sun_path)) return 91;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 92;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::copy(path, path + std::char_traits<char>::length(path), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return 93;
  }
  constexpr char ready[] = "READY\n";
  if (::send(fd, ready, sizeof(ready) - 1, 0) != static_cast<ssize_t>(sizeof(ready) - 1)) {
    ::close(fd);
    return 94;
  }
  std::string received;
  char buffer[64];
  while (received.find("RELEASE\n") == std::string::npos) {
    const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) { ::close(fd); return 95; }
    received.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const bool checking = argc >= 3 && std::string(argv[1]) == "run" &&
      std::string(argv[2]) == "-test";
  const char* record_key = checking ? "XRM_FAKE_CORE_CHECK_ARGV" : "XRM_FAKE_CORE_RUN_ARGV";
  const char* record_path = std::getenv(record_key);
  if (!record_path) record_path = std::getenv("XRM_FAKE_CORE_ARGV");
  if (record_path) {
    std::ofstream output(record_path, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < argc; ++index) {
      output << index << '\t' << argv[index] << '\n';
    }
  }
  const char* cwd_key = checking ? "XRM_FAKE_CORE_CHECK_CWD" : "XRM_FAKE_CORE_RUN_CWD";
  const char* record_cwd = std::getenv(cwd_key);
  if (!record_cwd) record_cwd = std::getenv("XRM_FAKE_CORE_CWD");
  if (record_cwd) {
    std::ofstream output(record_cwd, std::ios::binary | std::ios::trunc);
    output << std::filesystem::current_path().string();
  }
  if (argc > 1 && std::string(argv[1]) == "version") {
    std::cout << "Xray 26.9.9 (Xray, Penetrates Everything.)\n";
  }
  write_from_environment("XRM_FAKE_CORE_STDOUT", std::cout);
  write_hex_from_environment("XRM_FAKE_CORE_STDOUT_HEX", std::cout);
  write_repeated_from_environment("XRM_FAKE_CORE_STDOUT_BYTES", std::cout);
  write_from_environment("XRM_FAKE_CORE_STDERR", std::cerr);
  write_repeated_from_environment("XRM_FAKE_CORE_STDERR_BYTES", std::cerr);
  std::cout.flush();
  std::cerr.flush();
  if (checking) {
    if (const int barrier = wait_at_check_barrier(); barrier != 0) return barrier;
    if (const char* code = std::getenv("XRM_FAKE_CORE_CHECK_EXIT")) return std::atoi(code);
  }
  if (!checking && std::getenv("XRM_FAKE_CORE_RUN_HANG")) {
    if (const char* count_path = std::getenv("XRM_FAKE_CORE_RUN_COUNT")) {
      std::ofstream count(count_path, std::ios::app);
      count << "run\n";
    }
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (!checking) {
    if (const char* count_path = std::getenv("XRM_FAKE_CORE_RUN_COUNT")) {
      std::ofstream count(count_path, std::ios::app);
      count << "run\n";
    }
  }
  if (!checking && argc > 1 && std::string(argv[1]) == "run" &&
      !std::getenv("XRM_FAKE_CORE_RUN_EXIT") && !std::getenv("XRM_FAKE_CORE_EXIT") &&
      !std::getenv("XRM_FAKE_CORE_RUN_HANG")) {
    const auto* log_count_value = std::getenv("XRM_FAKE_CORE_LOG_COUNT");
    if (log_count_value) {
      const auto count = std::strtoull(log_count_value, nullptr, 10);
      const auto* interval_value = std::getenv("XRM_FAKE_CORE_LOG_INTERVAL_MS");
      const auto interval = interval_value ? std::strtoull(interval_value, nullptr, 10) : 0;
      for (std::uint64_t index = 0; index < count; ++index) {
        std::cout << "XRM_FAKE_CORE_LOG_" << std::setw(4) << std::setfill('0') << index << '\n' << std::flush;
        if (interval) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      }
    }
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (std::getenv("XRM_FAKE_CORE_HANG")) {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (const char* signal = std::getenv("XRM_FAKE_CORE_SIGNAL")) {
    if (std::string(signal) == "TERM") std::raise(SIGTERM);
  }
  if (const char* code = std::getenv("XRM_FAKE_CORE_EXIT")) {
    return std::atoi(code);
  }
  if (!checking) {
    if (const char* code = std::getenv("XRM_FAKE_CORE_RUN_EXIT")) return std::atoi(code);
  }
  return 0;
}
