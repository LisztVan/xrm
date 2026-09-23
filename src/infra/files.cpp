#include "infra/files.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace xrm {
namespace {

std::atomic<std::uint64_t> temp_sequence{0};

AppError file_error(ErrorCode code, std::string operation,
                    const std::filesystem::path& path) {
  operation += "：";
  operation += path.string();
  operation += ": ";
  operation += std::strerror(errno);
  return {code, std::move(operation), path.string(), std::nullopt};
}

AppError conflict(const std::filesystem::path& path) {
  return {ErrorCode::conflict, "文件在读取后已被外部修改", path.string(),
          "重新读取文件后再保存"};
}

Result<void> atomic_replace(const std::filesystem::path& path,
                            std::string_view bytes) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return std::unexpected(AppError{ErrorCode::io_error,
        "创建文件目录失败：" + error.message(), path.parent_path().string(), std::nullopt});
  }
  auto temp = path;
  temp += ".tmp-" + std::to_string(::getpid()) + "-" +
          std::to_string(temp_sequence.fetch_add(1, std::memory_order_relaxed));
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected(file_error(ErrorCode::io_error, "创建临时文件失败", temp));

  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { write_ok = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  if (write_ok && ::fsync(fd) != 0) write_ok = false;
  const int close_result = ::close(fd);
  if (!write_ok || close_result != 0) {
    auto result = file_error(ErrorCode::io_error, "写入临时文件失败", temp);
    std::filesystem::remove(temp, error);
    return std::unexpected(std::move(result));
  }
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    auto result = file_error(ErrorCode::io_error, "替换文件失败", path);
    std::filesystem::remove(temp, error);
    return std::unexpected(std::move(result));
  }
  const int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd >= 0) {
    (void)::fsync(dir_fd);
    (void)::close(dir_fd);
  }
  return {};
}

}  // namespace

Result<std::string> read_text(const std::filesystem::path& path, std::size_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    const auto code = error == std::errc::no_such_file_or_directory
                          ? ErrorCode::not_found : ErrorCode::io_error;
    return std::unexpected(AppError{code, "读取文件大小失败：" + error.message(),
                                    path.string(), std::nullopt});
  }
  if (size > max_bytes) {
    return std::unexpected(AppError{ErrorCode::too_large, "文件超过读取上限",
                                    path.string(), std::nullopt});
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::unexpected(AppError{ErrorCode::io_error, "打开文件失败",
                                              path.string(), std::nullopt});
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!bytes.empty()) input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() >= 0 && static_cast<std::size_t>(input.gcount()) != bytes.size()) {
    return std::unexpected(AppError{ErrorCode::io_error, "文件读取期间发生变化",
                                    path.string(), std::nullopt});
  }
  return bytes;
}

Result<void> replace_text(const std::filesystem::path& path, std::string_view bytes) {
  return atomic_replace(path, bytes);
}

Result<void> write_if_unchanged(const std::filesystem::path& path, const EditBase& base,
                                std::string_view bytes) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) return std::unexpected(AppError{ErrorCode::io_error,
      "检查文件状态失败：" + error.message(), path.string(), std::nullopt});
  if (!base.expected_text) {
    if (exists) return std::unexpected(conflict(path));
    return atomic_replace(path, bytes);
  }
  if (!exists) return std::unexpected(conflict(path));
  auto actual = read_text(path, 8 * 1024 * 1024);
  if (!actual) return std::unexpected(actual.error());
  if (*actual != *base.expected_text) return std::unexpected(conflict(path));
  return atomic_replace(path, bytes);
}

}  // namespace xrm
