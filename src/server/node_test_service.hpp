// XRM: Declares the node test service.

#pragma once

#include "infra/process.hpp"
#include "model/node.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace xrm {

class HttpClient;

enum class NodeTestKind { icmp, tcp, url, download };
enum class NodeTestStatus { success, failed, not_applicable, cancelled };

struct NodeTestRequest {
  Node node;
  NodeTestKind kind = NodeTestKind::tcp;
  std::string target_url;
  Milliseconds timeout{5000};
  std::size_t max_body_bytes = 8 * 1024 * 1024;
  std::filesystem::path xray_binary;
  std::filesystem::path temporary_root;
};

struct NodeTestResult {
  Id node_id;
  NodeTestKind kind = NodeTestKind::tcp;
  NodeTestStatus status = NodeTestStatus::failed;
  std::string target;
  UnixMillis sampled_at = 0;
  std::optional<double> resolve_ms;
  std::optional<double> latency_ms;
  std::optional<std::size_t> received_bytes;
  std::optional<double> bytes_per_second;
  std::string message;
};

// Owns only temporary test processes and sockets. It never reads or changes the
// production CoreManager, selection, RunRecord, Store, or runtime directory.
// Callbacks are delivered on the supplied uv loop.
class NodeTestService {
public:
  NodeTestService(uv_loop_t &, ProcessService &, HttpClient &,
                  std::filesystem::path ping_binary = "/usr/bin/ping");
  ~NodeTestService();
  NodeTestService(const NodeTestService &) = delete;
  NodeTestService &operator=(const NodeTestService &) = delete;

  Result<Id> start(NodeTestRequest, Completion<NodeTestResult>);
  Result<void> cancel(const Id &);
  void shutdown(std::function<void()> drained);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace xrm
