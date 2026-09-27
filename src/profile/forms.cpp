// XRM: Applies bounded edits to native module JSON in memory.
// Helpers return ModuleEdit with the original Source baseline; the caller
// chooses when to save it. Unknown native fields outside edited paths survive.

#include "profile/forms.hpp"

#include <algorithm>

namespace xrm {
namespace {

AppError invalid(std::string message, std::string_view key) {
  return {ErrorCode::invalid_argument, std::move(message), std::string(key),
          "此内容不能安全地使用常用表单修改；请改用原生编辑器"};
}

Result<Json> parse_document(const ModuleDocument &document) {
  try {
    auto root = Json::parse(document.source_text);
    if (!root.is_object())
      return std::unexpected(
          invalid("原生模块文档必须是 JSON 对象", document.key));
    if (!root.contains(document.key))
      return std::unexpected(invalid("文档缺少对应的原生根键", document.key));
    return root;
  } catch (const std::exception &exception) {
    return std::unexpected(
        invalid("无法安全解析原生 JSON：" + std::string(exception.what()),
                document.key));
  }
}

template <class Fn>
Result<ModuleEdit> update_object(const ModuleDocument &document,
                                 std::string_view expected_key, Fn &&update) {
  if (document.key != expected_key)
    return std::unexpected(invalid("表单与原生模块键不匹配", document.key));
  auto parsed = parse_document(document);
  if (!parsed)
    return std::unexpected(parsed.error());
  auto &native = (*parsed)[document.key];
  if (!native.is_object())
    return std::unexpected(
        invalid("该原生模块值不是对象，表单不会重建其结构", document.key));
  try {
    std::forward<Fn>(update)(native);
  } catch (const std::exception &exception) {
    return std::unexpected(
        invalid("原生字段类型不符合表单预期：" + std::string(exception.what()),
                document.key));
  }
  return ModuleEdit{
      document.key, {document.source_text}, parsed->dump(2) + "\n"};
}

Result<ModuleEdit> update_fakedns(const ModuleDocument &document, Json value) {
  if (document.key != "fakedns")
    return std::unexpected(invalid("表单与原生模块键不匹配", document.key));
  if (!value.is_object() && !value.is_array())
    return std::unexpected(
        invalid("FakeDNS 仅接受原生对象或数组", document.key));
  auto parsed = parse_document(document);
  if (!parsed)
    return std::unexpected(parsed.error());
  auto &native = (*parsed)["fakedns"];
  if (!native.is_object() && !native.is_array())
    return std::unexpected(invalid(
        "现有 FakeDNS 值不是对象或数组；请先使用原生编辑器", document.key));
  native = std::move(value);
  return ModuleEdit{
      document.key, {document.source_text}, parsed->dump(2) + "\n"};
}

Result<Json> module_array(const ModuleDocument &document,
                          CollectionKind collection) {
  auto parsed = parse_document(document);
  if (!parsed)
    return std::unexpected(parsed.error());
  std::string key;
  std::string array_key;
  switch (collection) {
  case CollectionKind::inbounds:
    key = "inbounds";
    array_key = "inbounds";
    break;
  case CollectionKind::outbounds:
    key = "outbounds";
    array_key = "outbounds";
    break;
  case CollectionKind::route_rules:
    key = "routing";
    array_key = "rules";
    break;
  case CollectionKind::native_balancers:
    key = "routing";
    array_key = "balancers";
    break;
  }
  if (document.key != key)
    return std::unexpected(invalid("列表操作与原生模块键不匹配", document.key));
  auto &native = (*parsed)[key];
  if (collection == CollectionKind::inbounds ||
      collection == CollectionKind::outbounds) {
    if (!native.is_array())
      return std::unexpected(
          invalid("目标原生列表不存在或不是数组；请使用原生编辑器检查文档",
                  document.key));
  } else if (!native.is_object() || !native.contains(array_key) ||
             !native[array_key].is_array()) {
    return std::unexpected(
        invalid("目标原生列表不存在或不是数组；请使用原生编辑器检查文档",
                document.key));
  }
  return *parsed;
}

std::optional<std::string> tag_of(const Json &value) {
  if (!value.is_object() || !value.contains("tag") || !value["tag"].is_string())
    return std::nullopt;
  return value["tag"].get<std::string>();
}

bool duplicate_tag(const Json &values, const Json &value,
                   std::optional<std::size_t> except = {}) {
  const auto candidate = tag_of(value);
  if (!candidate)
    return false;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (except && index == *except)
      continue;
    if (tag_of(values[index]) == candidate)
      return true;
  }
  return false;
}

} // namespace

Result<ModuleEdit> edit_json_draft(const ModuleDocument &document,
                                   const JsonDraftEdit &edit) {
  // Limit JSON-pointer edits to this native wrapper. The returned text is a
  // candidate Source edit, not an implicit save or runtime update.
  auto parsed = parse_document(document);
  if (!parsed)
    return std::unexpected(parsed.error());
  const auto root_prefix = "/" + document.key;
  const bool native_root_array = edit.pointer == root_prefix &&
                                 (edit.action == JsonEditAction::array_insert ||
                                  edit.action == JsonEditAction::array_erase ||
                                  edit.action == JsonEditAction::array_copy ||
                                  edit.action == JsonEditAction::array_move);
  if (!native_root_array &&
      (edit.pointer.size() <= root_prefix.size() ||
       edit.pointer.compare(0, root_prefix.size(), root_prefix) != 0 ||
       edit.pointer[root_prefix.size()] != '/')) {
    return std::unexpected(
        invalid("结构编辑只能修改当前原生模块内部字段", document.key));
  }
  try {
    const auto pointer = Json::json_pointer(edit.pointer);
    if (edit.action == JsonEditAction::set ||
        edit.action == JsonEditAction::erase ||
        edit.action == JsonEditAction::rename_key) {
      auto &parent = parsed->at(pointer.parent_pointer());
      if (!parent.is_object())
        return std::unexpected(
            invalid("字段的父级必须是 JSON 对象", document.key));
      const auto &key = pointer.back();
      if (edit.action == JsonEditAction::set) {
        if (!edit.value)
          return std::unexpected(
              invalid("设置字段需要带类型的 JSON 值", document.key));
        parent[key] = *edit.value;
      } else {
        if (!parent.contains(key))
          return std::unexpected(invalid("要修改的字段不存在", document.key));
        if (edit.action == JsonEditAction::erase)
          parent.erase(key);
        else {
          if (!edit.new_key || edit.new_key->empty() ||
              parent.contains(*edit.new_key))
            return std::unexpected(
                invalid("新字段名为空或已存在", document.key));
          auto value = std::move(parent[key]);
          parent.erase(key);
          parent[*edit.new_key] = std::move(value);
        }
      }
    } else {
      auto &values = parsed->at(pointer);
      if (!values.is_array() || !edit.index)
        return std::unexpected(
            invalid("数组操作需要数组路径和位置", document.key));
      const auto size = values.size();
      const auto position = *edit.index;
      switch (edit.action) {
      case JsonEditAction::array_replace:
        if (position >= size || !edit.value)
          return std::unexpected(
              invalid("替换需要有效位置和 JSON 值", document.key));
        values[position] = *edit.value;
        break;
      case JsonEditAction::array_insert:
        if (position > size || !edit.value)
          return std::unexpected(
              invalid("插入需要有效位置和 JSON 值", document.key));
        values.insert(values.begin() +
                          static_cast<Json::difference_type>(position),
                      *edit.value);
        break;
      case JsonEditAction::array_erase:
        if (position >= size)
          return std::unexpected(invalid("删除位置超出数组", document.key));
        values.erase(values.begin() +
                     static_cast<Json::difference_type>(position));
        break;
      case JsonEditAction::array_copy: {
        if (position >= size || !edit.destination || *edit.destination > size)
          return std::unexpected(invalid("复制位置超出数组", document.key));
        // Take a value copy before insertion; the array may reallocate.
        const auto value = values[position];
        values.insert(values.begin() +
                          static_cast<Json::difference_type>(*edit.destination),
                      value);
        break;
      }
      case JsonEditAction::array_move: {
        if (position >= size || !edit.destination || *edit.destination >= size)
          return std::unexpected(invalid("移动位置超出数组", document.key));
        auto value = values[position];
        values.erase(values.begin() +
                     static_cast<Json::difference_type>(position));
        values.insert(values.begin() +
                          static_cast<Json::difference_type>(*edit.destination),
                      std::move(value));
        break;
      }
      default:
        break;
      }
    }
  } catch (const std::exception &exception) {
    return std::unexpected(invalid(
        "结构编辑路径无效：" + std::string(exception.what()), document.key));
  }
  return ModuleEdit{
      document.key, {document.source_text}, parsed->dump(2) + "\n"};
}

Result<ModuleEdit> set_log_level(const ModuleDocument &document,
                                 std::optional<std::string> level) {
  return update_object(document, "log", [&](Json &native) {
    if (level)
      native["loglevel"] = *level;
    else
      native.erase("loglevel");
  });
}

Result<ModuleEdit> replace_dns_servers(const ModuleDocument &document,
                                       std::vector<Json> servers) {
  for (const auto &server : servers)
    if (!server.is_string() && !server.is_object())
      return std::unexpected(
          invalid("DNS server 必须是原生字符串或对象", document.key));
  return update_object(document, "dns", [&](Json &native) {
    native["servers"] = std::move(servers);
  });
}

Result<ModuleEdit> replace_dns_hosts(const ModuleDocument &document,
                                     Json hosts) {
  if (!hosts.is_object())
    return std::unexpected(invalid("DNS hosts 必须是原生对象", document.key));
  return update_object(document, "dns", [&](Json &native) {
    native["hosts"] = std::move(hosts);
  });
}

Result<ModuleEdit> set_api_services(const ModuleDocument &document,
                                    std::vector<std::string> services) {
  return update_object(document, "api", [&](Json &native) {
    native["services"] = std::move(services);
  });
}

Result<ModuleEdit> set_metrics_listener(const ModuleDocument &document,
                                        std::string listen) {
  if (listen.empty())
    return std::unexpected(invalid("metrics.listen 不能为空", document.key));
  return update_object(document, "metrics", [&](Json &native) {
    native["listen"] = std::move(listen);
  });
}

Result<ModuleEdit> put_policy_level(const ModuleDocument &document,
                                    std::string level, Json complete_level) {
  if (level.empty() || !complete_level.is_object())
    return std::unexpected(
        invalid("policy level 名称不能为空且内容必须是完整对象", document.key));
  return update_object(document, "policy", [&](Json &native) {
    if (!native["levels"].is_object() && !native["levels"].is_null())
      throw std::invalid_argument("levels");
    native["levels"][std::move(level)] = std::move(complete_level);
  });
}

Result<ModuleEdit> set_policy_counters(const ModuleDocument &document,
                                       bool in_up, bool in_down, bool out_up,
                                       bool out_down) {
  return update_object(document, "policy", [&](Json &native) {
    if (!native["system"].is_object() && !native["system"].is_null())
      throw std::invalid_argument("system");
    auto &system = native["system"];
    system["statsInboundUplink"] = in_up;
    system["statsInboundDownlink"] = in_down;
    system["statsOutboundUplink"] = out_up;
    system["statsOutboundDownlink"] = out_down;
  });
}

Result<ModuleEdit> replace_fakedns_pools(const ModuleDocument &document,
                                         Json object_or_array) {
  return update_fakedns(document, std::move(object_or_array));
}

Result<ModuleEdit> put_env(const ModuleDocument &document, std::string key,
                           std::string value) {
  if (key.empty())
    return std::unexpected(invalid("env 键不能为空", document.key));
  return update_object(document, "env", [&](Json &native) {
    native[std::move(key)] = std::move(value);
  });
}

Result<ModuleEdit> erase_env(const ModuleDocument &document, std::string key) {
  if (key.empty())
    return std::unexpected(invalid("env 键不能为空", document.key));
  return update_object(document, "env",
                       [&](Json &native) { native.erase(key); });
}

Result<ModuleEdit> set_version_range(const ModuleDocument &document,
                                     std::optional<std::string> min,
                                     std::optional<std::string> max) {
  return update_object(document, "version", [&](Json &native) {
    if (min)
      native["min"] = *min;
    else
      native.erase("min");
    if (max)
      native["max"] = *max;
    else
      native.erase("max");
  });
}

Result<ModuleEdit> replace_geodata_assets(const ModuleDocument &document,
                                          std::vector<Json> assets) {
  return update_object(document, "geodata", [&](Json &native) {
    native["assets"] = std::move(assets);
  });
}

Result<ModuleEdit> set_geodata_schedule(const ModuleDocument &document,
                                        std::string cron) {
  return update_object(document, "geodata",
                       [&](Json &native) { native["cron"] = std::move(cron); });
}

Result<ModuleEdit> set_observatory_probe(const ModuleDocument &document,
                                         std::string url, std::string interval,
                                         bool concurrency) {
  if (document.key != "observatory")
    return std::unexpected(
        invalid("probeUrl/probeInterval 属于原生 observatory；burstObservatory "
                "请编辑 pingConfig",
                document.key));
  return update_object(document, "observatory", [&](Json &native) {
    native["probeUrl"] = std::move(url);
    native["probeInterval"] = std::move(interval);
    native["enableConcurrency"] = concurrency;
  });
}

Result<ModuleEdit> replace_burst_ping_config(const ModuleDocument &document,
                                             Json complete_ping_config) {
  if (!complete_ping_config.is_object())
    return std::unexpected(
        invalid("pingConfig 必须是完整原生对象", document.key));
  return update_object(document, "burstObservatory", [&](Json &native) {
    native["pingConfig"] = std::move(complete_ping_config);
  });
}

Result<ModuleEdit> edit_native_list(const ModuleDocument &document,
                                    const ListEdit &edit) {
  // Collection operations preserve list order and all untouched object keys;
  // only the collection owned by this module is rewritten.
  auto parsed = module_array(document, edit.collection);
  if (!parsed)
    return std::unexpected(parsed.error());
  std::string wrapper;
  std::string array_key;
  switch (edit.collection) {
  case CollectionKind::inbounds:
    wrapper = "inbounds";
    array_key = "inbounds";
    break;
  case CollectionKind::outbounds:
    wrapper = "outbounds";
    array_key = "outbounds";
    break;
  case CollectionKind::route_rules:
    wrapper = "routing";
    array_key = "rules";
    break;
  case CollectionKind::native_balancers:
    wrapper = "routing";
    array_key = "balancers";
    break;
  }
  auto &values = (edit.collection == CollectionKind::inbounds ||
                  edit.collection == CollectionKind::outbounds)
                     ? (*parsed)[wrapper]
                     : (*parsed)[wrapper][array_key];
  const auto size = values.size();
  switch (edit.action) {
  case ListAction::insert:
    if (edit.index > size || !edit.object || !edit.object->is_object())
      return std::unexpected(invalid(
          "insert 索引必须在 0..size，且对象必须是完整原生对象", document.key));
    if (edit.collection != CollectionKind::route_rules && !tag_of(*edit.object))
      return std::unexpected(
          invalid("入站、出站和原生 balancer 新项必须有 tag", document.key));
    if (edit.collection != CollectionKind::route_rules &&
        duplicate_tag(values, *edit.object))
      return std::unexpected(
          invalid("同一原生列表中 tag 已存在", document.key));
    values.insert(values.begin() +
                      static_cast<Json::difference_type>(edit.index),
                  *edit.object);
    break;
  case ListAction::replace:
    if (edit.index >= size || !edit.object || !edit.object->is_object())
      return std::unexpected(
          invalid("replace 索引必须在 0..size-1，且对象必须是完整原生对象",
                  document.key));
    if (edit.collection != CollectionKind::route_rules && !tag_of(*edit.object))
      return std::unexpected(
          invalid("入站、出站和原生 balancer 替换项必须有 tag", document.key));
    if (edit.collection != CollectionKind::route_rules &&
        duplicate_tag(values, *edit.object, edit.index))
      return std::unexpected(
          invalid("同一原生列表中 tag 已存在", document.key));
    values[edit.index] = *edit.object;
    break;
  case ListAction::erase:
    if (edit.index >= size)
      return std::unexpected(
          invalid("erase 索引必须在 0..size-1", document.key));
    values.erase(values.begin() +
                 static_cast<Json::difference_type>(edit.index));
    break;
  case ListAction::move: {
    if (edit.index >= size || !edit.destination || *edit.destination >= size)
      return std::unexpected(
          invalid("move 的源和最终目标索引必须在 0..size-1", document.key));
    const auto value = values[edit.index];
    values.erase(values.begin() +
                 static_cast<Json::difference_type>(edit.index));
    values.insert(values.begin() +
                      static_cast<Json::difference_type>(*edit.destination),
                  value);
    break;
  }
  }
  return ModuleEdit{
      document.key, {document.source_text}, parsed->dump(2) + "\n"};
}

Result<Json> set_rule_target(Json original_rule, const Target &target,
                             std::string_view resolved_native_tag) {
  if (!original_rule.is_object())
    return std::unexpected(invalid("原生 routing rule 必须是对象", "routing"));
  if (resolved_native_tag.empty())
    return std::unexpected(invalid("路由目标尚未解析为原生 tag", "routing"));
  if (target.kind == TargetKind::node ||
      target.kind == TargetKind::native_outbound) {
    original_rule.erase("balancerTag");
    original_rule["outboundTag"] = resolved_native_tag;
  } else {
    original_rule.erase("outboundTag");
    original_rule["balancerTag"] = resolved_native_tag;
  }
  return original_rule;
}

} // namespace xrm
