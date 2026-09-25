#include "xray/runtime_writer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace xrm {
namespace {

AppError io_error(std::string message, const std::filesystem::path& path) {
  return {ErrorCode::io_error, std::move(message), path.string(), std::nullopt};
}

AppError invalid_path(std::string message, const std::filesystem::path& path) {
  return {ErrorCode::invalid_argument, std::move(message), path.string(), std::nullopt};
}

bool valid_component(const std::string& value) {
  if (value.empty() || value == "." || value == "..") return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
  });
}

bool valid_filename(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return !name.empty() && name != "." && name != ".." &&
         !path.is_absolute() && !path.has_root_name() && !path.has_parent_path() &&
         name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

const char* purpose_name(DirectoryPurpose purpose) {
  switch (purpose) {
    case DirectoryPurpose::preview: return "preview";
    case DirectoryPurpose::candidate: return "candidate";
  }
  return nullptr;
}

Result<std::filesystem::path> absolute_normal(const std::filesystem::path& path) {
  std::error_code ec;
  auto result = std::filesystem::absolute(path, ec);
  if (ec) return std::unexpected(io_error("Could not resolve path", path));
  return result.lexically_normal();
}

Result<void> set_directory_permissions(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  if (ec) return std::unexpected(io_error("Could not restrict directory permissions", path));
  return {};
}

Result<void> set_file_permissions(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ec);
  if (ec) return std::unexpected(io_error("Could not restrict file permissions", path));
  return {};
}

Result<void> require_directory(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::exists(status) ||
      std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
    return std::unexpected(io_error("Expected an owned regular directory", path));
  }
  return {};
}

void remove_tree_ignoring_errors(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

bool path_is_within(const std::filesystem::path& child,
                    const std::filesystem::path& parent) {
  const auto relative = child.lexically_relative(parent);
  return !relative.empty() && !relative.is_absolute() &&
         *relative.begin() != "..";
}

}  // namespace

RuntimeWriter::RuntimeWriter(std::filesystem::path data_root)
    : data_root_(std::move(data_root)) {}

Result<PreparedDirectory> RuntimeWriter::materialize(const PreparedFiles& files,
                                                     DirectoryPurpose purpose,
                                                     const Id& id,
                                                     std::span<const AssetSnapshot> assets) {
  const auto purpose_text = purpose_name(purpose);
  if (!purpose_text) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "Unknown runtime directory purpose", id,
                                    std::nullopt});
  }
  if (!valid_component(id)) {
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "Runtime directory id is not a path component", id,
                                    std::nullopt});
  }

  std::set<std::string> names;
  for (const auto& file : files.files) {
    if (!valid_filename(file.relative_name)) {
      return std::unexpected(invalid_path("Runtime output must be a plain filename",
                                          file.relative_name));
    }
    if (!names.insert(file.relative_name.string()).second) {
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Runtime FileSet contains a duplicate filename",
                                      file.relative_name.string(), std::nullopt});
    }
  }
  for (const auto& asset : assets) {
    if (!valid_filename(asset.runtime_name)) {
      return std::unexpected(invalid_path("Runtime asset name must be a plain filename",
                                          asset.runtime_name));
    }
    if (!names.insert(asset.runtime_name).second) {
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Runtime asset filename conflicts with another output",
                                      asset.runtime_name, std::nullopt});
    }
    std::error_code asset_ec;
    const auto status = std::filesystem::symlink_status(asset.immutable_source, asset_ec);
    if (asset_ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
      return std::unexpected(io_error("Runtime asset source must be a regular file",
                                      asset.immutable_source));
    }
  }

  auto root_result = absolute_normal(data_root_);
  if (!root_result) return std::unexpected(root_result.error());
  const auto root = *root_result;
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) return std::unexpected(io_error("Could not create runtime data root", root));

  auto runtime_root = root / "runtime";
  if (!std::filesystem::exists(runtime_root, ec)) {
    ec.clear();
    if (!std::filesystem::create_directory(runtime_root, ec) && ec) {
      return std::unexpected(io_error("Could not create runtime directory", runtime_root));
    }
  }
  auto runtime_check = require_directory(runtime_root);
  if (!runtime_check) return std::unexpected(runtime_check.error());
  auto runtime_perms = set_directory_permissions(runtime_root);
  if (!runtime_perms) return std::unexpected(runtime_perms.error());

  const auto purpose_root = runtime_root / purpose_text;
  ec.clear();
  if (!std::filesystem::exists(purpose_root, ec)) {
    ec.clear();
    if (!std::filesystem::create_directory(purpose_root, ec) && ec) {
      return std::unexpected(io_error("Could not create runtime purpose directory",
                                      purpose_root));
    }
  }
  auto purpose_check = require_directory(purpose_root);
  if (!purpose_check) return std::unexpected(purpose_check.error());
  auto purpose_perms = set_directory_permissions(purpose_root);
  if (!purpose_perms) return std::unexpected(purpose_perms.error());

  const auto confdir = purpose_root / id;
  ec.clear();
  if (!std::filesystem::create_directory(confdir, ec)) {
    if (!ec) {
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Runtime directory already exists", id,
                                      std::nullopt});
    }
    return std::unexpected(io_error("Could not create a fresh runtime directory", confdir));
  }
  auto directory_perms = set_directory_permissions(confdir);
  if (!directory_perms) {
    remove_tree_ignoring_errors(confdir);
    return std::unexpected(directory_perms.error());
  }

  for (const auto& file : files.files) {
    const auto destination = confdir / file.relative_name;
    std::ofstream output(destination, std::ios::binary | std::ios::out);
    if (!output) {
      remove_tree_ignoring_errors(confdir);
      return std::unexpected(io_error("Could not create runtime file", destination));
    }
    output.write(file.bytes.data(), static_cast<std::streamsize>(file.bytes.size()));
    output.flush();
    if (!output) {
      output.close();
      remove_tree_ignoring_errors(confdir);
      return std::unexpected(io_error("Could not write runtime file", destination));
    }
    output.close();
    if (!output) {
      remove_tree_ignoring_errors(confdir);
      return std::unexpected(io_error("Could not finish runtime file", destination));
    }
    auto file_perms = set_file_permissions(destination);
    if (!file_perms) {
      remove_tree_ignoring_errors(confdir);
      return std::unexpected(file_perms.error());
    }
  }

  for (const auto& asset : assets) {
    const auto destination = confdir / asset.runtime_name;
    ec.clear();
    std::filesystem::copy_file(asset.immutable_source, destination,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
      remove_tree_ignoring_errors(confdir);
      return std::unexpected(io_error("Could not snapshot runtime asset", destination));
    }
    auto asset_perms = set_file_permissions(destination);
    if (!asset_perms) {
      remove_tree_ignoring_errors(confdir);
      return std::unexpected(asset_perms.error());
    }
  }

  return PreparedDirectory{id, purpose, confdir, files.index};
}

Result<void> RuntimeWriter::export_files(const std::filesystem::path& confdir,
                                         const std::filesystem::path& new_destination) {
  auto source_result = absolute_normal(confdir);
  if (!source_result) return std::unexpected(source_result.error());
  auto destination_result = absolute_normal(new_destination);
  if (!destination_result) return std::unexpected(destination_result.error());
  const auto source = *source_result;
  const auto destination = *destination_result;
  if (source == destination || path_is_within(source, destination) ||
      path_is_within(destination, source)) {
    return std::unexpected(invalid_path("Export source and destination overlap", destination));
  }
  auto source_check = require_directory(source);
  if (!source_check) return std::unexpected(source_check.error());

  std::vector<std::filesystem::path> source_files;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec)) {
    const auto entry_status = it->symlink_status(ec);
    if (ec || std::filesystem::is_symlink(entry_status) ||
        !std::filesystem::is_regular_file(entry_status) ||
        !valid_filename(it->path().filename())) {
      return std::unexpected(io_error("Export source contains a non-regular entry",
                                      it->path()));
    }
    source_files.push_back(it->path());
  }
  if (ec) return std::unexpected(io_error("Could not enumerate export source", source));
  std::sort(source_files.begin(), source_files.end());

  const auto parent = destination.parent_path();
  std::filesystem::create_directories(parent, ec);
  if (ec) return std::unexpected(io_error("Could not create export parent", parent));
  const auto dest_status = std::filesystem::symlink_status(destination, ec);
  if (!ec && std::filesystem::exists(dest_status)) {
    if (std::filesystem::is_symlink(dest_status) || !std::filesystem::is_directory(dest_status)) {
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Export destination is not a regular directory",
                                      destination.string(), std::nullopt});
    }
    if (std::filesystem::directory_iterator(destination) !=
        std::filesystem::directory_iterator{}) {
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Export destination is not empty",
                                      destination.string(), std::nullopt});
    }
  } else if (ec != std::errc::no_such_file_or_directory) {
    return std::unexpected(io_error("Could not inspect export destination", destination));
  }
  ec.clear();

  std::filesystem::path staging;
  bool created_staging = false;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 16; ++attempt) {
    staging = parent / (".xrm-export-" + std::to_string(stamp) + "-" +
                        std::to_string(attempt));
    ec.clear();
    if (std::filesystem::create_directory(staging, ec)) {
      created_staging = true;
      break;
    }
    if (ec && ec != std::errc::file_exists) break;
  }
  if (!created_staging) {
    return std::unexpected(io_error("Could not create export staging directory", staging));
  }
  auto staging_perms = set_directory_permissions(staging);
  if (!staging_perms) {
    remove_tree_ignoring_errors(staging);
    return std::unexpected(staging_perms.error());
  }

  for (const auto& source_file : source_files) {
    const auto target = staging / source_file.filename();
    ec.clear();
    std::filesystem::copy_file(source_file, target,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
      remove_tree_ignoring_errors(staging);
      return std::unexpected(io_error("Could not copy exported runtime file", target));
    }
    auto file_perms = set_file_permissions(target);
    if (!file_perms) {
      remove_tree_ignoring_errors(staging);
      return std::unexpected(file_perms.error());
    }
  }

  if (!std::filesystem::exists(destination, ec)) {
    ec.clear();
    std::filesystem::rename(staging, destination, ec);
    if (ec) {
      remove_tree_ignoring_errors(staging);
      return std::unexpected(io_error("Could not publish exported files", destination));
    }
    return {};
  }

  std::vector<std::filesystem::path> copied;
  for (const auto& source_file : source_files) {
    const auto target = destination / source_file.filename();
    ec.clear();
    std::filesystem::copy_file(staging / source_file.filename(), target,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
      for (const auto& created : copied) {
        std::error_code ignored;
        std::filesystem::remove(created, ignored);
      }
      remove_tree_ignoring_errors(staging);
      return std::unexpected(io_error("Could not populate export destination", target));
    }
    copied.push_back(target);
    auto file_perms = set_file_permissions(target);
    if (!file_perms) {
      for (const auto& created : copied) {
        std::error_code ignored;
        std::filesystem::remove(created, ignored);
      }
      remove_tree_ignoring_errors(staging);
      return std::unexpected(file_perms.error());
    }
  }
  remove_tree_ignoring_errors(staging);
  return {};
}

Result<void> RuntimeWriter::discard(const PreparedDirectory& prepared,
                                    std::span<const Id> protected_run_ids) {
  const auto purpose_text = purpose_name(prepared.purpose);
  if (!purpose_text || !valid_component(prepared.id)) {
    return std::unexpected(invalid_path("Prepared directory identity is invalid",
                                        prepared.confdir));
  }
  if (std::find(protected_run_ids.begin(), protected_run_ids.end(), prepared.id) !=
      protected_run_ids.end()) {
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "Runtime directory is protected by a live run",
                                    prepared.id, std::nullopt});
  }

  auto root_result = absolute_normal(data_root_);
  if (!root_result) return std::unexpected(root_result.error());
  const auto expected = (*root_result / "runtime" / purpose_text / prepared.id)
                            .lexically_normal();
  auto actual_result = absolute_normal(prepared.confdir);
  if (!actual_result) return std::unexpected(actual_result.error());
  if (*actual_result != expected) {
    return std::unexpected(invalid_path("Refusing to discard a directory outside this writer",
                                        prepared.confdir));
  }

  for (const auto& path : {*root_result, *root_result / "runtime",
                           *root_result / "runtime" / purpose_text, expected}) {
    auto check = require_directory(path);
    if (!check) return std::unexpected(check.error());
  }
  std::error_code ec;
  std::filesystem::remove_all(expected, ec);
  if (ec) return std::unexpected(io_error("Could not discard runtime directory", expected));
  return {};
}

}  // namespace xrm
