#include "profile/profile_service.hpp"

#include "infra/files.hpp"
#include "infra/store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>
#include <set>
#include <system_error>

namespace xrm {
namespace {

constexpr std::size_t max_profile_file_bytes = 8 * 1024 * 1024;

AppError error(ErrorCode code, std::string message, std::optional<std::string> subject = {}) {
  return {code, std::move(message), std::move(subject), std::nullopt};
}

template <class T>
Result<T> fail(ErrorCode code, std::string message,
               std::optional<std::string> subject = {}) {
  return std::unexpected(error(code, std::move(message), std::move(subject)));
}

std::string new_id() {
  static thread_local std::mt19937_64 random(std::random_device{}());
  std::array<unsigned char, 16> bytes{};
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char digits[] = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) id.push_back('-');
    id.push_back(digits[bytes[i] >> 4]);
    id.push_back(digits[bytes[i] & 0x0f]);
  }
  return id;
}

std::string target_kind(TargetKind kind) {
  switch (kind) {
    case TargetKind::node: return "node";
    case TargetKind::group: return "group";
    case TargetKind::native_outbound: return "native_outbound";
    case TargetKind::native_balancer: return "native_balancer";
  }
  return "node";
}

Result<TargetKind> parse_target_kind(std::string_view kind) {
  if (kind == "node") return TargetKind::node;
  if (kind == "group") return TargetKind::group;
  if (kind == "native_outbound") return TargetKind::native_outbound;
  if (kind == "native_balancer") return TargetKind::native_balancer;
  return fail<TargetKind>(ErrorCode::invalid_argument, "Profile target kind 无效");
}

std::filesystem::path preset_root() {
#ifdef XRM_PRESETS_DIR
  return std::filesystem::path(XRM_PRESETS_DIR);
#else
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "presets";
#endif
}

std::string profile_json(const ProfileSettings& value) {
  return Json(value).dump(2) + "\n";
}

Result<std::filesystem::path> checked_module_path(
    const std::filesystem::path& profile_dir, const std::filesystem::path& relative) {
  if (relative.empty() || relative.is_absolute()) {
    return fail<std::filesystem::path>(ErrorCode::invalid_argument,
        "模块文件名必须是 Profile 内的相对路径", relative.string());
  }
  const auto normalized = relative.lexically_normal();
  if (normalized.empty() || normalized == "." ||
      std::any_of(normalized.begin(), normalized.end(), [](const auto& part) {
        return part == "..";
      })) {
    return fail<std::filesystem::path>(ErrorCode::invalid_argument,
        "模块文件名不能越出 Profile 目录", relative.string());
  }
  if (normalized.filename() == "90_nodes_tail.json") {
    return fail<std::filesystem::path>(ErrorCode::conflict,
        "90_nodes_tail.json 是节点运行分片保留名", relative.string());
  }
  if (normalized.extension() != ".json") {
    return fail<std::filesystem::path>(ErrorCode::invalid_argument,
        "原生模块文件必须使用 .json 扩展名", relative.string());
  }
  return profile_dir / "modules" / normalized;
}

Result<void> validate_settings(const ProfileSettings& settings) {
  if (settings.id.empty() || settings.name.empty() || settings.core_id.empty()) {
    return fail<void>(ErrorCode::invalid_argument, "Profile ID、名称与核心 ID 不能为空");
  }
  std::set<std::string> keys;
  std::set<std::string> filenames;
  for (const auto& module : settings.modules) {
    if (!find_module(module.key)) {
      return fail<void>(ErrorCode::invalid_argument, "未知原生模块", module.key);
    }
    if (!keys.insert(module.key).second) {
      return fail<void>(ErrorCode::conflict, "Profile 含重复模块键", module.key);
    }
    if (module.filename.empty() || module.filename.is_absolute()) {
      return fail<void>(ErrorCode::invalid_argument, "模块文件名必须是相对路径", module.key);
    }
    const auto normalized = module.filename.lexically_normal();
    if (normalized.empty() || normalized == "." ||
        std::any_of(normalized.begin(), normalized.end(), [](const auto& part) {
          return part == "..";
        })) {
      return fail<void>(ErrorCode::invalid_argument, "模块文件名不能越出 Profile 目录", module.key);
    }
    if (normalized.extension() != ".json") {
      return fail<void>(ErrorCode::invalid_argument, "原生模块文件必须使用 .json 扩展名", module.key);
    }
    if (normalized.filename() == "90_nodes_tail.json") {
      return fail<void>(ErrorCode::conflict, "模块文件名占用节点分片保留名", module.key);
    }
    if (!filenames.insert(normalized.generic_string()).second) {
      return fail<void>(ErrorCode::conflict, "多个模块映射到同一文件", normalized.string());
    }
  }
  return {};
}

Result<ProfileSettings> read_profile_file(const std::filesystem::path& directory) {
  auto text = read_text(directory / "profile.json", max_profile_file_bytes);
  if (!text) return std::unexpected(text.error());
  try {
    return Json::parse(*text).get<ProfileSettings>();
  } catch (const std::exception& exception) {
    return fail<ProfileSettings>(ErrorCode::invalid_argument,
        "读取 Profile JSON 失败：" + std::string(exception.what()), "profile.json");
  }
}

std::vector<ModuleSetting> default_module_settings() {
  std::vector<ModuleSetting> modules;
  modules.reserve(module_catalog().size());
  for (const auto& descriptor : module_catalog()) {
    modules.push_back({descriptor.key, descriptor.default_filename, false});
  }
  return modules;
}

Result<std::filesystem::path> profile_directory(Store& store, const Id& id) {
  return store.profile_path(id);
}

bool safe_profile_name(std::string_view name) {
  return !name.empty() && std::none_of(name.begin(), name.end(), [](unsigned char ch) {
    return std::iscntrl(ch) != 0;
  });
}

}  // namespace

void to_json(Json& json, const ProfileSettings& value) {
  Json selected = nullptr;
  if (value.selected) {
    selected = Json{{"kind", target_kind(value.selected->kind)},
                    {"id_or_tag", value.selected->id_or_tag}};
  }
  Json asset_dir = nullptr;
  if (value.environment.asset_dir) asset_dir = value.environment.asset_dir->string();
  json = Json{{"id", value.id},
              {"name", value.name},
              {"core_id", value.core_id},
              {"selected", std::move(selected)},
              {"append_selected_default", value.append_selected_default},
              {"environment", Json{{"cwd", value.environment.cwd.string()},
                                    {"process_environment", value.environment.process_environment},
                                    {"asset_dir", std::move(asset_dir)}}},
              {"modules", Json::array()},
              {"additional_node_ids", value.additional_node_ids}};
  for (const auto& module : value.modules) {
    json["modules"].push_back(Json{{"key", module.key},
                                   {"filename", module.filename.generic_string()},
                                   {"included", module.included}});
  }
}

void from_json(const Json& json, ProfileSettings& value) {
  value.id = json.at("id").get<Id>();
  value.name = json.at("name").get<std::string>();
  value.core_id = json.at("core_id").get<Id>();
  const auto& selected = json.at("selected");
  value.selected.reset();
  if (!selected.is_null()) {
    auto kind = parse_target_kind(selected.at("kind").get<std::string>());
    if (!kind) throw std::invalid_argument(kind.error().message);
    value.selected = Target{*kind, selected.at("id_or_tag").get<std::string>()};
  }
  value.append_selected_default = json.at("append_selected_default").get<bool>();
  const auto& environment = json.at("environment");
  value.environment.cwd = environment.at("cwd").get<std::string>();
  value.environment.process_environment =
      environment.at("process_environment").get<std::map<std::string, std::string>>();
  const auto& asset_dir = environment.at("asset_dir");
  value.environment.asset_dir = asset_dir.is_null()
      ? std::nullopt : std::optional<std::filesystem::path>(asset_dir.get<std::string>());
  value.modules.clear();
  for (const auto& module : json.at("modules")) {
    value.modules.push_back({module.at("key").get<std::string>(),
                             module.at("filename").get<std::string>(),
                             module.at("included").get<bool>()});
  }
  value.additional_node_ids = json.at("additional_node_ids").get<std::vector<Id>>();
}

ProfileService::ProfileService(Store& store, std::filesystem::path profiles_root)
    : store_(store), profiles_root_(std::move(profiles_root)) {}

Result<ProfileSettings> ProfileService::create(const ProfileCreate& request) {
  if (!safe_profile_name(request.name) || request.core_id.empty()) {
    return fail<ProfileSettings>(ErrorCode::invalid_argument,
        "Profile 名称与核心 ID 不能为空");
  }
  if (request.preset != "local") {
    return fail<ProfileSettings>(ErrorCode::not_found, "找不到本地 Profile 预设", request.preset);
  }

  const auto preset = preset_root() / "local";
  auto manifest_text = read_text(preset / "modules.json", 64 * 1024);
  if (!manifest_text) return std::unexpected(manifest_text.error());
  Json manifest;
  try { manifest = Json::parse(*manifest_text); }
  catch (const std::exception& exception) {
    return fail<ProfileSettings>(ErrorCode::invalid_argument,
        "local 预设清单无效：" + std::string(exception.what()), "modules.json");
  }
  if (!manifest.is_object() || !manifest.contains("included") ||
      !manifest["included"].is_array()) {
    return fail<ProfileSettings>(ErrorCode::invalid_argument, "local 预设清单缺少 included 列表");
  }

  ProfileSettings settings;
  settings.id = new_id();
  settings.name = request.name;
  settings.core_id = request.core_id;
  settings.append_selected_default = manifest.value("append_selected_default", false);
  settings.environment = request.environment;
  settings.modules = default_module_settings();
  std::set<std::string> included;
  for (const auto& key : manifest["included"]) {
    if (!key.is_string() || !find_module(key.get<std::string>())) {
      return fail<ProfileSettings>(ErrorCode::invalid_argument, "local 预设引用未知模块");
    }
    included.insert(key.get<std::string>());
  }
  for (auto& module : settings.modules) module.included = included.contains(module.key);
  if (auto valid = validate_settings(settings); !valid) return std::unexpected(valid.error());

  std::error_code fs_error;
  std::filesystem::create_directories(profiles_root_, fs_error);
  if (fs_error) return fail<ProfileSettings>(ErrorCode::io_error,
      "创建 Profile 根目录失败：" + fs_error.message());
  const auto final_dir = profiles_root_ / settings.id;
  const auto staging_dir = profiles_root_ / (".create-" + settings.id);
  std::filesystem::create_directories(staging_dir / "modules", fs_error);
  if (fs_error) return fail<ProfileSettings>(ErrorCode::io_error,
      "创建 Profile 暂存目录失败：" + fs_error.message());
  auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(staging_dir, ignored);
  };
  for (const auto& module : settings.modules) {
    if (!module.included) continue;
    const auto source = preset / "modules" / module.filename;
    auto bytes = read_text(source, max_profile_file_bytes);
    if (!bytes) { cleanup(); return std::unexpected(bytes.error()); }
    auto written = replace_text(staging_dir / "modules" / module.filename, *bytes);
    if (!written) { cleanup(); return std::unexpected(written.error()); }
  }
  auto profile_written = replace_text(staging_dir / "profile.json", profile_json(settings));
  if (!profile_written) { cleanup(); return std::unexpected(profile_written.error()); }
  std::filesystem::rename(staging_dir, final_dir, fs_error);
  if (fs_error) { cleanup(); return fail<ProfileSettings>(ErrorCode::io_error,
      "提交 Profile 目录失败：" + fs_error.message()); }
  auto registered = store_.register_profile(settings.id, final_dir);
  if (!registered) {
    std::filesystem::remove_all(final_dir, fs_error);
    return std::unexpected(registered.error());
  }
  return settings;
}

Result<std::vector<ProfileSummary>> ProfileService::list() const {
  auto ids = store_.profile_ids();
  if (!ids) return std::unexpected(ids.error());
  std::vector<ProfileSummary> summaries;
  summaries.reserve(ids->size());
  for (const auto& id : *ids) {
    auto settings = get(id);
    if (!settings) return std::unexpected(settings.error());
    summaries.push_back({settings->id, settings->name, settings->core_id});
  }
  return summaries;
}

Result<ProfileSettings> ProfileService::get(const Id& id) const {
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  auto settings = read_profile_file(*directory);
  if (!settings) return std::unexpected(settings.error());
  if (settings->id != id) return fail<ProfileSettings>(ErrorCode::conflict,
      "Profile 文件 ID 与索引不一致", id);
  if (auto valid = validate_settings(*settings); !valid) return std::unexpected(valid.error());
  return settings;
}

Result<void> ProfileService::save(const Id& id, const ProfileEdit& edit) {
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  if (edit.value.id != id) return fail<void>(ErrorCode::invalid_argument,
      "Profile ID 不可通过编辑修改", id);
  if (auto valid = validate_settings(edit.value); !valid) return valid;
  auto current_text = read_text(*directory / "profile.json", max_profile_file_bytes);
  if (!current_text) return std::unexpected(current_text.error());
  if (!edit.base.expected_text || *edit.base.expected_text != *current_text) {
    return fail<void>(ErrorCode::conflict, "profile.json 已被外部修改", "profile.json");
  }
  return write_if_unchanged(*directory / "profile.json", edit.base, profile_json(edit.value));
}

Result<ProfileSettings> ProfileService::clone(const Id& id, std::string new_name) {
  if (!safe_profile_name(new_name)) return fail<ProfileSettings>(ErrorCode::invalid_argument,
      "Profile 名称不能为空或包含控制字符");
  auto original = get(id);
  if (!original) return std::unexpected(original.error());
  auto source_dir = profile_directory(store_, id);
  if (!source_dir) return std::unexpected(source_dir.error());
  auto clone = *original;
  clone.id = new_id();
  clone.name = std::move(new_name);
  const auto staging = profiles_root_ / (".clone-" + clone.id);
  const auto destination = profiles_root_ / clone.id;
  std::error_code fs_error;
  std::filesystem::create_directories(profiles_root_, fs_error);
  if (fs_error) return fail<ProfileSettings>(ErrorCode::io_error,
      "创建 Profile 根目录失败：" + fs_error.message());
  std::filesystem::copy(*source_dir, staging,
      std::filesystem::copy_options::recursive, fs_error);
  if (fs_error) {
    std::filesystem::remove_all(staging, fs_error);
    return fail<ProfileSettings>(ErrorCode::io_error,
        "复制 Profile 文件失败：" + fs_error.message());
  }
  auto saved = replace_text(staging / "profile.json", profile_json(clone));
  if (!saved) { std::filesystem::remove_all(staging, fs_error); return std::unexpected(saved.error()); }
  std::filesystem::rename(staging, destination, fs_error);
  if (fs_error) { std::filesystem::remove_all(staging, fs_error); return fail<ProfileSettings>(
      ErrorCode::io_error, "提交副本 Profile 失败：" + fs_error.message()); }
  auto registered = store_.register_profile(clone.id, destination);
  if (!registered) { std::filesystem::remove_all(destination, fs_error); return std::unexpected(registered.error()); }
  return clone;
}

Result<void> ProfileService::remove(const Id& id) {
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  const auto tombstone = directory->parent_path() / (".remove-" + id);
  std::error_code fs_error;
  std::filesystem::rename(*directory, tombstone, fs_error);
  if (fs_error) return fail<void>(ErrorCode::io_error,
      "暂存待删除 Profile 失败：" + fs_error.message(), id);
  auto unregistered = store_.unregister_profile(id);
  if (!unregistered) {
    std::filesystem::rename(tombstone, *directory, fs_error);
    return std::unexpected(unregistered.error());
  }
  std::filesystem::remove_all(tombstone, fs_error);
  if (fs_error) return fail<void>(ErrorCode::io_error,
      "Profile 索引已移除但目录清理失败：" + fs_error.message(), id);
  return {};
}

Result<std::vector<ModuleView>> ProfileService::modules(const Id& id) const {
  auto settings = get(id);
  if (!settings) return std::unexpected(settings.error());
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  std::vector<ModuleView> views;
  views.reserve(module_catalog().size());
  for (const auto& descriptor : module_catalog()) {
    const auto setting = std::find_if(settings->modules.begin(), settings->modules.end(),
        [&](const ModuleSetting& item) { return item.key == descriptor.key; });
    if (setting == settings->modules.end()) return fail<std::vector<ModuleView>>(
        ErrorCode::invalid_argument, "Profile 缺少模块设置", descriptor.key);
    ModuleView view{descriptor, *setting, std::nullopt, "native"};
    auto source = checked_module_path(*directory, setting->filename);
    if (!source) return std::unexpected(source.error());
    std::error_code fs_error;
    const bool exists = std::filesystem::exists(*source, fs_error);
    if (fs_error) return fail<std::vector<ModuleView>>(ErrorCode::io_error,
        "检查模块源文件失败：" + fs_error.message(), descriptor.key);
    if (exists) {
      auto bytes = read_text(*source, max_profile_file_bytes);
      if (!bytes) return std::unexpected(bytes.error());
      view.document = ModuleDocument{descriptor.key, setting->filename, std::move(*bytes)};
    }
    views.push_back(std::move(view));
  }
  return views;
}

Result<ModuleView> ProfileService::module(const Id& id, std::string_view key) const {
  if (!find_module(key)) return fail<ModuleView>(ErrorCode::not_found, "未知原生模块", std::string(key));
  auto views = modules(id);
  if (!views) return std::unexpected(views.error());
  const auto found = std::find_if(views->begin(), views->end(),
      [&](const ModuleView& view) { return view.descriptor.key == key; });
  if (found == views->end()) return fail<ModuleView>(ErrorCode::not_found,
      "Profile 中没有该模块", std::string(key));
  return *found;
}

Result<void> ProfileService::save_module(const Id& id, const ModuleEdit& edit) {
  const auto* descriptor = find_module(edit.key);
  if (!descriptor) return fail<void>(ErrorCode::not_found, "未知原生模块", edit.key);
  auto settings = get(id);
  if (!settings) return std::unexpected(settings.error());
  const auto item = std::find_if(settings->modules.begin(), settings->modules.end(),
      [&](const ModuleSetting& module) { return module.key == edit.key; });
  if (item == settings->modules.end()) return fail<void>(ErrorCode::not_found,
      "Profile 中没有该模块设置", edit.key);
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  auto path = checked_module_path(*directory, item->filename);
  if (!path) return std::unexpected(path.error());
  return write_if_unchanged(*path, edit.base, edit.text);
}

Result<void> ProfileService::include_module(const Id& id, std::string_view key, bool included) {
  auto settings = get(id);
  if (!settings) return std::unexpected(settings.error());
  auto module = std::find_if(settings->modules.begin(), settings->modules.end(),
      [&](const ModuleSetting& item) { return item.key == key; });
  if (module == settings->modules.end()) return fail<void>(ErrorCode::not_found,
      "Profile 中没有该模块设置", std::string(key));
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  auto path = checked_module_path(*directory, module->filename);
  if (!path) return std::unexpected(path.error());
  bool created_document = false;
  std::error_code fs_error;
  if (included && !std::filesystem::exists(*path, fs_error)) {
    if (fs_error) return fail<void>(ErrorCode::io_error,
        "检查模块源文件失败：" + fs_error.message(), std::string(key));
    if (key != "stats") return fail<void>(ErrorCode::invalid_argument,
        "启用模块前请先保存原生内容", std::string(key));
    auto created = write_if_unchanged(*path, EditBase{}, "{\"stats\":{}}\n");
    if (!created) return created;
    created_document = true;
  }
  if (module->included == included) return {};
  const auto profile_file = *directory / "profile.json";
  auto base = read_text(profile_file, max_profile_file_bytes);
  if (!base) {
    if (created_document) std::filesystem::remove(*path, fs_error);
    return std::unexpected(base.error());
  }
  module->included = included;
  auto saved = write_if_unchanged(profile_file, EditBase{*base}, profile_json(*settings));
  if (!saved && created_document) std::filesystem::remove(*path, fs_error);
  return saved;
}

Result<void> ProfileService::rename_module_file(
    const Id& id, std::string_view key, const std::filesystem::path& relative_name) {
  auto settings = get(id);
  if (!settings) return std::unexpected(settings.error());
  auto module = std::find_if(settings->modules.begin(), settings->modules.end(),
      [&](const ModuleSetting& item) { return item.key == key; });
  if (module == settings->modules.end()) return fail<void>(ErrorCode::not_found,
      "Profile 中没有该模块设置", std::string(key));
  auto directory = profile_directory(store_, id);
  if (!directory) return std::unexpected(directory.error());
  auto destination = checked_module_path(*directory, relative_name);
  if (!destination) return std::unexpected(destination.error());
  const auto normalized = relative_name.lexically_normal();
  for (const auto& other : settings->modules) {
    if (other.key != key && other.filename.lexically_normal() == normalized) {
      return fail<void>(ErrorCode::conflict, "多个模块映射到同一文件", normalized.string());
    }
  }
  if (module->filename.lexically_normal() == normalized) return {};
  auto source = checked_module_path(*directory, module->filename);
  if (!source) return std::unexpected(source.error());
  std::error_code fs_error;
  if (std::filesystem::exists(*destination, fs_error)) {
    return fail<void>(ErrorCode::conflict, "目标模块文件已存在", normalized.string());
  }
  if (fs_error) return fail<void>(ErrorCode::io_error,
      "检查模块目标文件失败：" + fs_error.message(), normalized.string());
  const bool source_exists = std::filesystem::exists(*source, fs_error);
  if (fs_error) return fail<void>(ErrorCode::io_error,
      "检查模块源文件失败：" + fs_error.message(), module->key);
  bool copied = false;
  if (source_exists) {
    auto bytes = read_text(*source, max_profile_file_bytes);
    if (!bytes) return std::unexpected(bytes.error());
    auto created = write_if_unchanged(*destination, EditBase{}, *bytes);
    if (!created) return created;
    copied = true;
  }
  auto profile_text = read_text(*directory / "profile.json", max_profile_file_bytes);
  if (!profile_text) {
    if (copied) std::filesystem::remove(*destination, fs_error);
    return std::unexpected(profile_text.error());
  }
  module->filename = normalized;
  auto saved = write_if_unchanged(*directory / "profile.json", EditBase{*profile_text},
                                  profile_json(*settings));
  if (!saved) {
    if (copied) std::filesystem::remove(*destination, fs_error);
    return saved;
  }
  if (source_exists) {
    std::filesystem::remove(*source, fs_error);
    if (fs_error) return fail<void>(ErrorCode::io_error,
        "模块路径已更新但旧文件清理失败：" + fs_error.message(), module->key);
  }
  return {};
}

Result<ModuleSnapshot> ProfileService::snapshot(const Id& id) const {
  auto settings = get(id);
  if (!settings) return std::unexpected(settings.error());
  auto views = modules(id);
  if (!views) return std::unexpected(views.error());
  ModuleSnapshot snapshot;
  snapshot.profile = std::move(*settings);
  for (auto& view : *views) {
    if (view.document) snapshot.modules.push_back(std::move(*view.document));
  }
  return snapshot;
}

}  // namespace xrm
