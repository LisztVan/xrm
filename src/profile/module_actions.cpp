// XRM: Applies single-module Source edits and enabled-state changes.

#include "profile/forms.hpp"

#include <algorithm>
#include <set>

namespace xrm {
namespace {

const ModuleDocument *find_document(const ProfileSnapshot &snapshot,
                                    std::string_view key) {
  const auto found = std::find_if(
      snapshot.modules.begin(), snapshot.modules.end(),
      [&](const ModuleDocument &document) { return document.key == key; });
  return found == snapshot.modules.end() ? nullptr : &*found;
}

const ModuleSetting *find_setting(const ProfileSettings &profile,
                                  std::string_view key) {
  const auto found = std::find_if(
      profile.modules.begin(), profile.modules.end(),
      [&](const ModuleSetting &setting) { return setting.key == key; });
  return found == profile.modules.end() ? nullptr : &*found;
}

std::optional<std::string> text_of(const ModuleDocument *document) {
  if (document)
    return document->source_text;
  return std::nullopt;
}

ModuleDocument empty_document(std::string key, std::string root) {
  return {key, {}, Json{{std::move(root), Json::object()}}.dump()};
}

Result<ModuleEdit> edit_policy(const ProfileSnapshot &snapshot, bool enabled) {
  const auto *source = find_document(snapshot, "policy");
  if (!source && !enabled)
    return ModuleEdit{"policy", {}, ""};
  auto document = source ? *source : empty_document("policy", "policy");
  auto edit = set_policy_counters(document, enabled, enabled, enabled, enabled);
  return edit;
}

Result<ModuleEdit> edit_stats(const ProfileSnapshot &snapshot, bool enabled) {
  const auto *source = find_document(snapshot, "stats");
  if (!source && !enabled)
    return ModuleEdit{"stats", {}, ""};
  auto document = source ? *source : empty_document("stats", "stats");
  if (!enabled)
    return ModuleEdit{"stats", {text_of(source)}, source->source_text};
  auto parsed = Json::parse(document.source_text);
  if (!parsed.is_object() || !parsed.contains("stats") ||
      !parsed["stats"].is_object())
    return std::unexpected(AppError{
        ErrorCode::invalid_argument, "stats 原生内容不是对象；请使用原生编辑器",
        "stats", "跨模块统计动作不会重建已有 stats 结构"});
  return ModuleEdit{"stats", {text_of(source)}, parsed.dump(2) + "\n"};
}

Result<ModuleEdit> edit_metrics(const ProfileSnapshot &snapshot, bool enabled,
                                const std::string &listen) {
  const auto *source = find_document(snapshot, "metrics");
  if (!source && !enabled)
    return ModuleEdit{"metrics", {}, ""};
  auto document = source ? *source : empty_document("metrics", "metrics");
  if (!enabled)
    return ModuleEdit{"metrics", {text_of(source)}, source->source_text};
  return set_metrics_listener(document, listen);
}

} // namespace

Result<ModuleChangePlan> plan_traffic_stats(const ProfileSnapshot &snapshot,
                                            bool enabled,
                                            std::string metrics_listen) {
  if (enabled && metrics_listen.empty())
    return std::unexpected(AppError{ErrorCode::invalid_argument,
                                    "启用统计时必须提供 metrics.listen",
                                    "metrics", std::nullopt});
  for (const auto key : {"stats", "policy", "metrics"}) {
    if (!find_setting(snapshot.profile, key))
      return std::unexpected(AppError{ErrorCode::not_found,
                                      "Profile 缺少统计相关原生模块设置", key,
                                      std::nullopt});
  }

  auto stats = edit_stats(snapshot, enabled);
  if (!stats)
    return std::unexpected(stats.error());
  auto policy = edit_policy(snapshot, enabled);
  if (!policy)
    return std::unexpected(policy.error());
  auto metrics = edit_metrics(snapshot, enabled, metrics_listen);
  if (!metrics)
    return std::unexpected(metrics.error());

  ModuleChangePlan plan;
  plan.changes.push_back({std::move(*stats), enabled});
  plan.changes.push_back({std::move(*policy), enabled});
  plan.changes.push_back({std::move(*metrics), enabled});
  plan.notices.push_back(
      {"native_module_plan", enabled ? "将分别保存 stats、policy 与 metrics "
                                       "原生模块；API 模块不会自动修改。"
                                     : "将分别停用 stats、policy 与 metrics "
                                       "原生模块；现有原生内容会保留。"});
  return plan;
}

SaveManyResult save_module_changes(ProfileService &profiles,
                                   const Id &profile_id,
                                   const ModuleChangePlan &plan) {
  SaveManyResult result;
  std::set<std::string> seen;
  for (const auto &change : plan.changes) {
    const auto &edit = change.edit;
    if (!find_module(edit.key)) {
      result.failure =
          AppError{ErrorCode::invalid_argument, "表单计划包含未知原生模块",
                   edit.key, std::nullopt};
      return result;
    }
    if (!seen.insert(edit.key).second) {
      result.failure =
          AppError{ErrorCode::invalid_argument, "表单计划包含重复原生模块",
                   edit.key, std::nullopt};
      return result;
    }
  }

  for (const auto &change : plan.changes) {
    const auto &edit = change.edit;
    auto before = profiles.get(profile_id);
    if (!before) {
      result.failure = before.error();
      return result;
    }
    const auto *setting = find_setting(*before, edit.key);
    if (!setting) {
      result.failure =
          AppError{ErrorCode::not_found, "Profile 中没有该模块设置", edit.key,
                   std::nullopt};
      return result;
    }
    const bool inclusion_changed =
        setting->included != change.include_after_save;
    // An empty edit with no base represents a module whose content is not being
    // created (disable on a never-configured profile). It only changes enabled.
    const bool no_source_creation =
        !edit.base.expected_text && edit.text.empty();
    if (!no_source_creation) {
      auto saved = profiles.save_module(profile_id, edit);
      if (!saved) {
        auto failure = saved.error();
        if (!failure.subject || failure.subject->starts_with("/"))
          failure.subject = edit.key;
        result.failure = std::move(failure);
        return result;
      }
      if (edit.base.expected_text != std::optional<std::string>{edit.text})
        result.saved.push_back(edit.key);
    }

    auto included = profiles.include_module(profile_id, edit.key,
                                            change.include_after_save);
    if (!included) {
      auto failure = included.error();
      failure.subject = edit.key;
      result.failure = std::move(failure);
      return result;
    }
    if (inclusion_changed && std::find(result.saved.begin(), result.saved.end(),
                                       edit.key) == result.saved.end())
      result.saved.push_back(edit.key);
  }
  return result;
}

} // namespace xrm
