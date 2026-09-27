#include "infra/process.hpp"

#include <uv.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xrm {
namespace {

constexpr std::size_t kCaptureLimit = 1024 * 1024;
constexpr std::uint64_t kKillGraceMs = 300;

AppError uv_error(int code, std::string operation) {
  operation += ": ";
  operation += uv_strerror(code);
  return {ErrorCode::io_error, std::move(operation), std::nullopt, std::nullopt};
}

void append_bounded(std::string& destination, const char* data, std::size_t size,
                    bool& truncated) {
  const auto remaining = destination.size() < kCaptureLimit
                             ? kCaptureLimit - destination.size()
                             : 0;
  const auto copied = std::min(remaining, size);
  destination.append(data, copied);
  if (copied != size) truncated = true;
}

}  // namespace

struct ProcessService::Impl {
  struct Child {
    Impl* owner = nullptr;
    ProcessId id = 0;
    std::optional<Id> command_id;
    uv_process_t process{};
    uv_pipe_t stdout_pipe{};
    uv_pipe_t stderr_pipe{};
    uv_timer_t deadline_timer{};
    uv_timer_t kill_timer{};
    int open_handles = 0;
    bool process_open = false;
    bool stdout_open = false;
    bool stderr_open = false;
    bool deadline_open = false;
    bool kill_timer_open = false;
    bool exit_seen = false;
    bool timed_out = false;
    bool cancelled = false;
    ProcessResult result;
    ProcessCallbacks callbacks;
    Completion<ProcessResult> completion;
  };

  explicit Impl(uv_loop_t& event_loop) : loop(&event_loop) {}

  static void allocate(uv_handle_t*, std::size_t suggested, uv_buf_t* buffer) {
    const auto size = std::clamp<std::size_t>(suggested, 4096, 65536);
    buffer->base = static_cast<char*>(std::malloc(size));
    buffer->len = buffer->base ? size : 0;
  }

  static void read_stdout(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
    read_stream(stream, count, buffer, true);
  }
  static void read_stderr(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
    read_stream(stream, count, buffer, false);
  }

  static void read_stream(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer,
                          bool stdout_stream) {
    auto* child = static_cast<Child*>(stream->data);
    if (count > 0) {
      const auto size = static_cast<std::size_t>(count);
      if (stdout_stream) {
        append_bounded(child->result.stdout_text, buffer->base, size,
                       child->result.output_truncated);
        if (child->callbacks.stdout_chunk) {
          child->callbacks.stdout_chunk(child->id, std::string(buffer->base, size));
        }
      } else {
        append_bounded(child->result.stderr_text, buffer->base, size,
                       child->result.output_truncated);
        if (child->callbacks.stderr_chunk) {
          child->callbacks.stderr_chunk(child->id, std::string(buffer->base, size));
        }
      }
    }
    std::free(buffer->base);
    if (count < 0) {
      uv_read_stop(stream);
      auto* handle = reinterpret_cast<uv_handle_t*>(stream);
      if (!uv_is_closing(handle)) {
        if (child->callbacks.output_closed)
          child->callbacks.output_closed(child->id, stdout_stream
              ? LogStream::core_stdout : LogStream::core_stderr);
        uv_close(handle, on_handle_closed);
      }
    }
  }

  static void on_handle_closed(uv_handle_t* handle) {
    auto* child = static_cast<Child*>(handle->data);
    --child->open_handles;
    child->owner->maybe_finish(child);
  }

  static void close_handle(uv_handle_t* handle, bool& opened) {
    if (!opened) return;
    opened = false;
    if (!uv_is_closing(handle)) uv_close(handle, on_handle_closed);
  }

  static void on_exit(uv_process_t* process, std::int64_t exit_status,
                      int term_signal) {
    auto* child = static_cast<Child*>(process->data);
    child->exit_seen = true;
    child->result.exit = {static_cast<int>(exit_status), term_signal};
    if (child->callbacks.exited) child->callbacks.exited(child->id, child->result.exit);
    close_handle(reinterpret_cast<uv_handle_t*>(&child->process), child->process_open);
    close_handle(reinterpret_cast<uv_handle_t*>(&child->deadline_timer), child->deadline_open);
    close_handle(reinterpret_cast<uv_handle_t*>(&child->kill_timer), child->kill_timer_open);
  }

  static void on_deadline(uv_timer_t* timer) {
    auto* child = static_cast<Child*>(timer->data);
    child->timed_out = true;
    send_term(child);
  }

  static void on_kill_grace(uv_timer_t* timer) {
    auto* child = static_cast<Child*>(timer->data);
    if (!child->exit_seen) uv_process_kill(&child->process, SIGKILL);
  }

  static void send_term(Child* child) {
    if (child->exit_seen) return;
    const int result = uv_process_kill(&child->process, SIGTERM);
    if (result == UV_ESRCH) return;
    if (child->kill_timer_open) {
      uv_timer_start(&child->kill_timer, on_kill_grace, kKillGraceMs, 0);
    }
  }

  static void close_unspawned(Child* child) {
    child->exit_seen = true;
    close_handle(reinterpret_cast<uv_handle_t*>(&child->stdout_pipe), child->stdout_open);
    close_handle(reinterpret_cast<uv_handle_t*>(&child->stderr_pipe), child->stderr_open);
    close_handle(reinterpret_cast<uv_handle_t*>(&child->deadline_timer), child->deadline_open);
    close_handle(reinterpret_cast<uv_handle_t*>(&child->kill_timer), child->kill_timer_open);
  }

  void maybe_finish(Child* child) {
    if (!child->exit_seen || child->open_handles != 0) return;
    const ProcessId id = child->id;
    const auto command_id = child->command_id;
    auto completion = std::move(child->completion);
    auto result = std::move(child->result);
    const bool timed_out = child->timed_out;
    const bool cancelled = child->cancelled;
    children.erase(id);
    if (command_id) commands.erase(*command_id);
    if (completion) {
      if (timed_out) {
        completion(std::unexpected(AppError{ErrorCode::timed_out,
                                             "进程执行超过时限并已回收",
                                             command_id, std::nullopt}));
      } else if (cancelled) {
        completion(std::unexpected(AppError{ErrorCode::cancelled,
                                             "进程命令已取消并回收",
                                             command_id, std::nullopt}));
      } else {
        completion(std::move(result));
      }
    }
    notify_drained();
  }

  void notify_drained() {
    if (!shutting_down || !children.empty() || !drained) return;
    auto callback = std::move(drained);
    callback();
  }

  uv_loop_t* loop;
  ProcessId next_process = 1;
  std::uint64_t next_command = 1;
  std::unordered_map<ProcessId, std::unique_ptr<Child>> children;
  std::unordered_map<Id, ProcessId> commands;
  bool shutting_down = false;
  std::function<void()> drained;
};

ProcessService::ProcessService(uv_loop_t& loop) : impl_(std::make_unique<Impl>(loop)) {}
ProcessService::~ProcessService() = default;

Result<ProcessId> ProcessService::spawn(ProcessSpec spec, ProcessCallbacks callbacks) {
  if (impl_->shutting_down) {
    return std::unexpected(AppError{ErrorCode::busy, "进程服务正在关闭", std::nullopt, std::nullopt});
  }
  if (spec.executable.empty()) {
    return std::unexpected(AppError{ErrorCode::invalid_argument, "可执行文件路径不能为空", "executable", std::nullopt});
  }
  auto child = std::make_unique<Impl::Child>();
  child->owner = impl_.get();
  child->id = impl_->next_process++;
  child->callbacks = std::move(callbacks);
  const ProcessId id = child->id;
  auto* raw = child.get();
  impl_->children.emplace(id, std::move(child));

  auto initialize_pipe = [&](uv_pipe_t& pipe, bool& opened) -> int {
    const int status = uv_pipe_init(impl_->loop, &pipe, 0);
    if (status == 0) {
      opened = true;
      ++raw->open_handles;
      pipe.data = raw;
    }
    return status;
  };
  auto initialize_timer = [&](uv_timer_t& timer, bool& opened) -> int {
    const int status = uv_timer_init(impl_->loop, &timer);
    if (status == 0) {
      opened = true;
      ++raw->open_handles;
      timer.data = raw;
    }
    return status;
  };
  int status = initialize_pipe(raw->stdout_pipe, raw->stdout_open);
  if (status == 0) status = initialize_pipe(raw->stderr_pipe, raw->stderr_open);
  if (status == 0) status = initialize_timer(raw->deadline_timer, raw->deadline_open);
  if (status == 0) status = initialize_timer(raw->kill_timer, raw->kill_timer_open);
  if (status != 0) {
    Impl::close_unspawned(raw);
    return std::unexpected(uv_error(status, "初始化进程句柄失败"));
  }

  std::vector<std::string> argument_storage;
  argument_storage.reserve(spec.arguments.size() + 1);
  argument_storage.push_back(spec.executable.string());
  argument_storage.insert(argument_storage.end(), spec.arguments.begin(), spec.arguments.end());
  std::vector<char*> arguments;
  arguments.reserve(argument_storage.size() + 1);
  for (auto& argument : argument_storage) arguments.push_back(argument.data());
  arguments.push_back(nullptr);

  std::vector<std::string> environment_storage;
  environment_storage.reserve(spec.environment.size());
  for (const auto& [key, value] : spec.environment) environment_storage.push_back(key + "=" + value);
  std::vector<char*> environment;
  environment.reserve(environment_storage.size() + 1);
  for (auto& entry : environment_storage) environment.push_back(entry.data());
  environment.push_back(nullptr);

  uv_stdio_container_t stdio[3]{};
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&raw->stdout_pipe);
  stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&raw->stderr_pipe);
  const auto cwd = spec.cwd.empty() ? std::string{} : spec.cwd.string();
  uv_process_options_t options{};
  options.exit_cb = Impl::on_exit;
  options.file = argument_storage.front().c_str();
  options.args = arguments.data();
  options.env = environment.data();
  options.cwd = cwd.empty() ? nullptr : cwd.c_str();
  options.stdio_count = 3;
  options.stdio = stdio;
  status = uv_spawn(impl_->loop, &raw->process, &options);
  if (status != 0) {
    Impl::close_unspawned(raw);
    return std::unexpected(uv_error(status, "启动子进程失败"));
  }
  raw->process_open = true;
  ++raw->open_handles;
  raw->process.data = raw;
  status = uv_read_start(reinterpret_cast<uv_stream_t*>(&raw->stdout_pipe), Impl::allocate,
                         Impl::read_stdout);
  if (status == 0) {
    status = uv_read_start(reinterpret_cast<uv_stream_t*>(&raw->stderr_pipe), Impl::allocate,
                           Impl::read_stderr);
  }
  if (status != 0) Impl::send_term(raw);
  return id;
}

Result<Id> ProcessService::execute(ProcessSpec spec, Milliseconds timeout,
                                   Completion<ProcessResult> completion) {
  if (timeout.count() <= 0) {
    return std::unexpected(AppError{ErrorCode::invalid_argument, "进程超时必须大于零", "timeout", std::nullopt});
  }
  const Id command_id = "command-" + std::to_string(impl_->next_command++);
  auto process_id = spawn(std::move(spec), {});
  if (!process_id) return std::unexpected(process_id.error());
  auto& child = *impl_->children.at(*process_id);
  child.command_id = command_id;
  child.completion = std::move(completion);
  impl_->commands.emplace(command_id, *process_id);
  const int status = uv_timer_start(&child.deadline_timer, Impl::on_deadline,
                                    static_cast<std::uint64_t>(timeout.count()), 0);
  if (status != 0) {
    child.cancelled = true;
    Impl::send_term(&child);
    return command_id;
  }
  return command_id;
}

Result<void> ProcessService::terminate(ProcessId process, Milliseconds grace) {
  const auto it = impl_->children.find(process);
  if (it == impl_->children.end() || it->second->command_id) {
    return std::unexpected(AppError{ErrorCode::not_found, "没有正在运行的受管进程",
                                    std::to_string(process), std::nullopt});
  }
  auto& child = *it->second;
  if (child.exit_seen) return {};
  const int status = uv_process_kill(&child.process, SIGTERM);
  if (status != 0 && status != UV_ESRCH) return std::unexpected(uv_error(status, "发送SIGTERM失败"));
  if (grace.count() > 0) {
    const int timer_status = uv_timer_start(&child.kill_timer, Impl::on_kill_grace,
                                            static_cast<std::uint64_t>(grace.count()), 0);
    if (timer_status != 0) return std::unexpected(uv_error(timer_status, "设置强制终止时限失败"));
  }
  return {};
}

Result<void> ProcessService::cancel_command(const Id& command_id) {
  const auto found = impl_->commands.find(command_id);
  if (found == impl_->commands.end()) {
    return std::unexpected(AppError{ErrorCode::not_found, "进程命令不存在", command_id, std::nullopt});
  }
  auto& child = *impl_->children.at(found->second);
  child.cancelled = true;
  Impl::send_term(&child);
  return {};
}

std::optional<int> ProcessService::pid(ProcessId process) const {
  const auto it = impl_->children.find(process);
  if (it == impl_->children.end() || it->second->exit_seen || it->second->process.pid == 0) {
    return std::nullopt;
  }
  return static_cast<int>(it->second->process.pid);
}

void ProcessService::shutdown(std::function<void()> drained) {
  if (impl_->shutting_down) return;
  impl_->shutting_down = true;
  impl_->drained = std::move(drained);
  for (auto& [_, child] : impl_->children) {
    if (!child->exit_seen) {
      Impl::send_term(child.get());
      if (child->kill_timer_open) {
        uv_timer_start(&child->kill_timer, Impl::on_kill_grace, kKillGraceMs, 0);
      }
    }
  }
  impl_->notify_drained();
}

}  // namespace xrm
