#include "infra/store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string_view>
#include <utility>

namespace xrm {
namespace {

AppError db_error(sqlite3* db, std::string_view operation) {
  std::string message(operation);
  message += ": ";
  message += db ? sqlite3_errmsg(db) : "SQLite database unavailable";
  return {ErrorCode::storage_error, std::move(message), std::nullopt, std::nullopt};
}

AppError invalid(std::string message, std::string subject = {}) {
  return {ErrorCode::invalid_argument, std::move(message),
          subject.empty() ? std::nullopt : std::optional<std::string>(std::move(subject)),
          std::nullopt};
}

AppError missing(std::string kind, const Id& id) {
  return {ErrorCode::not_found, kind + "不存在", id, std::nullopt};
}

class Statement {
 public:
  Statement(sqlite3* db, std::string_view sql) : db_(db) {
    result_ = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr);
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  bool ready() const { return result_ == SQLITE_OK; }
  sqlite3_stmt* get() const { return statement_; }
  AppError error(std::string_view operation) const { return db_error(db_, operation); }

 private:
  sqlite3* db_;
  sqlite3_stmt* statement_ = nullptr;
  int result_ = SQLITE_ERROR;
};

void bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
  sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* statement, int column) {
  const auto* value = sqlite3_column_text(statement, column);
  const auto bytes = sqlite3_column_bytes(statement, column);
  return value ? std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes))
               : std::string{};
}

template <class F>
Result<void> exec(sqlite3* db, std::string_view sql, F&& operation) {
  char* message = nullptr;
  const int status = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &message);
  sqlite3_free(message);
  if (status != SQLITE_OK) return std::unexpected(db_error(db, operation()));
  return {};
}

std::string folded(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool contains_all_labels(const std::vector<std::string>& node_labels,
                         const std::vector<std::string>& required) {
  return std::all_of(required.begin(), required.end(), [&](const auto& label) {
    return std::find(node_labels.begin(), node_labels.end(), label) != node_labels.end();
  });
}

Node node_from_columns(sqlite3_stmt* statement) {
  Json labels = Json::parse(column_text(statement, 4));
  Json outbound = Json::parse(column_text(statement, 7));
  Node node{column_text(statement, 0), std::nullopt,
            column_text(statement, 2), std::nullopt,
            labels.get<std::vector<std::string>>(),
            sqlite3_column_int(statement, 5) != 0,
            sqlite3_column_int(statement, 6) != 0, std::move(outbound)};
  if (sqlite3_column_type(statement, 1) != SQLITE_NULL) {
    node.source_id = column_text(statement, 1);
  }
  if (sqlite3_column_type(statement, 3) != SQLITE_NULL) {
    node.alias = column_text(statement, 3);
  }
  return node;
}

}  // namespace

struct Store::Impl {
  explicit Impl(sqlite3* handle) : db(handle) {}
  ~Impl() { if (db) sqlite3_close_v2(db); }
  sqlite3* db = nullptr;
};

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open(std::filesystem::path database) {
  if (database.empty()) return std::unexpected(invalid("数据库路径不能为空"));
  sqlite3* handle = nullptr;
  const auto native_path = database.string();
  const int status = sqlite3_open_v2(native_path.c_str(), &handle,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                         SQLITE_OPEN_FULLMUTEX,
                                     nullptr);
  if (status != SQLITE_OK) {
    auto error = db_error(handle, "打开数据库失败");
    if (handle) sqlite3_close_v2(handle);
    return std::unexpected(std::move(error));
  }
  sqlite3_busy_timeout(handle, 5000);
  auto impl = std::make_unique<Impl>(handle);
  const char* schema =
      "PRAGMA journal_mode=WAL;"
      "CREATE TABLE IF NOT EXISTS nodes ("
      "id TEXT PRIMARY KEY NOT NULL, source_id TEXT, original_name TEXT NOT NULL,"
      "alias TEXT, labels_json TEXT NOT NULL, enabled INTEGER NOT NULL,"
      "source_missing INTEGER NOT NULL, outbound_json TEXT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS nodes_source_idx ON nodes(source_id);"
      "CREATE TABLE IF NOT EXISTS profiles (id TEXT PRIMARY KEY NOT NULL, path TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS cores ("
      "id TEXT PRIMARY KEY NOT NULL, name TEXT NOT NULL, path TEXT NOT NULL,"
      "reported_version TEXT NOT NULL, managed INTEGER NOT NULL);";
  char* message = nullptr;
  const int schema_status = sqlite3_exec(handle, schema, nullptr, nullptr, &message);
  sqlite3_free(message);
  if (schema_status != SQLITE_OK) return std::unexpected(db_error(handle, "初始化数据库失败"));
  return std::unique_ptr<Store>(new Store(std::move(impl)));
}

Result<Page<Node>> Store::list_nodes(const NodeQuery& query, PageRequest request) const {
  if (request.limit == 0 || request.limit > 1000) {
    return std::unexpected(invalid("节点列表limit必须在1到1000之间", "limit"));
  }
  Statement statement(impl_->db, "SELECT id, source_id, original_name, alias, labels_json, "
                                 "enabled, source_missing, outbound_json FROM nodes ORDER BY id");
  if (!statement.ready()) return std::unexpected(statement.error("准备节点列表失败"));
  std::vector<Node> matches;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE) break;
    if (status != SQLITE_ROW) return std::unexpected(statement.error("读取节点列表失败"));
    try {
      Node node = node_from_columns(statement.get());
      if (query.source_id && node.source_id != query.source_id) continue;
      if (!query.include_missing && node.source_missing) continue;
      if (query.text) {
        const auto text = folded(*query.text);
        if (folded(node.original_name).find(text) == std::string::npos &&
            (!node.alias || folded(*node.alias).find(text) == std::string::npos)) continue;
      }
      if (!contains_all_labels(node.labels, query.labels)) continue;
      matches.push_back(std::move(node));
    } catch (const std::exception& error) {
      return std::unexpected(AppError{ErrorCode::storage_error,
                                      std::string("节点库存包含无效数据: ") + error.what(),
                                      std::nullopt, std::nullopt});
    }
  }

  Page<Node> page;
  page.total = matches.size();
  if (request.offset < matches.size()) {
    const auto end = std::min(matches.size(), request.offset + request.limit);
    page.items.insert(page.items.end(),
                      std::make_move_iterator(matches.begin() + request.offset),
                      std::make_move_iterator(matches.begin() + end));
    if (end < matches.size()) page.next_offset = end;
  }
  return page;
}

Result<Node> Store::get_node(const Id& id) const {
  Statement statement(impl_->db, "SELECT id,source_id,original_name,alias,labels_json,enabled,"
                                 "source_missing,outbound_json FROM nodes WHERE id=?1");
  if (!statement.ready()) return std::unexpected(statement.error("准备读取节点失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE) return std::unexpected(missing("节点", id));
  if (status != SQLITE_ROW) return std::unexpected(statement.error("读取节点失败"));
  try {
    return node_from_columns(statement.get());
  } catch (const std::exception& error) {
    return std::unexpected(AppError{ErrorCode::storage_error,
                                    std::string("节点数据无效: ") + error.what(), id, std::nullopt});
  }
}

Result<std::vector<Node>> Store::all_nodes() const {
  const auto page = list_nodes(NodeQuery{.include_missing = true}, {0, 1000});
  if (!page) return std::unexpected(page.error());
  if (page->total > page->items.size()) {
    return std::unexpected(AppError{ErrorCode::too_large, "节点数量超过单次读取上限",
                                    std::nullopt, "使用分页列表"});
  }
  return page->items;
}

Result<void> Store::put_node(const Node& node) {
  if (node.id.empty()) return std::unexpected(invalid("节点ID不能为空", "id"));
  if (node.original_name.empty()) return std::unexpected(invalid("节点名称不能为空", "name"));
  if (!node.outbound.is_object()) return std::unexpected(invalid("原生outbound必须是对象", "outbound"));
  const auto json = Json(node);
  if (auto started = exec(impl_->db, "BEGIN IMMEDIATE", [] { return "开始保存节点事务失败"; }); !started) {
    return started;
  }
  Statement statement(impl_->db, "INSERT INTO nodes(id,source_id,original_name,alias,labels_json,"
                                 "enabled,source_missing,outbound_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
                                 "ON CONFLICT(id) DO UPDATE SET source_id=excluded.source_id,"
                                 "original_name=excluded.original_name,alias=excluded.alias,"
                                 "labels_json=excluded.labels_json,enabled=excluded.enabled,"
                                 "source_missing=excluded.source_missing,outbound_json=excluded.outbound_json");
  if (!statement.ready()) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(statement.error("准备保存节点失败"));
  }
  bind_text(statement.get(), 1, node.id);
  if (node.source_id) bind_text(statement.get(), 2, *node.source_id); else sqlite3_bind_null(statement.get(), 2);
  bind_text(statement.get(), 3, node.original_name);
  if (node.alias) bind_text(statement.get(), 4, *node.alias); else sqlite3_bind_null(statement.get(), 4);
  bind_text(statement.get(), 5, json.at("labels").dump());
  sqlite3_bind_int(statement.get(), 6, node.enabled ? 1 : 0);
  sqlite3_bind_int(statement.get(), 7, node.source_missing ? 1 : 0);
  bind_text(statement.get(), 8, node.outbound.dump());
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    auto error = statement.error("保存节点失败");
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::unexpected(std::move(error));
  }
  if (auto committed = exec(impl_->db, "COMMIT", [] { return "提交节点保存失败"; }); !committed) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    return committed;
  }
  return {};
}

Result<void> Store::erase_node(const Id& id) {
  Statement statement(impl_->db, "DELETE FROM nodes WHERE id=?1");
  if (!statement.ready()) return std::unexpected(statement.error("准备删除节点失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return std::unexpected(statement.error("删除节点失败"));
  if (sqlite3_changes(impl_->db) == 0) return std::unexpected(missing("节点", id));
  return {};
}

Result<void> Store::register_profile(const Id& id, const std::filesystem::path& path) {
  if (id.empty() || path.empty()) return std::unexpected(invalid("Profile ID和路径不能为空"));
  Statement statement(impl_->db, "INSERT INTO profiles(id,path) VALUES(?1,?2) "
                                 "ON CONFLICT(id) DO UPDATE SET path=excluded.path");
  if (!statement.ready()) return std::unexpected(statement.error("准备登记Profile失败"));
  bind_text(statement.get(), 1, id);
  bind_text(statement.get(), 2, path.string());
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return std::unexpected(statement.error("登记Profile失败"));
  return {};
}

Result<std::filesystem::path> Store::profile_path(const Id& id) const {
  Statement statement(impl_->db, "SELECT path FROM profiles WHERE id=?1");
  if (!statement.ready()) return std::unexpected(statement.error("准备读取Profile索引失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE) return std::unexpected(missing("Profile", id));
  if (status != SQLITE_ROW) return std::unexpected(statement.error("读取Profile索引失败"));
  return std::filesystem::path(column_text(statement.get(), 0));
}

Result<std::vector<Id>> Store::profile_ids() const {
  Statement statement(impl_->db, "SELECT id FROM profiles ORDER BY id");
  if (!statement.ready()) return std::unexpected(statement.error("准备读取Profile列表失败"));
  std::vector<Id> values;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE) break;
    if (status != SQLITE_ROW) return std::unexpected(statement.error("读取Profile列表失败"));
    values.push_back(column_text(statement.get(), 0));
  }
  return values;
}

Result<void> Store::unregister_profile(const Id& id) {
  Statement statement(impl_->db, "DELETE FROM profiles WHERE id=?1");
  if (!statement.ready()) return std::unexpected(statement.error("准备删除Profile索引失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return std::unexpected(statement.error("删除Profile索引失败"));
  if (sqlite3_changes(impl_->db) == 0) return std::unexpected(missing("Profile", id));
  return {};
}

Result<std::vector<CoreInstall>> Store::cores() const {
  Statement statement(impl_->db, "SELECT id,name,path,reported_version,managed FROM cores ORDER BY id");
  if (!statement.ready()) return std::unexpected(statement.error("准备读取核心列表失败"));
  std::vector<CoreInstall> values;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE) break;
    if (status != SQLITE_ROW) return std::unexpected(statement.error("读取核心列表失败"));
    values.push_back({column_text(statement.get(), 0), column_text(statement.get(), 1),
                      column_text(statement.get(), 2), column_text(statement.get(), 3),
                      sqlite3_column_int(statement.get(), 4) != 0});
  }
  return values;
}

Result<CoreInstall> Store::get_core(const Id& id) const {
  Statement statement(impl_->db, "SELECT id,name,path,reported_version,managed FROM cores WHERE id=?1");
  if (!statement.ready()) return std::unexpected(statement.error("准备读取核心登记失败"));
  bind_text(statement.get(), 1, id);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE) return std::unexpected(missing("核心", id));
  if (status != SQLITE_ROW) return std::unexpected(statement.error("读取核心登记失败"));
  return CoreInstall{column_text(statement.get(), 0), column_text(statement.get(), 1),
                     column_text(statement.get(), 2), column_text(statement.get(), 3),
                     sqlite3_column_int(statement.get(), 4) != 0};
}

Result<void> Store::put_core(const CoreInstall& core) {
  if (core.id.empty() || core.name.empty() || core.path.empty() || core.reported_version.empty()) {
    return std::unexpected(invalid("核心ID、名称、路径和版本均不能为空"));
  }
  Statement statement(impl_->db, "INSERT INTO cores(id,name,path,reported_version,managed) "
                                 "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(id) DO UPDATE SET "
                                 "name=excluded.name,path=excluded.path,"
                                 "reported_version=excluded.reported_version,managed=excluded.managed");
  if (!statement.ready()) return std::unexpected(statement.error("准备保存核心登记失败"));
  bind_text(statement.get(), 1, core.id);
  bind_text(statement.get(), 2, core.name);
  bind_text(statement.get(), 3, core.path.string());
  bind_text(statement.get(), 4, core.reported_version);
  sqlite3_bind_int(statement.get(), 5, core.managed ? 1 : 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return std::unexpected(statement.error("保存核心登记失败"));
  return {};
}

Result<void> Store::remove_core(const Id& id) {
  Statement statement(impl_->db, "DELETE FROM cores WHERE id=?1");
  if (!statement.ready()) return std::unexpected(statement.error("准备删除核心登记失败"));
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return std::unexpected(statement.error("删除核心登记失败"));
  if (sqlite3_changes(impl_->db) == 0) return std::unexpected(missing("核心", id));
  return {};
}

}  // namespace xrm
