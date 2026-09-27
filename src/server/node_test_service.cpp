/// @file
/// @brief Runs requested node tests using bounded, separate test resources.
///
/// A test may prepare a temporary core/proxy, but does not mutate the active
/// runtime, saved target, or source node document.

#include "server/node_test_service.hpp"

#include "infra/http.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/runtime_writer.hpp"

#include <uv.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <netinet/in.h>
#include <regex>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace xrm {
namespace {

UnixMillis now_millis() {
  return std::chrono::duration_cast<Milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

AppError invalid(std::string message, std::string subject) {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject),
          std::nullopt};
}

std::optional<Endpoint> endpoint_of(const Node &node) {
  if (auto *stream = std::get_if<StreamNode>(&node.settings))
    return stream->endpoint;
  return std::nullopt;
}

bool tcp_applicable(const Node &node) {
  const auto *stream = std::get_if<StreamNode>(&node.settings);
  return stream &&
         !std::holds_alternative<HysteriaTransport>(stream->transport) &&
         !std::holds_alternative<Hysteria2Settings>(stream->protocol);
}

std::optional<double> ping_time(const std::string &output) {
  static const std::regex pattern(R"(time[=<]([0-9]+(?:\.[0-9]+)?)\s*ms)");
  std::smatch match;
  if (!std::regex_search(output, match, pattern))
    return std::nullopt;
  try {
    return std::stod(match[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

Result<std::uint16_t> free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return std::unexpected(AppError{
        ErrorCode::io_error, "无法为测试核心分配loopback端口", {}, {}});
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  const auto bound =
      ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  socklen_t length = sizeof(address);
  const auto named =
      bound == 0
          ? ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length)
          : -1;
  ::close(fd);
  if (bound != 0 || named != 0)
    return std::unexpected(AppError{
        ErrorCode::io_error, "无法为测试核心分配loopback端口", {}, {}});
  return ntohs(address.sin_port);
}

PreparedFiles isolated_files(const Json &outbound, std::uint16_t port) {
  constexpr auto inbound_tag = "xrm-node-test-in";
  constexpr auto outbound_tag = "xrm-node-test-out";
  PreparedFiles files;
  Json inbound = {
      {"inbounds", Json::array({Json{
                       {"listen", "127.0.0.1"},
                       {"port", port},
                       {"protocol", "socks"},
                       {"tag", inbound_tag},
                       {"settings", {{"auth", "noauth"}, {"udp", false}}}}})}};
  Json outbounds = {{"outbounds", Json::array({outbound})}};
  Json routing = {
      {"routing",
       {{"domainStrategy", "AsIs"},
        {"rules", Json::array({Json{{"type", "field"},
                                    {"inboundTag", Json::array({inbound_tag})},
                                    {"network", "tcp"},
                                    {"outboundTag", outbound_tag}}})}}}};
  files.files.push_back({"01_inbounds.json", inbound.dump(2) + "\n"});
  files.files.push_back({"03_outbounds.json", outbounds.dump(2) + "\n"});
  files.files.push_back({"04_routing.json", routing.dump(2) + "\n"});
  return files;
}

} // namespace

struct NodeTestService::Impl : std::enable_shared_from_this<Impl> {
  // Each Task owns its timer, optional resolver/process/HTTP request, and
  // completion. finish() and shutdown must drain these before releasing it.
  struct Task;
  struct Socket {
    std::shared_ptr<Task> task;
    uv_tcp_t tcp{};
    uv_connect_t connect{};
    std::chrono::steady_clock::time_point started;
  };
  struct Task {
    enum class ProxyPhase { none, checking, waiting, fetching };
    std::shared_ptr<Impl> owner;
    Id id;
    NodeTestRequest request;
    Completion<NodeTestResult> completion;
    NodeTestResult result;
    uv_timer_t timer{};
    bool timer_open = false;
    bool resolver_pending = false;
    bool finished = false;
    bool cancelled = false;
    uv_getaddrinfo_t resolver{};
    addrinfo *addresses = nullptr;
    addrinfo *next_address = nullptr;
    std::optional<Id> process_command;
    std::optional<Id> http_request;
    std::optional<ProcessId> test_process;
    std::unique_ptr<RuntimeWriter> writer;
    std::optional<PreparedDirectory> prepared;
    std::string test_stderr;
    std::uint16_t proxy_port = 0;
    ProxyPhase proxy_phase = ProxyPhase::none;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point http_started;
    Socket *socket = nullptr;
    std::size_t pending_handles = 0;
    std::optional<NodeTestResult> deferred;

    ~Task() {
      if (addresses)
        uv_freeaddrinfo(addresses);
    }
  };

  uv_loop_t *loop;
  ProcessService *process;
  HttpClient *http;
  std::filesystem::path ping_binary;
  std::uint64_t next_id = 1;
  bool shutting_down = false;
  std::function<void()> drained;
  std::unordered_map<Id, std::shared_ptr<Task>> tasks;

  Impl(uv_loop_t &event_loop, ProcessService &processes, HttpClient &client,
       std::filesystem::path ping)
      : loop(&event_loop), process(&processes), http(&client),
        ping_binary(std::move(ping)) {}

  static void timer_closed(uv_handle_t *handle) {
    auto *task = static_cast<Task *>(handle->data);
    auto owner = task->owner;
    --task->pending_handles;
    owner->maybe_finish(*task);
  }

  static void socket_closed(uv_handle_t *handle) {
    auto *socket = static_cast<Socket *>(handle->data);
    const auto task = socket->task;
    delete socket;
    --task->pending_handles;
    task->owner->maybe_finish(*task);
  }

  void close_socket(Task &task) {
    if (!task.socket)
      return;
    auto *socket = task.socket;
    task.socket = nullptr;
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&socket->tcp))) {
      uv_close(reinterpret_cast<uv_handle_t *>(&socket->tcp), socket_closed);
    }
  }

  void finish(Task &task, NodeTestStatus status, std::string message = {}) {
    if (task.finished)
      return;
    task.finished = true;
    task.result.status = status;
    task.result.sampled_at = now_millis();
    task.result.message = std::move(message);
    close_socket(task);
    if (task.resolver_pending)
      uv_cancel(reinterpret_cast<uv_req_t *>(&task.resolver));
    if (task.http_request) {
      (void)http->cancel(*task.http_request);
    }
    if (task.test_process) {
      (void)process->terminate(*task.test_process, Milliseconds{1000});
    }
    if (task.timer_open) {
      task.timer_open = false;
      uv_close(reinterpret_cast<uv_handle_t *>(&task.timer), timer_closed);
    }
    maybe_finish(task);
  }

  void maybe_finish(Task &task) {
    if (!task.finished || task.pending_handles != 0 || task.resolver_pending ||
        task.process_command || task.http_request || task.test_process)
      return;
    if (task.prepared && task.writer) {
      const auto discarded = task.writer->discard(*task.prepared, {});
      if (!discarded && task.result.status == NodeTestStatus::success) {
        task.result.status = NodeTestStatus::failed;
        task.result.message =
            "测试已结束，但临时配置清理失败: " + discarded.error().message;
      }
    }
    const auto id = task.id;
    auto completion = std::move(task.completion);
    auto result = std::move(task.result);
    tasks.erase(id);
    if (completion)
      completion(std::move(result));
    if (shutting_down && tasks.empty() && drained) {
      auto callback = std::move(drained);
      callback();
    }
  }

  static void on_timer(uv_timer_t *timer) {
    auto *task = static_cast<Task *>(timer->data);
    auto owner = task->owner;
    if (task->deferred) {
      task->result = std::move(*task->deferred);
      task->deferred.reset();
      owner->finish(*task, task->result.status,
                    std::move(task->result.message));
    } else if (task->proxy_phase == Task::ProxyPhase::waiting) {
      if (std::chrono::steady_clock::now() - task->started >
          task->request.timeout) {
        owner->finish(*task, NodeTestStatus::failed,
                      "隔离测试核心未提供loopback SOCKS入口: " +
                          task->test_stderr);
      } else if (!task->socket) {
        owner->probe_proxy(*task);
      }
    } else {
      owner->finish(*task, NodeTestStatus::failed, "TCP连接超时");
    }
  }

  static void on_connected(uv_connect_t *request, int status) {
    auto *socket = static_cast<Socket *>(request->data);
    const auto task = socket->task;
    const auto owner = task->owner;
    if (task->finished)
      return;
    if (task->proxy_phase == Task::ProxyPhase::waiting) {
      owner->close_socket(*task);
      if (status == 0)
        owner->start_http(*task);
      return;
    }
    if (status == 0) {
      task->result.latency_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - socket->started)
              .count();
      owner->finish(*task, NodeTestStatus::success,
                    "TCP连接已建立；未验证代理认证");
      return;
    }
    owner->close_socket(*task);
    if (task->next_address) {
      owner->connect_next(*task);
    } else {
      owner->finish(*task, NodeTestStatus::failed,
                    std::string("TCP连接失败: ") + uv_strerror(status));
    }
  }

  void probe_proxy(Task &task) {
    auto *socket = new Socket{};
    socket->task = tasks.at(task.id);
    const int initialized = uv_tcp_init(loop, &socket->tcp);
    if (initialized != 0) {
      delete socket;
      finish(task, NodeTestStatus::failed, uv_strerror(initialized));
      return;
    }
    ++task.pending_handles;
    socket->tcp.data = socket;
    socket->connect.data = socket;
    sockaddr_in address{};
    uv_ip4_addr("127.0.0.1", task.proxy_port, &address);
    const int started =
        uv_tcp_connect(&socket->connect, &socket->tcp,
                       reinterpret_cast<sockaddr *>(&address), on_connected);
    if (started != 0) {
      uv_close(reinterpret_cast<uv_handle_t *>(&socket->tcp), socket_closed);
      return;
    }
    task.socket = socket;
  }

  void start_http(Task &task) {
    if (task.finished || task.proxy_phase != Task::ProxyPhase::waiting)
      return;
    task.proxy_phase = Task::ProxyPhase::fetching;
    uv_timer_stop(&task.timer);
    HttpRequest request;
    request.url = task.request.target_url;
    request.user_agent = "XRM node test";
    request.route.proxy_url =
        "socks5h://127.0.0.1:" + std::to_string(task.proxy_port);
    const auto elapsed = std::chrono::duration_cast<Milliseconds>(
        std::chrono::steady_clock::now() - task.started);
    if (elapsed >= task.request.timeout) {
      finish(task, NodeTestStatus::failed, "测试已超过总超时");
      return;
    }
    request.timeout = task.request.timeout - elapsed;
    request.max_body_bytes = task.request.max_body_bytes;
    task.http_started = std::chrono::steady_clock::now();
    auto id = http->fetch(
        std::move(request), [weak = std::weak_ptr<Task>(tasks.at(task.id))](
                                Result<HttpResponse> reply) {
          auto current = weak.lock();
          if (!current)
            return;
          auto owner = current->owner;
          current->http_request.reset();
          if (current->finished) {
            owner->maybe_finish(*current);
            return;
          }
          if (!reply) {
            owner->finish(*current, NodeTestStatus::failed,
                          "经测试代理请求失败: " + reply.error().message);
            return;
          }
          if (reply->status < 200 || reply->status >= 300) {
            owner->finish(*current, NodeTestStatus::failed,
                          "测试目标HTTP状态: " + std::to_string(reply->status));
            return;
          }
          const auto elapsed =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - current->http_started)
                  .count();
          current->result.latency_ms = elapsed;
          current->result.received_bytes = reply->body.size();
          if (current->request.kind == NodeTestKind::download && elapsed > 0) {
            current->result.bytes_per_second =
                static_cast<double>(reply->body.size()) * 1000.0 / elapsed;
          }
          owner->finish(*current, NodeTestStatus::success,
                        "经隔离Xray代理收到HTTP响应；非生产核心");
        });
    if (id)
      task.http_request = *id;
    else
      finish(task, NodeTestStatus::failed, id.error().message);
  }

  void start_proxy(Task &task) {
    task.result.target = task.request.target_url;
    if (task.request.xray_binary.empty() ||
        task.request.temporary_root.empty()) {
      task.deferred = task.result;
      task.deferred->status = NodeTestStatus::not_applicable;
      task.deferred->message =
          "需指定独立测试Xray程序和临时目录；不会借用生产核心";
      uv_timer_start(&task.timer, on_timer, 0, 0);
      return;
    }
    if (!(task.request.target_url.starts_with("http://") ||
          task.request.target_url.starts_with("https://"))) {
      task.deferred = task.result;
      task.deferred->status = NodeTestStatus::failed;
      task.deferred->message = "测试目标必须是HTTP(S) URL";
      uv_timer_start(&task.timer, on_timer, 0, 0);
      return;
    }
    auto port = free_loopback_port();
    if (!port) {
      finish(task, NodeTestStatus::failed, port.error().message);
      return;
    }
    task.proxy_port = *port;
    auto outbound = write_outbound(task.request.node, "xrm-node-test-out");
    if (!outbound) {
      finish(task, NodeTestStatus::not_applicable,
             "节点无法单独转换为Xray出站: " + outbound.error().message);
      return;
    }
    task.writer = std::make_unique<RuntimeWriter>(task.request.temporary_root);
    auto prepared =
        task.writer->materialize(isolated_files(*outbound, task.proxy_port),
                                 DirectoryPurpose::preview, task.id);
    if (!prepared) {
      finish(task, NodeTestStatus::failed, prepared.error().message);
      return;
    }
    task.prepared = std::move(*prepared);
    task.proxy_phase = Task::ProxyPhase::checking;
    ProcessSpec check{
        task.request.xray_binary,
        {"run", "-test", "-confdir", task.prepared->confdir.string()},
        task.request.temporary_root,
        {}};
    auto command = process->execute(
        std::move(check), task.request.timeout,
        [weak = std::weak_ptr<Task>(tasks.at(task.id))](
            Result<ProcessResult> checked) {
          auto current = weak.lock();
          if (!current)
            return;
          auto owner = current->owner;
          current->process_command.reset();
          if (current->cancelled) {
            owner->finish(*current, NodeTestStatus::cancelled, "测试已取消");
            return;
          }
          if (!checked) {
            owner->finish(*current, NodeTestStatus::failed,
                          "隔离Xray配置检查失败: " + checked.error().message);
            return;
          }
          if (checked->exit.exit_code != 0 || checked->exit.term_signal != 0) {
            owner->finish(*current, NodeTestStatus::failed,
                          "隔离Xray配置检查失败: " + checked->stderr_text);
            return;
          }
          ProcessSpec run{
              current->request.xray_binary,
              {"run", "-confdir", current->prepared->confdir.string()},
              current->request.temporary_root,
              {}};
          ProcessCallbacks callbacks;
          callbacks.stderr_chunk = [weak](ProcessId, std::string chunk) {
            if (auto current = weak.lock()) {
              constexpr std::size_t limit = 4096;
              current->test_stderr.append(chunk.substr(
                  0, limit - std::min(limit, current->test_stderr.size())));
            }
          };
          callbacks.exited = [weak](ProcessId, ProcessExit exit) {
            auto current = weak.lock();
            if (!current)
              return;
            auto owner = current->owner;
            current->test_process.reset();
            if (!current->finished) {
              owner->finish(*current, NodeTestStatus::failed,
                            "隔离测试Xray意外退出 (" +
                                std::to_string(exit.exit_code) +
                                "): " + current->test_stderr);
            } else {
              owner->maybe_finish(*current);
            }
          };
          auto spawned =
              owner->process->spawn(std::move(run), std::move(callbacks));
          if (!spawned) {
            owner->finish(*current, NodeTestStatus::failed,
                          "启动隔离测试Xray失败: " + spawned.error().message);
            return;
          }
          current->test_process = *spawned;
          current->proxy_phase = Task::ProxyPhase::waiting;
          uv_timer_start(&current->timer, on_timer, 25, 50);
        });
    if (command)
      task.process_command = *command;
    else
      finish(task, NodeTestStatus::failed, command.error().message);
  }

  void connect_next(Task &task) {
    if (task.finished)
      return;
    while (task.next_address) {
      auto *address = task.next_address;
      task.next_address = address->ai_next;
      auto *socket = new Socket{};
      socket->task = tasks.at(task.id);
      const int init = uv_tcp_init(loop, &socket->tcp);
      if (init != 0) {
        delete socket;
        finish(task, NodeTestStatus::failed, uv_strerror(init));
        return;
      }
      ++task.pending_handles;
      socket->tcp.data = socket;
      socket->connect.data = socket;
      socket->started = std::chrono::steady_clock::now();
      const int started = uv_tcp_connect(&socket->connect, &socket->tcp,
                                         address->ai_addr, on_connected);
      if (started == 0) {
        task.socket = socket;
        return;
      }
      uv_close(reinterpret_cast<uv_handle_t *>(&socket->tcp), socket_closed);
      if (!task.next_address) {
        finish(task, NodeTestStatus::failed,
               std::string("TCP连接失败: ") + uv_strerror(started));
        return;
      }
    }
  }

  static void on_resolved(uv_getaddrinfo_t *request, int status,
                          addrinfo *addresses) {
    auto *task = static_cast<Task *>(request->data);
    auto owner = task->owner;
    task->resolver_pending = false;
    if (task->finished) {
      if (addresses)
        uv_freeaddrinfo(addresses);
      owner->maybe_finish(*task);
      return;
    }
    if (status != 0 || !addresses) {
      owner->finish(*task, NodeTestStatus::failed,
                    std::string("解析节点地址失败: ") + uv_strerror(status));
      return;
    }
    task->result.resolve_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - task->started)
            .count();
    task->addresses = addresses;
    task->next_address = addresses;
    owner->connect_next(*task);
  }

  Result<Id> start(NodeTestRequest request,
                   Completion<NodeTestResult> completion) {
    if (shutting_down)
      return std::unexpected(
          AppError{ErrorCode::busy, "节点测试服务正在关闭", {}, {}});
    if (tasks.size() >= 3)
      return std::unexpected(
          AppError{ErrorCode::busy,
                   "节点测试并发上限为3，请等待或取消已有测试",
                   {},
                   {}});
    if (!completion)
      return std::unexpected(invalid("测试回调不能为空", "completion"));
    if (request.node.id.empty())
      return std::unexpected(invalid("节点ID不能为空", "node_id"));
    if (request.timeout.count() <= 0)
      return std::unexpected(invalid("超时必须大于零", "timeout"));
    if (request.max_body_bytes == 0 ||
        request.max_body_bytes > 32 * 1024 * 1024)
      return std::unexpected(
          invalid("下载上限必须在1字节至32MiB之间", "max_body_bytes"));
    if (request.timeout > Milliseconds{60000})
      return std::unexpected(invalid("单节点测试超时不能超过60秒", "timeout"));

    auto task = std::make_shared<Task>();
    task->owner = shared_from_this();
    task->id = "node-test-" + std::to_string(next_id++);
    task->request = std::move(request);
    task->started = std::chrono::steady_clock::now();
    task->completion = std::move(completion);
    task->result.node_id = task->request.node.id;
    task->result.kind = task->request.kind;
    const Id id = task->id;
    tasks.emplace(id, task);

    if (uv_timer_init(loop, &task->timer) != 0) {
      tasks.erase(id);
      return std::unexpected(
          AppError{ErrorCode::io_error, "创建测试计时器失败", {}, {}});
    }
    task->timer_open = true;
    task->timer.data = task.get();
    ++task->pending_handles;

    const auto endpoint = endpoint_of(task->request.node);
    if (task->request.kind == NodeTestKind::tcp ||
        task->request.kind == NodeTestKind::icmp) {
      if (!endpoint || endpoint->address.empty()) {
        task->deferred = task->result;
        task->deferred->status = NodeTestStatus::not_applicable;
        task->deferred->message = "原生自定义出站没有可确定的单一服务器地址";
        uv_timer_start(&task->timer, on_timer, 0, 0);
        return id;
      }
      task->result.target = endpoint->address;
    }

    if (task->request.kind == NodeTestKind::tcp) {
      task->result.target += ":" + std::to_string(endpoint->port);
      if (!tcp_applicable(task->request.node)) {
        task->deferred = task->result;
        task->deferred->status = NodeTestStatus::not_applicable;
        task->deferred->message = "纯UDP/QUIC节点不适用TCPing";
        uv_timer_start(&task->timer, on_timer, 0, 0);
        return id;
      }
      if (endpoint->port == 0) {
        finish(*task, NodeTestStatus::failed, "节点端口无效");
        return id;
      }
      uv_timer_start(&task->timer, on_timer,
                     static_cast<std::uint64_t>(task->request.timeout.count()),
                     0);
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      task->resolver.data = task.get();
      task->resolver_pending = true;
      const auto service = std::to_string(endpoint->port);
      const int status =
          uv_getaddrinfo(loop, &task->resolver, on_resolved,
                         endpoint->address.c_str(), service.c_str(), &hints);
      if (status != 0) {
        task->resolver_pending = false;
        finish(*task, NodeTestStatus::failed,
               std::string("解析节点地址失败: ") + uv_strerror(status));
      }
      return id;
    }

    if (task->request.kind == NodeTestKind::icmp) {
      if (!std::filesystem::exists(ping_binary)) {
        task->deferred = task->result;
        task->deferred->status = NodeTestStatus::not_applicable;
        task->deferred->message = "系统ping工具不可用";
        uv_timer_start(&task->timer, on_timer, 0, 0);
        return id;
      }
      const auto seconds = std::max<std::int64_t>(
          1, (task->request.timeout.count() + 999) / 1000);
      ProcessSpec spec{
          ping_binary,
          {"-n", "-c", "1", "-W", std::to_string(seconds), endpoint->address},
          {},
          {{"LC_ALL", "C"}}};
      auto command = process->execute(
          std::move(spec), task->request.timeout + Milliseconds{500},
          [weak = std::weak_ptr<Task>(task)](Result<ProcessResult> result) {
            auto current = weak.lock();
            if (!current)
              return;
            const auto owner = current->owner;
            current->process_command.reset();
            if (current->cancelled) {
              owner->finish(*current, NodeTestStatus::cancelled,
                            "ICMP测试已取消");
            } else if (!result) {
              owner->finish(*current, NodeTestStatus::failed,
                            result.error().message);
            } else if (result->exit.exit_code != 0 ||
                       result->exit.term_signal != 0) {
              owner->finish(*current, NodeTestStatus::failed,
                            result->stderr_text.empty() ? "ICMP探测失败或无应答"
                                                        : result->stderr_text);
            } else if (auto delay = ping_time(result->stdout_text)) {
              current->result.latency_ms = delay;
              owner->finish(*current, NodeTestStatus::success,
                            "ICMP应答；未验证端口或代理认证");
            } else {
              owner->finish(*current, NodeTestStatus::failed,
                            "ping输出没有可解析的往返时间");
            }
          });
      if (command)
        task->process_command = *command;
      else
        finish(*task, NodeTestStatus::failed, command.error().message);
      return id;
    }

    start_proxy(*task);
    return id;
  }

  Result<void> cancel(const Id &id) {
    auto found = tasks.find(id);
    if (found == tasks.end())
      return std::unexpected(
          AppError{ErrorCode::not_found, "测试任务不存在", id, {}});
    auto task = found->second;
    task->cancelled = true;
    if (task->process_command) {
      auto outcome = process->cancel_command(*task->process_command);
      if (!outcome)
        return outcome;
      return {};
    }
    finish(*task, NodeTestStatus::cancelled, "测试已取消");
    return {};
  }

  void shutdown(std::function<void()> callback) {
    shutting_down = true;
    drained = std::move(callback);
    std::vector<Id> ids;
    ids.reserve(tasks.size());
    for (const auto &[id, task] : tasks)
      ids.push_back(id);
    for (const auto &id : ids)
      (void)cancel(id);
    if (tasks.empty() && drained) {
      auto done = std::move(drained);
      done();
    }
  }
};

NodeTestService::NodeTestService(uv_loop_t &loop, ProcessService &process,
                                 HttpClient &http,
                                 std::filesystem::path ping_binary)
    : impl_(std::make_shared<Impl>(loop, process, http,
                                   std::move(ping_binary))) {}

NodeTestService::~NodeTestService() {
  if (impl_)
    impl_->shutdown({});
}

Result<Id> NodeTestService::start(NodeTestRequest request,
                                  Completion<NodeTestResult> completion) {
  return impl_->start(std::move(request), std::move(completion));
}

Result<void> NodeTestService::cancel(const Id &id) { return impl_->cancel(id); }

void NodeTestService::shutdown(std::function<void()> drained) {
  impl_->shutdown(std::move(drained));
}

} // namespace xrm
