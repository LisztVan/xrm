#include "server/application.hpp"

#include "xray/process_adapter.hpp"

#include <array>
#include <random>
#include <unistd.h>

namespace xrm {
namespace {

Id new_id() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char digits[] = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) id.push_back('-');
    id.push_back(digits[bytes[index] >> 4]);
    id.push_back(digits[bytes[index] & 0x0f]);
  }
  return id;
}

AppError invalid(std::string message, std::string subject) {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject), std::nullopt};
}

}  // namespace

Application::Application(Store& store, ProcessService& processes,
                         std::string application_version, Id boot_id)
    : store_(&store), processes_(&processes),
      application_version_(std::move(application_version)), boot_id_(std::move(boot_id)) {}

Json Application::server_info() const {
  return Json{{"version", application_version_}, {"protocol", 1}, {"boot_id", boot_id_}};
}

Result<NodeSummary> Application::add_manual_node(std::string name, Json outbound) {
  if (name.empty()) return std::unexpected(invalid("节点名称不能为空", "name"));
  if (!outbound.is_object()) return std::unexpected(invalid("outbound必须是原生JSON对象", "outbound"));
  if (!outbound.contains("protocol") || !outbound.at("protocol").is_string() ||
      outbound.at("protocol").get<std::string>().empty()) {
    return std::unexpected(invalid("outbound必须包含非空protocol", "outbound.protocol"));
  }
  Node node{new_id(), std::nullopt, name, std::nullopt, {}, true, false,
            std::move(outbound)};
  if (auto stored = store_->put_node(node); !stored) return std::unexpected(stored.error());
  return summarize_node(node);
}

Result<Page<NodeSummary>> Application::list_nodes(const NodeQuery& query,
                                                  PageRequest request) const {
  auto nodes = store_->list_nodes(query, request);
  if (!nodes) return std::unexpected(nodes.error());
  Page<NodeSummary> page;
  page.total = nodes->total;
  page.next_offset = nodes->next_offset;
  page.items.reserve(nodes->items.size());
  for (const auto& node : nodes->items) page.items.push_back(summarize_node(node));
  return page;
}

Result<std::vector<CoreInstall>> Application::list_cores() const {
  return store_->cores();
}

void Application::register_core(std::filesystem::path path, std::string name,
                                Completion<CoreInstall> completion) {
  if (path.empty() || name.empty()) {
    completion(std::unexpected(invalid("核心路径和名称不能为空", "path")));
    return;
  }
  if (!path.is_absolute() || ::access(path.c_str(), X_OK) != 0) {
    completion(std::unexpected(invalid("核心必须是可执行文件的绝对路径", "path")));
    return;
  }
  const auto id = new_id();
  auto* store = store_;
  const auto core_name = std::move(name);
  const auto core_path = std::move(path);
  auto accepted = processes_->execute(
      version_command(core_path), Milliseconds{5000},
      [completion, store, id, core_name, core_path](
          Result<ProcessResult> process_result) mutable {
        if (!process_result) {
          completion(std::unexpected(process_result.error()));
          return;
        }
        auto version = read_core_version(*process_result);
        if (!version) {
          completion(std::unexpected(version.error()));
          return;
        }
        CoreInstall core{id, core_name, core_path, std::move(*version), false};
        if (auto saved = store->put_core(core); !saved) {
          completion(std::unexpected(saved.error()));
          return;
        }
        completion(std::move(core));
      });
  if (!accepted) completion(std::unexpected(accepted.error()));
}

}  // namespace xrm
