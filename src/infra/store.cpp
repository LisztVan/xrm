// XRM: Persists Profiles, modules, nodes, sources, and Manager settings.

#include "infra/store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace xrm {
namespace {

AppError db_error(sqlite3 *db, std::string_view operation) {
  std::string message(operation);
  message += ": ";
  message += db ? sqlite3_errmsg(db) : "SQLite database unavailable";
  return {ErrorCode::storage_error, std::move(message), std::nullopt,
          std::nullopt};
}

AppError invalid(std::string message, std::string subject = {}) {
  return {ErrorCode::invalid_argument, std::move(message),
          subject.empty() ? std::nullopt
                          : std::optional<std::string>(std::move(subject)),
          std::nullopt};
}

AppError missing(std::string kind, const Id &id) {
  return {ErrorCode::not_found, kind + "不存在", id, std::nullopt};
}

class Statement {
public:
  Statement(sqlite3 *db, std::string_view sql) : db_(db) {
    result_ = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
                                 &statement_, nullptr);
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  bool ready() const { return result_ == SQLITE_OK; }
  sqlite3_stmt *get() const { return statement_; }
  AppError error(std::string_view operation) const {
    return db_error(db_, operation);
  }

private:
  sqlite3 *db_;
  sqlite3_stmt *statement_ = nullptr;
  int result_ = SQLITE_ERROR;
};

void bind_text(sqlite3_stmt *statement, int index, std::string_view value) {
  sqlite3_bind_text(statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt *statement, int column) {
  const auto *value = sqlite3_column_text(statement, column);
  const auto bytes = sqlite3_column_bytes(statement, column);
  return value ? std::string(reinterpret_cast<const char *>(value),
                             static_cast<std::size_t>(bytes))
               : std::string{};
}

Json geodata_record_json(const GeodataResourceRecord &record) {
  Json current = nullptr;
  if (record.current) {
    const auto &version = *record.current;
    current = Json{
        {"resource_id", version.resource_id},
        {"kind", version.kind == GeoAssetKind::geoip ? "geoip" : "geosite"},
        {"file_name", version.file_name},
        {"version_id", version.version_id},
        {"asset_dir", version.asset_dir.string()},
        {"path", version.path.string()},
        {"labels", version.labels},
        {"bytes", version.bytes}};
  }
  return Json{
      {"id", record.id},
      {"name", record.name},
      {"kind", record.kind == GeoAssetKind::geoip ? "geoip" : "geosite"},
      {"file_name", record.file_name},
      {"url", record.url},
      {"local_path", record.local_path.string()},
      {"auto_update", record.auto_update},
      {"update_interval_seconds", record.update_interval.count()},
      {"validator_core_id", record.validator_core_id},
      {"settings_revision", record.settings_revision},
      {"current", std::move(current)},
      {"last_attempt", record.last_attempt},
      {"last_success", record.last_success},
      {"last_result", record.last_result}};
}

GeodataResourceRecord geodata_record_from_json(const Json &json) {
  GeodataResourceRecord record;
  record.id = json.at("id").get<std::string>();
  record.name = json.at("name").get<std::string>();
  record.kind = json.at("kind").get<std::string>() == "geoip"
                    ? GeoAssetKind::geoip
                    : GeoAssetKind::geosite;
  record.file_name = json.at("file_name").get<std::string>();
  record.url = json.value("url", std::string{});
  record.local_path = json.value("local_path", std::string{});
  record.auto_update = json.value("auto_update", false);
  record.update_interval = std::chrono::seconds{
      json.value("update_interval_seconds", std::int64_t{0})};
  record.validator_core_id = json.value("validator_core_id", std::string{});
  record.settings_revision = json.at("settings_revision").get<std::uint64_t>();
  if (json.contains("current") && !json.at("current").is_null()) {
    const auto &value = json.at("current");
    GeodataAssetVersion version;
    version.resource_id = value.at("resource_id").get<std::string>();
    version.kind = value.at("kind").get<std::string>() == "geoip"
                       ? GeoAssetKind::geoip
                       : GeoAssetKind::geosite;
    version.file_name = value.at("file_name").get<std::string>();
    version.version_id = value.at("version_id").get<std::string>();
    version.asset_dir = value.at("asset_dir").get<std::string>();
    version.path = value.at("path").get<std::string>();
    version.labels = value.at("labels").get<std::set<std::string>>();
    version.bytes = value.at("bytes").get<std::size_t>();
    record.current = std::move(version);
  }
  record.last_attempt = json.value("last_attempt", UnixMillis{0});
  record.last_success = json.value("last_success", UnixMillis{0});
  record.last_result = json.value("last_result", std::string{});
  return record;
}

template <class F>
Result<void> exec(sqlite3 *db, std::string_view sql, F &&operation) {
  char *message = nullptr;
  const int status =
      sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &message);
  sqlite3_free(message);
  if (status != SQLITE_OK)
    return std::unexpected(db_error(db, operation()));
  return {};
}

std::string folded(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool contains_all_labels(const std::vector<std::string> &node_labels,
                         const std::vector<std::string> &required) {
  return std::all_of(required.begin(), required.end(), [&](const auto &label) {
    return std::find(node_labels.begin(), node_labels.end(), label) !=
           node_labels.end();
  });
}

Node node_from_columns(sqlite3_stmt *statement) {
  Json labels = Json::parse(column_text(statement, 4));
  Json stored = Json::parse(column_text(statement, 7));
  Node node{column_text(statement, 0),
            std::nullopt,
            column_text(statement, 2),
            std::nullopt,
            labels.get<std::vector<std::string>>(),
            sqlite3_column_int(statement, 5) != 0,
            sqlite3_column_int(statement, 6) != 0,
            RawOutbound{stored}};
  if (sqlite3_column_type(statement, 1) != SQLITE_NULL) {
    node.source_id = column_text(statement, 1);
  }
  if (sqlite3_column_type(statement, 3) != SQLITE_NULL) {
    node.alias = column_text(statement, 3);
  }
  if (stored.is_object() && stored.contains("$xrm_node_settings") &&
      !stored.contains("protocol")) {
    Json document{
        {"id", node.id},
        {"source_id", node.source_id ? Json(*node.source_id) : Json(nullptr)},
        {"upstream_id", stored.value("$xrm_upstream_id", Json(nullptr))},
        {"original_name", node.original_name},
        {"alias", node.alias ? Json(*node.alias) : Json(nullptr)},
        {"labels", node.labels},
        {"enabled", node.enabled},
        {"source_missing", node.source_missing},
        {"settings", stored.at("$xrm_node_settings")}};
    document.get_to(node);
  }
  return node;
}

Json stored_node_settings(const Node &node) {
  if (const auto *raw = std::get_if<RawOutbound>(&node.settings);
      raw && !node.source_id && !node.upstream_id)
    return raw->value;
  const Json encoded(node);
  return Json{{"$xrm_node_settings", encoded.at("settings")},
              {"$xrm_upstream_id",
               node.upstream_id ? Json(*node.upstream_id) : Json(nullptr)}};
}

AppError source_missing_error(const Id &id) {
  return AppError{ErrorCode::not_found, "订阅来源不存在", id, std::nullopt};
}

Result<Subscription> source_from_json(std::string_view text, const Id &id) {
  try {
    return Json::parse(text).get<Subscription>();
  } catch (const std::exception &error) {
    return std::unexpected(AppError{
        ErrorCode::storage_error,
        std::string("订阅来源数据无效: ") + error.what(), id, std::nullopt});
  }
}

Result<SubscriptionState> state_from_json(std::string_view text, const Id &id) {
  try {
    return Json::parse(text).get<SubscriptionState>();
  } catch (const std::exception &error) {
    return std::unexpected(AppError{
        ErrorCode::storage_error,
        std::string("订阅状态数据无效: ") + error.what(), id, std::nullopt});
  }
}

bool request_settings_equal(const Subscription &left,
                            const Subscription &right) {
  Json a(left), b(right);
  a.erase("settings_revision");
  b.erase("settings_revision");
  return a == b;
}

Json default_program_settings() {
  return Json{{"appearance",
               {{"theme", "dark"},
                {"color_mode", "auto"},
                {"density", "comfortable"},
                {"icon_mode", "unicode"},
                {"reduce_motion", false},
                {"cli_color", "auto"}}},
              {"editor", {{"argv", Json::array()}}},
              {"tests",
               {{"default_url", ""},
                {"timeout_ms", 5000},
                {"concurrency", 3},
                {"max_body_bytes", 8 * 1024 * 1024}}},
              {"general", Json::object()}};
}

bool one_of(const Json &value,
            std::initializer_list<std::string_view> choices) {
  if (!value.is_string())
    return false;
  const auto text = value.get<std::string>();
  return std::find(choices.begin(), choices.end(), text) != choices.end();
}

Result<Json> apply_program_settings_patch(Json current, const Json &patch) {
  if (!patch.is_object())
    return std::unexpected(invalid("程序设置补丁必须是对象", "patch"));
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    if (!it.value().is_object())
      return std::unexpected(invalid("设置分组必须是对象", it.key()));
    if (it.key() == "appearance") {
      for (auto field = it.value().begin(); field != it.value().end();
           ++field) {
        const auto &key = field.key();
        const auto &value = field.value();
        bool valid = false;
        if (key == "theme")
          valid = one_of(value, {"default", "dark", "light"});
        else if (key == "color_mode")
          valid = one_of(value, {"auto", "truecolor", "256", "none"});
        else if (key == "density")
          valid = one_of(value, {"comfortable", "compact"});
        else if (key == "icon_mode")
          valid = one_of(value, {"unicode", "ascii"});
        else if (key == "cli_color")
          valid = one_of(value, {"auto", "always", "never"});
        else if (key == "reduce_motion")
          valid = value.is_boolean();
        if (!valid)
          return std::unexpected(
              invalid("不支持的界面设置或值", "appearance." + key));
        current["appearance"][key] = value;
      }
    } else if (it.key() == "editor") {
      for (auto field = it.value().begin(); field != it.value().end();
           ++field) {
        if (field.key() != "argv" || !field.value().is_array())
          return std::unexpected(
              invalid("Editor只支持argv数组", "editor." + field.key()));
        for (const auto &arg : field.value()) {
          if (!arg.is_string() ||
              arg.get<std::string>().find('\0') != std::string::npos)
            return std::unexpected(
                invalid("Editor argv必须是无NUL字符串", "editor.argv"));
        }
        if (!field.value().empty() &&
            field.value().front().get<std::string>().empty())
          return std::unexpected(
              invalid("Editor可执行文件不能为空", "editor.argv"));
        current["editor"]["argv"] = field.value();
      }
    } else if (it.key() == "tests") {
      if (!current.contains("tests"))
        current["tests"] = default_program_settings()["tests"];
      for (auto field = it.value().begin(); field != it.value().end();
           ++field) {
        const auto &key = field.key();
        const auto &value = field.value();
        bool valid = false;
        if (key == "default_url")
          valid = value.is_string() &&
                  (value.get<std::string>().empty() ||
                   value.get<std::string>().starts_with("http://") ||
                   value.get<std::string>().starts_with("https://"));
        else if (key == "timeout_ms" || key == "max_body_bytes" ||
                 key == "concurrency") {
          if (value.is_number_integer()) {
            const auto number = value.get<std::int64_t>();
            if (key == "timeout_ms")
              valid = number >= 100 && number <= 60000;
            else if (key == "max_body_bytes")
              valid = number >= 1 && number <= 32 * 1024 * 1024;
            else
              valid = number >= 1 && number <= 3;
          }
        }
        if (!valid)
          return std::unexpected(
              invalid("不支持的节点测试设置或值", "tests." + key));
        current["tests"][key] = value;
      }
    } else if (it.key() == "general") {
      if (!it.value().empty())
        return std::unexpected(
            invalid("当前没有可调整的General设置", "general"));
    } else {
      return std::unexpected(invalid("不支持的程序设置分组", it.key()));
    }
  }
  return current;
}

Result<void> upsert_node_row(sqlite3 *db, const Node &node) {
  if (node.id.empty())
    return std::unexpected(invalid("节点ID不能为空", "id"));
  if (node.original_name.empty())
    return std::unexpected(invalid("节点名称不能为空", "name"));
  const auto *raw_outbound = std::get_if<RawOutbound>(&node.settings);
  if (raw_outbound && !raw_outbound->value.is_object())
    return std::unexpected(invalid("原生outbound必须是对象", "outbound"));
  const Json json(node);
  Statement statement(
      db,
      "INSERT INTO nodes(id,source_id,original_name,alias,labels_json,"
      "enabled,source_missing,outbound_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
      "ON CONFLICT(id) DO UPDATE SET source_id=excluded.source_id,"
      "original_name=excluded.original_name,alias=excluded.alias,"
      "labels_json=excluded.labels_json,enabled=excluded.enabled,"
      "source_missing=excluded.source_missing,outbound_json=excluded.outbound_"
      "json");
  if (!statement.ready())
    return std::unexpected(statement.error("准备保存节点失败"));
  bind_text(statement.get(), 1, node.id);
  if (node.source_id)
    bind_text(statement.get(), 2, *node.source_id);
  else
    sqlite3_bind_null(statement.get(), 2);
  bind_text(statement.get(), 3, node.original_name);
  if (node.alias)
    bind_text(statement.get(), 4, *node.alias);
  else
    sqlite3_bind_null(statement.get(), 4);
  bind_text(statement.get(), 5, json.at("labels").dump());
  sqlite3_bind_int(statement.get(), 6, node.enabled ? 1 : 0);
  sqlite3_bind_int(statement.get(), 7, node.source_missing ? 1 : 0);
  bind_text(statement.get(), 8, stored_node_settings(node).dump());
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("保存节点失败"));
  return {};
}

} // namespace

struct Store::Impl {
  explicit Impl(sqlite3 *handle) : db(handle) {}
  ~Impl() {
    if (db)
      sqlite3_close_v2(db);
  }
  sqlite3 *db = nullptr;
};

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open(std::filesystem::path database) {
  if (database.empty())
    return std::unexpected(invalid("数据库路径不能为空"));
  sqlite3 *handle = nullptr;
  const auto native_path = database.string();
  const int status = sqlite3_open_v2(
      native_path.c_str(), &handle,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (status != SQLITE_OK) {
    auto error = db_error(handle, "打开数据库失败");
    if (handle)
      sqlite3_close_v2(handle);
    return std::unexpected(std::move(error));
  }
  sqlite3_busy_timeout(handle, 5000);
  auto impl = std::make_unique<Impl>(handle);
  const char *schema =
      "PRAGMA journal_mode=WAL;"
      "CREATE TABLE IF NOT EXISTS nodes ("
      "id TEXT PRIMARY KEY NOT NULL, source_id TEXT, original_name TEXT NOT "
      "NULL,"
      "alias TEXT, labels_json TEXT NOT NULL, enabled INTEGER NOT NULL,"
      "source_missing INTEGER NOT NULL, outbound_json TEXT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS nodes_source_idx ON nodes(source_id);"
      "CREATE TABLE IF NOT EXISTS subscriptions ("
      "id TEXT PRIMARY KEY NOT NULL, settings_json TEXT NOT NULL, state_json "
      "TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS profiles (id TEXT PRIMARY KEY NOT NULL, path "
      "TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS cores ("
      "id TEXT PRIMARY KEY NOT NULL, name TEXT NOT NULL, path TEXT NOT NULL,"
      "reported_version TEXT NOT NULL, managed INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS geodata_resources ("
      "id TEXT PRIMARY KEY NOT NULL, settings_revision INTEGER NOT NULL,"
      "record_json TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS runtime_state ("
      "id INTEGER PRIMARY KEY CHECK(id=1), document_json TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS program_settings ("
      "id INTEGER PRIMARY KEY CHECK(id=1), document_json TEXT NOT NULL);";
  char *message = nullptr;
  const int schema_status =
      sqlite3_exec(handle, schema, nullptr, nullptr, &message);
  sqlite3_free(message);
  if (schema_status != SQLITE_OK)
    return std::unexpected(db_error(handle, "初始化数据库失败"));
  return std::unique_ptr<Store>(new Store(std::move(impl)));
}

Result<Page<Node>> Store::list_nodes(const NodeQuery &query,
                                     PageRequest request) const {
  if (request.limit == 0 || request.limit > 1000) {
    return std::unexpected(invalid("节点列表limit必须在1到1000之间", "limit"));
  }
  Statement statement(
      impl_->db,
      "SELECT id, source_id, original_name, alias, labels_json, "
      "enabled, source_missing, outbound_json FROM nodes ORDER BY id");
  if (!statement.ready())
    return std::unexpected(statement.error("准备节点列表失败"));
  std::vector<Node> matches;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE)
      break;
    if (status != SQLITE_ROW)
      return std::unexpected(statement.error("读取节点列表失败"));
    try {
      Node node = node_from_columns(statement.get());
      if (query.source_id && node.source_id != query.source_id)
        continue;
      if (!query.include_missing && node.source_missing)
        continue;
      if (query.text) {
        const auto text = folded(*query.text);
        if (folded(node.original_name).find(text) == std::string::npos &&
            (!node.alias ||
             folded(*node.alias).find(text) == std::string::npos))
          continue;
      }
      if (!contains_all_labels(node.labels, query.labels))
        continue;
      matches.push_back(std::move(node));
    } catch (const std::exception &error) {
      return std::unexpected(
          AppError{ErrorCode::storage_error,
                   std::string("节点库存包含无效数据: ") + error.what(),
                   std::nullopt, std::nullopt});
    }
  }

  Page<Node> page;
  page.total = matches.size();
  if (request.offset < matches.size()) {
    const auto end = std::min(matches.size(), request.offset + request.limit);
    page.items.reserve(end - request.offset);
    for (auto index = request.offset; index < end; ++index)
      page.items.push_back(std::move(matches[index]));
    if (end < matches.size())
      page.next_offset = end;
  }
  return page;
}

Result<Node> Store::get_node(const Id &id) const {
  Statement statement(
      impl_->db, "SELECT id,source_id,original_name,alias,labels_json,enabled,"
                 "source_missing,outbound_json FROM nodes WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取节点失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return std::unexpected(missing("节点", id));
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取节点失败"));
  try {
    return node_from_columns(statement.get());
  } catch (const std::exception &error) {
    return std::unexpected(AppError{
        ErrorCode::storage_error, std::string("节点数据无效: ") + error.what(),
        id, std::nullopt});
  }
}

Result<std::vector<Node>> Store::all_nodes() const {
  const auto page = list_nodes(NodeQuery{.include_missing = true}, {0, 1000});
  if (!page)
    return std::unexpected(page.error());
  if (page->total > page->items.size()) {
    return std::unexpected(AppError{ErrorCode::too_large,
                                    "节点数量超过单次读取上限", std::nullopt,
                                    "使用分页列表"});
  }
  return page->items;
}

Result<void> Store::put_node(const Node &node) {
  if (auto started = exec(impl_->db, "BEGIN IMMEDIATE",
                          [] { return "开始保存节点事务失败"; });
      !started) {
    return started;
  }
  if (auto saved = upsert_node_row(impl_->db, node); !saved) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return saved;
  }
  if (auto committed =
          exec(impl_->db, "COMMIT", [] { return "提交节点保存失败"; });
      !committed) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return committed;
  }
  return {};
}

Result<void> Store::erase_node(const Id &id) {
  Statement statement(impl_->db, "DELETE FROM nodes WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备删除节点失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("删除节点失败"));
  if (sqlite3_changes(impl_->db) == 0)
    return std::unexpected(missing("节点", id));
  return {};
}

Result<void> Store::prune_history(const Id &source_id,
                                  const SubscriptionState &expected_state,
                                  const std::vector<Node> &expected_nodes,
                                  const std::vector<Id> &selected_ids) {
  if (selected_ids.empty())
    return std::unexpected(invalid("至少选择一条历史记录", "ids"));
  if (auto started = exec(impl_->db, "BEGIN IMMEDIATE",
                          [] { return "开始历史节点整理事务失败"; });
      !started)
    return started;
  auto rollback = [&] {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
  };
  auto stale = [&]() -> Result<void> {
    rollback();
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "来源或节点已变化，请重新预览", source_id,
                                    std::nullopt});
  };
  auto state = get_source_state(source_id);
  if (!state || Json(*state) != Json(expected_state))
    return stale();
  std::vector<Node> actual;
  std::size_t offset = 0;
  for (;;) {
    auto page =
        list_nodes(NodeQuery{.source_id = source_id, .include_missing = true},
                   {offset, 1000});
    if (!page) {
      rollback();
      return std::unexpected(page.error());
    }
    actual.insert(actual.end(), page->items.begin(), page->items.end());
    if (!page->next_offset)
      break;
    offset = *page->next_offset;
  }
  if (actual.size() != expected_nodes.size())
    return stale();
  for (std::size_t index = 0; index < actual.size(); ++index)
    if (Json(actual[index]) != Json(expected_nodes[index]))
      return stale();
  std::set<Id> unique_ids(selected_ids.begin(), selected_ids.end());
  if (unique_ids.size() != selected_ids.size())
    return stale();
  for (const auto &id : selected_ids) {
    const auto found =
        std::find_if(actual.begin(), actual.end(), [&](const Node &node) {
          return node.id == id && node.source_missing;
        });
    if (found == actual.end())
      return stale();
    Statement statement(
        impl_->db,
        "DELETE FROM nodes WHERE id=?1 AND source_id=?2 AND source_missing=1");
    if (!statement.ready()) {
      rollback();
      return std::unexpected(statement.error("准备整理历史节点失败"));
    }
    bind_text(statement.get(), 1, id);
    bind_text(statement.get(), 2, source_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->db) != 1) {
      rollback();
      return std::unexpected(db_error(impl_->db, "整理历史节点失败"));
    }
  }
  if (auto committed =
          exec(impl_->db, "COMMIT", [] { return "提交历史节点整理失败"; });
      !committed) {
    rollback();
    return committed;
  }
  return {};
}

Result<std::vector<Subscription>> Store::list_sources() const {
  Statement statement(impl_->db,
                      "SELECT id,settings_json FROM subscriptions ORDER BY id");
  if (!statement.ready())
    return std::unexpected(statement.error("准备订阅列表失败"));
  std::vector<Subscription> sources;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE)
      break;
    if (status != SQLITE_ROW)
      return std::unexpected(statement.error("读取订阅列表失败"));
    auto source = source_from_json(column_text(statement.get(), 1),
                                   column_text(statement.get(), 0));
    if (!source)
      return std::unexpected(source.error());
    sources.push_back(std::move(*source));
  }
  return sources;
}

Result<Subscription> Store::get_source(const Id &id) const {
  Statement statement(impl_->db,
                      "SELECT settings_json FROM subscriptions WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取订阅失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return std::unexpected(source_missing_error(id));
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取订阅失败"));
  return source_from_json(column_text(statement.get(), 0), id);
}

Result<SubscriptionState> Store::get_source_state(const Id &id) const {
  Statement statement(impl_->db,
                      "SELECT state_json FROM subscriptions WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取订阅状态失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return std::unexpected(source_missing_error(id));
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取订阅状态失败"));
  return state_from_json(column_text(statement.get(), 0), id);
}

Result<void> Store::put_source(const Subscription &requested) {
  if (requested.id.empty())
    return std::unexpected(invalid("订阅ID不能为空", "id"));
  if (requested.name.empty())
    return std::unexpected(invalid("订阅名称不能为空", "name"));
  if (requested.url.empty())
    return std::unexpected(invalid("订阅URL不能为空", "url"));
  if (requested.interval.count() <= 0)
    return std::unexpected(invalid("订阅刷新间隔必须大于零", "interval"));

  Subscription stored = requested;
  SubscriptionState state;
  Statement existing(
      impl_->db,
      "SELECT settings_json,state_json FROM subscriptions WHERE id=?1");
  if (!existing.ready())
    return std::unexpected(existing.error("准备读取订阅设置失败"));
  bind_text(existing.get(), 1, requested.id);
  const int status = sqlite3_step(existing.get());
  if (status == SQLITE_ROW) {
    auto current =
        source_from_json(column_text(existing.get(), 0), requested.id);
    if (!current)
      return std::unexpected(current.error());
    auto current_state =
        state_from_json(column_text(existing.get(), 1), requested.id);
    if (!current_state)
      return std::unexpected(current_state.error());
    state = std::move(*current_state);
    if (request_settings_equal(*current, requested)) {
      stored.settings_revision = current->settings_revision;
    } else {
      stored.settings_revision = current->settings_revision + 1;
      state.etag.reset();
      state.last_modified.reset();
      state.last_success.reset();
      state.last_result = "settings_changed";
      state.last_error.reset();
      state.last_error_stage.reset();
      state.last_unsupported_count = 0;
      state.last_malformed_count = 0;
    }
  } else if (status == SQLITE_DONE) {
    stored.settings_revision = 0;
  } else {
    return std::unexpected(existing.error("读取订阅设置失败"));
  }

  Statement statement(
      impl_->db,
      "INSERT INTO subscriptions(id,settings_json,state_json) VALUES(?1,?2,?3) "
      "ON CONFLICT(id) DO UPDATE SET settings_json=excluded.settings_json,"
      "state_json=excluded.state_json");
  if (!statement.ready())
    return std::unexpected(statement.error("准备保存订阅失败"));
  bind_text(statement.get(), 1, stored.id);
  bind_text(statement.get(), 2, Json(stored).dump());
  bind_text(statement.get(), 3, Json(state).dump());
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("保存订阅失败"));
  return {};
}

Result<void> Store::remove_source(const Id &id, bool keep_as_manual) {
  if (auto started = exec(impl_->db, "BEGIN IMMEDIATE",
                          [] { return "开始删除订阅事务失败"; });
      !started)
    return started;
  Statement source(impl_->db, "SELECT 1 FROM subscriptions WHERE id=?1");
  if (!source.ready()) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(source.error("准备检查订阅失败"));
  }
  bind_text(source.get(), 1, id);
  const int found = sqlite3_step(source.get());
  if (found == SQLITE_DONE) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(source_missing_error(id));
  }
  if (found != SQLITE_ROW) {
    auto error = source.error("检查订阅失败");
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(std::move(error));
  }

  std::vector<Node> nodes;
  {
    Statement select(
        impl_->db,
        "SELECT id,source_id,original_name,alias,labels_json,enabled,"
        "source_missing,outbound_json FROM nodes WHERE source_id=?1");
    if (!select.ready()) {
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(select.error("准备读取来源节点失败"));
    }
    bind_text(select.get(), 1, id);
    for (;;) {
      const int status = sqlite3_step(select.get());
      if (status == SQLITE_DONE)
        break;
      if (status != SQLITE_ROW) {
        auto error = select.error("读取来源节点失败");
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(std::move(error));
      }
      try {
        nodes.push_back(node_from_columns(select.get()));
      } catch (const std::exception &exception) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(
            AppError{ErrorCode::storage_error,
                     std::string("来源节点数据无效: ") + exception.what(), id,
                     std::nullopt});
      }
    }
  }
  for (auto &node : nodes) {
    if (keep_as_manual) {
      node.source_id.reset();
      node.upstream_id.reset();
      node.source_missing = false;
      if (auto saved = upsert_node_row(impl_->db, node); !saved) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return saved;
      }
    }
  }
  if (!keep_as_manual) {
    Statement erase_nodes(impl_->db, "DELETE FROM nodes WHERE source_id=?1");
    if (!erase_nodes.ready()) {
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(erase_nodes.error("准备删除来源库存失败"));
    }
    bind_text(erase_nodes.get(), 1, id);
    if (sqlite3_step(erase_nodes.get()) != SQLITE_DONE) {
      auto error = erase_nodes.error("删除来源库存失败");
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(std::move(error));
    }
  }
  Statement remove(impl_->db, "DELETE FROM subscriptions WHERE id=?1");
  if (!remove.ready()) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(remove.error("准备删除订阅失败"));
  }
  bind_text(remove.get(), 1, id);
  if (sqlite3_step(remove.get()) != SQLITE_DONE) {
    auto error = remove.error("删除订阅失败");
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(std::move(error));
  }
  if (auto committed =
          exec(impl_->db, "COMMIT", [] { return "提交删除订阅失败"; });
      !committed) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return committed;
  }
  return {};
}

Result<void> Store::commit_source(const Id &id, const SourceCommit &commit) {
  if (auto started = exec(impl_->db, "BEGIN IMMEDIATE",
                          [] { return "开始订阅库存事务失败"; });
      !started)
    return started;
  Statement source(impl_->db,
                   "SELECT settings_json FROM subscriptions WHERE id=?1");
  if (!source.ready()) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(source.error("准备检查订阅版本失败"));
  }
  bind_text(source.get(), 1, id);
  const int status = sqlite3_step(source.get());
  if (status == SQLITE_DONE) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(source_missing_error(id));
  }
  if (status != SQLITE_ROW) {
    auto error = source.error("读取订阅版本失败");
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(std::move(error));
  }
  auto current = source_from_json(column_text(source.get(), 0), id);
  if (!current) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(current.error());
  }
  if (current->settings_revision != commit.expected_settings_revision) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "订阅设置在刷新期间发生变化，旧结果未提交",
                                    id, std::nullopt});
  }
  for (const auto &node : commit.changes.upserts) {
    if (node.source_id != std::optional<Id>(id)) {
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(
          invalid("来源事务只能写入同一来源的节点", "source_id"));
    }
    bool exists = false;
    std::optional<Id> existing_source;
    {
      Statement existing_node(impl_->db,
                              "SELECT source_id FROM nodes WHERE id=?1");
      if (!existing_node.ready()) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(existing_node.error("准备检查来源节点身份失败"));
      }
      bind_text(existing_node.get(), 1, node.id);
      const int existing_status = sqlite3_step(existing_node.get());
      if (existing_status == SQLITE_ROW) {
        exists = true;
        if (sqlite3_column_type(existing_node.get(), 0) != SQLITE_NULL)
          existing_source = column_text(existing_node.get(), 0);
      } else if (existing_status != SQLITE_DONE) {
        auto error = existing_node.error("检查来源节点身份失败");
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(std::move(error));
      }
    }
    if (exists && existing_source != std::optional<Id>(id)) {
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "来源刷新不能覆盖另一节点或手工节点的ID",
                                      node.id, std::nullopt});
    }
    if (auto saved = upsert_node_row(impl_->db, node); !saved) {
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return saved;
    }
  }
  for (const auto &node_id : commit.changes.erase_ids) {
    Statement erase(impl_->db,
                    "DELETE FROM nodes WHERE id=?1 AND source_id=?2");
    if (!erase.ready()) {
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(erase.error("准备清理来源节点失败"));
    }
    bind_text(erase.get(), 1, node_id);
    bind_text(erase.get(), 2, id);
    if (sqlite3_step(erase.get()) != SQLITE_DONE) {
      auto error = erase.error("清理来源节点失败");
      sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
      return std::unexpected(std::move(error));
    }
  }
  auto state = commit.state;
  state.last_unsupported_count = commit.report.unsupported;
  state.last_malformed_count = commit.report.malformed;
  Statement update(impl_->db,
                   "UPDATE subscriptions SET state_json=?2 WHERE id=?1");
  if (!update.ready()) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(update.error("准备更新订阅状态失败"));
  }
  bind_text(update.get(), 1, id);
  bind_text(update.get(), 2, Json(state).dump());
  if (sqlite3_step(update.get()) != SQLITE_DONE) {
    auto error = update.error("更新订阅状态失败");
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(std::move(error));
  }
  if (auto committed =
          exec(impl_->db, "COMMIT", [] { return "提交订阅库存事务失败"; });
      !committed) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return committed;
  }
  return {};
}

Result<void> Store::register_profile(const Id &id,
                                     const std::filesystem::path &path) {
  if (id.empty() || path.empty())
    return std::unexpected(invalid("Profile ID和路径不能为空"));
  Statement statement(impl_->db,
                      "INSERT INTO profiles(id,path) VALUES(?1,?2) "
                      "ON CONFLICT(id) DO UPDATE SET path=excluded.path");
  if (!statement.ready())
    return std::unexpected(statement.error("准备登记Profile失败"));
  bind_text(statement.get(), 1, id);
  bind_text(statement.get(), 2, path.string());
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("登记Profile失败"));
  return {};
}

Result<std::filesystem::path> Store::profile_path(const Id &id) const {
  Statement statement(impl_->db, "SELECT path FROM profiles WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取Profile索引失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return std::unexpected(missing("Profile", id));
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取Profile索引失败"));
  return std::filesystem::path(column_text(statement.get(), 0));
}

Result<std::vector<Id>> Store::profile_ids() const {
  Statement statement(impl_->db, "SELECT id FROM profiles ORDER BY id");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取Profile列表失败"));
  std::vector<Id> values;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE)
      break;
    if (status != SQLITE_ROW)
      return std::unexpected(statement.error("读取Profile列表失败"));
    values.push_back(column_text(statement.get(), 0));
  }
  return values;
}

Result<void> Store::unregister_profile(const Id &id) {
  Statement statement(impl_->db, "DELETE FROM profiles WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备删除Profile索引失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("删除Profile索引失败"));
  if (sqlite3_changes(impl_->db) == 0)
    return std::unexpected(missing("Profile", id));
  return {};
}

Result<std::vector<CoreInstall>> Store::cores() const {
  Statement statement(
      impl_->db,
      "SELECT id,name,path,reported_version,managed FROM cores ORDER BY id");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取核心列表失败"));
  std::vector<CoreInstall> values;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE)
      break;
    if (status != SQLITE_ROW)
      return std::unexpected(statement.error("读取核心列表失败"));
    values.push_back(
        {column_text(statement.get(), 0), column_text(statement.get(), 1),
         column_text(statement.get(), 2), column_text(statement.get(), 3),
         sqlite3_column_int(statement.get(), 4) != 0});
  }
  return values;
}

Result<CoreInstall> Store::get_core(const Id &id) const {
  Statement statement(
      impl_->db,
      "SELECT id,name,path,reported_version,managed FROM cores WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取核心登记失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return std::unexpected(missing("核心", id));
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取核心登记失败"));
  return CoreInstall{
      column_text(statement.get(), 0), column_text(statement.get(), 1),
      column_text(statement.get(), 2), column_text(statement.get(), 3),
      sqlite3_column_int(statement.get(), 4) != 0};
}

Result<void> Store::put_core(const CoreInstall &core) {
  if (core.id.empty() || core.name.empty() || core.path.empty() ||
      core.reported_version.empty()) {
    return std::unexpected(invalid("核心ID、名称、路径和版本均不能为空"));
  }
  Statement statement(
      impl_->db,
      "INSERT INTO cores(id,name,path,reported_version,managed) "
      "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(id) DO UPDATE SET "
      "name=excluded.name,path=excluded.path,"
      "reported_version=excluded.reported_version,managed=excluded.managed");
  if (!statement.ready())
    return std::unexpected(statement.error("准备保存核心登记失败"));
  bind_text(statement.get(), 1, core.id);
  bind_text(statement.get(), 2, core.name);
  bind_text(statement.get(), 3, core.path.string());
  bind_text(statement.get(), 4, core.reported_version);
  sqlite3_bind_int(statement.get(), 5, core.managed ? 1 : 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("保存核心登记失败"));
  return {};
}

Result<std::vector<GeodataResourceRecord>> Store::geodata_resources() const {
  Statement statement(impl_->db,
                      "SELECT record_json FROM geodata_resources ORDER BY id");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取Geodata资源列表失败"));
  std::vector<GeodataResourceRecord> records;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE)
      return records;
    if (status != SQLITE_ROW)
      return std::unexpected(statement.error("读取Geodata资源列表失败"));
    try {
      records.push_back(geodata_record_from_json(
          Json::parse(column_text(statement.get(), 0))));
    } catch (const Json::exception &error) {
      return std::unexpected(
          AppError{ErrorCode::storage_error,
                   "Geodata资源元数据损坏: " + std::string(error.what()),
                   std::nullopt, std::nullopt});
    }
  }
}

Result<GeodataResourceRecord> Store::get_geodata_resource(const Id &id) const {
  Statement statement(impl_->db,
                      "SELECT record_json FROM geodata_resources WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取Geodata资源失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return std::unexpected(missing("Geodata资源", id));
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取Geodata资源失败"));
  try {
    return geodata_record_from_json(
        Json::parse(column_text(statement.get(), 0)));
  } catch (const Json::exception &error) {
    return std::unexpected(
        AppError{ErrorCode::storage_error,
                 "Geodata资源元数据损坏: " + std::string(error.what()), id,
                 std::nullopt});
  }
}

Result<GeodataResourceRecord>
Store::put_geodata_resource(const GeodataResourceRecord &input) {
  const std::filesystem::path filename(input.file_name);
  if (input.id.empty() || input.name.empty() || input.file_name.empty() ||
      filename != filename.filename() || filename == "." || filename == ".." ||
      (input.url.empty() == input.local_path.empty())) {
    return std::unexpected(
        invalid("Geodata资源需要ID、名称、文件名和唯一的URL或本地路径"));
  }
  if (!input.local_path.empty() && !input.local_path.is_absolute()) {
    return std::unexpected(
        invalid("Geodata本地资源必须使用绝对路径", input.local_path.string()));
  }
  if (input.update_interval.count() < 0 ||
      input.update_interval > std::chrono::hours{24 * 365}) {
    return std::unexpected(invalid("Geodata自动更新周期必须在0到365天之间"));
  }
  auto record = input;
  auto existing = get_geodata_resource(input.id);
  if (existing) {
    record.current = existing->current;
    record.last_attempt = existing->last_attempt;
    record.last_success = existing->last_success;
    record.last_result = existing->last_result;
    const bool changed =
        existing->name != input.name || existing->kind != input.kind ||
        existing->file_name != input.file_name || existing->url != input.url ||
        existing->local_path != input.local_path ||
        existing->auto_update != input.auto_update ||
        existing->update_interval != input.update_interval ||
        existing->validator_core_id != input.validator_core_id;
    record.settings_revision = existing->settings_revision + (changed ? 1 : 0);
    if (existing->kind != input.kind ||
        existing->file_name != input.file_name) {
      record.current.reset();
    }
  } else if (existing.error().code == ErrorCode::not_found) {
    record.settings_revision = 1;
    record.current.reset();
    record.last_attempt = 0;
    record.last_success = 0;
    record.last_result.clear();
  } else {
    return std::unexpected(existing.error());
  }
  Statement statement(
      impl_->db,
      "INSERT INTO geodata_resources(id,settings_revision,record_json) "
      "VALUES(?1,?2,?3) ON CONFLICT(id) DO UPDATE SET "
      "settings_revision=excluded.settings_revision,record_json=excluded."
      "record_json");
  if (!statement.ready())
    return std::unexpected(statement.error("准备保存Geodata资源失败"));
  bind_text(statement.get(), 1, record.id);
  sqlite3_bind_int64(statement.get(), 2,
                     static_cast<sqlite3_int64>(record.settings_revision));
  bind_text(statement.get(), 3, geodata_record_json(record).dump());
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("保存Geodata资源失败"));
  return record;
}

Result<void> Store::commit_geodata_resource_result(
    const Id &id, std::uint64_t expected_revision,
    std::optional<GeodataAssetVersion> version, UnixMillis attempted_at,
    std::string result) {
  auto record = get_geodata_resource(id);
  if (!record)
    return std::unexpected(record.error());
  if (record->settings_revision != expected_revision) {
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "Geodata资源设置在获取期间已变化", id,
                                    std::nullopt});
  }
  if (version && (version->resource_id != id || version->kind != record->kind ||
                  version->file_name != record->file_name ||
                  version->version_id.empty() || version->path.empty())) {
    return std::unexpected(invalid("Geodata版本与资源不匹配", id));
  }
  record->last_attempt = attempted_at;
  record->last_result = std::move(result);
  if (version) {
    record->current = std::move(version);
    record->last_success = attempted_at;
  }
  Statement statement(impl_->db, "UPDATE geodata_resources SET record_json=?1 "
                                 "WHERE id=?2 AND settings_revision=?3");
  if (!statement.ready())
    return std::unexpected(statement.error("准备更新Geodata版本失败"));
  bind_text(statement.get(), 1, geodata_record_json(*record).dump());
  bind_text(statement.get(), 2, id);
  sqlite3_bind_int64(statement.get(), 3,
                     static_cast<sqlite3_int64>(expected_revision));
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("更新Geodata版本失败"));
  if (sqlite3_changes(impl_->db) != 1) {
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "Geodata资源设置在获取期间已变化", id,
                                    std::nullopt});
  }
  return {};
}

Result<void> Store::remove_geodata_resource(const Id &id) {
  Statement statement(impl_->db, "DELETE FROM geodata_resources WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备删除Geodata资源失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("删除Geodata资源失败"));
  if (sqlite3_changes(impl_->db) != 1)
    return std::unexpected(missing("Geodata资源", id));
  return {};
}

Result<RuntimePersistent> Store::load_runtime() const {
  Statement statement(impl_->db,
                      "SELECT document_json FROM runtime_state WHERE id=1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备运行状态读取失败"));
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return RuntimePersistent{};
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取运行状态失败"));
  try {
    return Json::parse(column_text(statement.get(), 0))
        .get<RuntimePersistent>();
  } catch (const Json::exception &exception) {
    return std::unexpected(
        AppError{ErrorCode::storage_error,
                 "运行状态JSON损坏: " + std::string(exception.what()),
                 std::nullopt, std::nullopt});
  }
}

Result<void> Store::save_runtime(const RuntimePersistent &runtime) {
  Statement statement(
      impl_->db,
      "INSERT INTO runtime_state(id, document_json) VALUES(1, ?1) "
      "ON CONFLICT(id) DO UPDATE SET document_json=excluded.document_json");
  if (!statement.ready())
    return std::unexpected(statement.error("准备运行状态保存失败"));
  try {
    const auto bytes = Json(runtime).dump();
    bind_text(statement.get(), 1, bytes);
  } catch (const Json::exception &exception) {
    return std::unexpected(
        AppError{ErrorCode::invalid_argument,
                 "运行状态不能序列化: " + std::string(exception.what()),
                 std::nullopt, std::nullopt});
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("保存运行状态失败"));
  return {};
}

Result<void> Store::remove_core(const Id &id) {
  Statement statement(impl_->db, "DELETE FROM cores WHERE id=?1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备删除核心登记失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("删除核心登记失败"));
  if (sqlite3_changes(impl_->db) == 0)
    return std::unexpected(missing("核心", id));
  return {};
}

Result<Json> Store::load_program_settings() const {
  Statement statement(impl_->db,
                      "SELECT document_json FROM program_settings WHERE id=1");
  if (!statement.ready())
    return std::unexpected(statement.error("准备读取程序设置失败"));
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE)
    return default_program_settings();
  if (status != SQLITE_ROW)
    return std::unexpected(statement.error("读取程序设置失败"));
  try {
    auto stored = Json::parse(column_text(statement.get(), 0));
    if (!stored.is_object())
      return std::unexpected(AppError{ErrorCode::storage_error,
                                      "程序设置JSON不是对象", std::nullopt,
                                      std::nullopt});
    return stored;
  } catch (const Json::exception &error) {
    return std::unexpected(
        AppError{ErrorCode::storage_error,
                 "程序设置JSON损坏: " + std::string(error.what()), std::nullopt,
                 std::nullopt});
  }
}

Result<Json> Store::update_program_settings(const Json &patch) {
  auto current = load_program_settings();
  if (!current)
    return std::unexpected(current.error());
  auto next = apply_program_settings_patch(std::move(*current), patch);
  if (!next)
    return std::unexpected(next.error());
  Statement statement(
      impl_->db,
      "INSERT INTO program_settings(id,document_json) VALUES(1,?1) "
      "ON CONFLICT(id) DO UPDATE SET document_json=excluded.document_json");
  if (!statement.ready())
    return std::unexpected(statement.error("准备保存程序设置失败"));
  const auto serialized = next->dump();
  bind_text(statement.get(), 1, serialized);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::unexpected(statement.error("保存程序设置失败"));
  return next;
}

} // namespace xrm
