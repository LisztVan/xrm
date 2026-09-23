#include "xray/core_manager.hpp"

#include "infra/store.hpp"
#include "xray/process_adapter.hpp"
#include "xray/runtime_assembler.hpp"

#include <uv.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <utility>

namespace xrm {
namespace {

AppError core_error(ErrorCode code, std::string message, std::optional<std::string> subject = {}) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

UnixMillis now_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

Id new_run_id() {
  static std::atomic<std::uint64_t> sequence{1};
  return "run-" + std::to_string(now_millis()) + "-" + std::to_string(sequence.fetch_add(1));
}

ProcessSpec command_for(const RunSpec& run, std::vector<std::string> arguments) {
  return ProcessSpec{run.binary, std::move(arguments), run.cwd, run.environment};
}

}  // namespace

ProcessSpec check_command(const RunSpec& run) {
  return command_for(run, {"run", "-test", "-confdir", run.confdir.string()});
}

ProcessSpec run_command(const RunSpec& run) {
  return command_for(run, {"run", "-confdir", run.confdir.string()});
}

ProcessSpec dump_command(const RunSpec& run) {
  return command_for(run, {"run", "-dump", "-confdir", run.confdir.string()});
}

struct CoreManager::Impl {
  enum class Phase { idle, checking, stopping_old, starting, observing, restoring, stopping, preview_check };
  struct Observation {
    Impl* owner = nullptr;
    uv_timer_t timer{};
    ProcessId process = 0;
  };

  Impl(uv_loop_t& loop_value, Store& store_value, ProcessService& process_value,
       RuntimeWriter& writer_value, LifecycleOptions option_value)
      : loop(&loop_value), store(&store_value), process(&process_value), writer(&writer_value), options(option_value) {
    auto loaded = store->load_runtime();
    if (loaded) persistent = std::move(*loaded);
    if (persistent.active) {
      status.active = persistent.active;
      status.desired_running = persistent.desired_running;
      status.state = CoreState::failed;
      status.last_error = core_error(ErrorCode::result_unknown,
          "服务重启后尚未恢复核心进程", persistent.active->run_id);
    }
  }

  static void close_observation(uv_handle_t* handle) {
    delete static_cast<Observation*>(handle->data);
  }
  static void on_observation(uv_timer_t* timer) {
    auto* observation = static_cast<Observation*>(timer->data);
    auto* owner = observation->owner;
    const auto process_id = observation->process;
    const auto found = owner->observation_timers.find(process_id);
    if (found != owner->observation_timers.end() && found->second == observation)
      owner->observation_timers.erase(found);
    uv_timer_stop(timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(timer)))
      uv_close(reinterpret_cast<uv_handle_t*>(timer), close_observation);
    owner->observed(process_id);
  }

  Result<void> begin_observation(ProcessId process_id) {
    auto* observation = new Observation;
    observation->owner = this;
    observation->process = process_id;
    int status_code = uv_timer_init(loop, &observation->timer);
    if (status_code != 0) {
      delete observation;
      return std::unexpected(core_error(ErrorCode::io_error, "初始化核心观察计时器失败"));
    }
    observation->timer.data = observation;
    status_code = uv_timer_start(&observation->timer, on_observation,
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, options.process_observation.count())), 0);
    if (status_code != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(&observation->timer), close_observation);
      return std::unexpected(core_error(ErrorCode::io_error, "启动核心观察计时器失败"));
    }
    observation_timers[process_id] = observation;
    return {};
  }

  void close_timer(ProcessId id) {
    const auto found = observation_timers.find(id);
    if (found == observation_timers.end()) return;
    auto* observation = found->second;
    observation_timers.erase(found);
    uv_timer_stop(&observation->timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&observation->timer)))
      uv_close(reinterpret_cast<uv_handle_t*>(&observation->timer), close_observation);
  }

  void output(LogStream stream, std::string bytes) {
    if (output_callback && !bytes.empty()) output_callback(stream, std::move(bytes));
  }

  void finish(Result<ActivationResult> result) {
    phase = Phase::idle;
    active_command.reset();
    auto callback = std::move(completion);
    if (callback) callback(std::move(result));
  }

  void persist() {
    persistent.desired_running = status.desired_running;
    persistent.active = status.active;
    (void)store->save_runtime(persistent);
  }

  void check_ready(Result<ProcessResult> result) {
    active_command.reset();
    if (result) {
      output(LogStream::core_stdout, result->stdout_text);
      output(LogStream::core_stderr, result->stderr_text);
    }
    if (stop_requested) {
      if (completion) completion(std::unexpected(core_error(ErrorCode::cancelled, "连接请求被用户停止动作取消")));
      completion = {};
      stop_after_pending_check();
      return;
    }
    if (!result || result->exit.exit_code != 0 || result->exit.term_signal != 0) {
      status.last_error = result ? core_error(ErrorCode::core_check_failed,
          "Xray候选配置检查失败", result->stderr_text) : result.error();
      if (activation_mode != ActivationMode::check_only) {
        if (status.active && active_process) status.state = CoreState::running;
        else status.state = CoreState::failed;
      }
      if (activation_mode == ActivationMode::check_only) {
        discard_record(candidate_record);
        finish(std::unexpected(*status.last_error));
      } else {
        discard_record(candidate_record);
        finish(std::unexpected(*status.last_error));
      }
      return;
    }
    if (activation_mode == ActivationMode::check_only) {
      discard_record(candidate_record);
      finish(ActivationResult{ActivationEffect::checked_only, status.active, std::nullopt});
      return;
    }
    if (active_process) {
      phase = Phase::stopping_old;
      const auto id = *active_process;
      status.state = CoreState::starting;
      auto terminated = process->terminate(id, options.stop_grace);
      if (!terminated) {
        status.state = CoreState::running;
        finish(std::unexpected(terminated.error()));
      }
      return;
    }
    start_candidate(false);
  }

  void stop_after_pending_check() {
    phase = Phase::stopping;
    if (active_process) {
      status.state = CoreState::stopping;
      (void)process->terminate(*active_process, options.stop_grace);
    } else {
      complete_stop();
    }
  }

  void complete_stop() {
    status.state = CoreState::stopped;
    status.desired_running = false;
    status.active.reset();
    active_process.reset();
    persistent.active.reset();
    persistent.desired_running = false;
    (void)store->save_runtime(persistent);
    auto activation_callback = std::move(completion);
    if (activation_callback)
      activation_callback(std::unexpected(core_error(ErrorCode::cancelled, "连接请求已被停止")));
    phase = Phase::idle;
    auto callback = std::move(stop_completion);
    if (callback) callback(ActivationResult{ActivationEffect::stopped, std::nullopt, std::nullopt});
  }

  void discard_record(const std::optional<RunRecord>& record) {
    if (!record || record->run_id.empty()) return;
    PreparedDirectory directory{record->run_id, DirectoryPurpose::candidate,
                                record->spec.confdir, record->index};
    const std::array<Id, 2> protected_ids{
        status.active ? status.active->run_id : Id{},
        persistent.previous ? persistent.previous->run_id : Id{}};
    (void)writer->discard(directory, protected_ids);
  }

  void start_candidate(bool restoring) {
    const auto run = restoring ? *previous_record : *candidate_record;
    phase = restoring ? Phase::restoring : Phase::starting;
    if (!restoring) status.state = CoreState::starting;
    else status.state = CoreState::starting;
    ProcessCallbacks callbacks;
    callbacks.stdout_chunk = [this](ProcessId, std::string bytes) {
      output(LogStream::core_stdout, std::move(bytes));
    };
    callbacks.stderr_chunk = [this](ProcessId, std::string bytes) {
      output(LogStream::core_stderr, std::move(bytes));
    };
    callbacks.exited = [this, restoring](ProcessId id, ProcessExit exit) {
      process_exited(id, exit, restoring);
    };
    auto spawned = process->spawn(run_command(run.spec), std::move(callbacks));
    if (!spawned) {
      candidate_failed(core_error(ErrorCode::core_start_failed,
                                  "启动Xray核心失败: " + spawned.error().message));
      return;
    }
    if (restoring) restoration_process = *spawned;
    else candidate_process = *spawned;
    auto observation = begin_observation(*spawned);
    if (!observation) {
      (void)process->terminate(*spawned, options.stop_grace);
      candidate_failed(observation.error());
    }
  }

  void observed(ProcessId id) {
    if (id == candidate_process && phase == Phase::starting) {
      active_process = id;
      candidate_process.reset();
      status.active = candidate_record;
      status.state = CoreState::running;
      status.desired_running = true;
      status.health = RuntimeHealth::process_only;
      status.last_error.reset();
      persistent.previous = previous_record;
      persistent.active = status.active;
      persistent.desired_running = true;
      auto saved = store->save_runtime(persistent);
      if (!saved) status.last_error = saved.error();
      finish(ActivationResult{ActivationEffect::running_new, status.active,
                              status.last_error});
      return;
    }
    if (id == restoration_process && phase == Phase::restoring) {
      active_process = id;
      restoration_process.reset();
      status.active = previous_record;
      status.state = CoreState::running;
      status.desired_running = true;
      status.health = RuntimeHealth::process_only;
      persistent.active = status.active;
      persistent.desired_running = true;
      (void)store->save_runtime(persistent);
      auto result = ActivationResult{ActivationEffect::restored_old, status.active, attempt_error};
      finish(result);
      return;
    }
  }

  void process_exited(ProcessId id, ProcessExit exit, bool restoring) {
    close_timer(id);
    if (phase == Phase::stopping && id == candidate_process) {
      candidate_process.reset();
      complete_stop();
      return;
    }
    if (phase == Phase::stopping && id == restoration_process) {
      restoration_process.reset();
      complete_stop();
      return;
    }
    if (phase == Phase::stopping && id == active_process) {
      active_process.reset();
      complete_stop();
      return;
    }
    if (phase == Phase::stopping_old && id == active_process) {
      active_process.reset();
      status.active.reset();
      start_candidate(false);
      return;
    }
    if (!restoring && id == candidate_process) {
      candidate_process.reset();
      candidate_failed(core_error(ErrorCode::core_start_failed,
          "Xray核心在稳定运行前退出 (exit=" + std::to_string(exit.exit_code) + ", signal=" +
          std::to_string(exit.term_signal) + ")"));
      return;
    }
    if (restoring && id == restoration_process) {
      restoration_process.reset();
      status.active.reset();
      status.state = CoreState::failed;
      status.desired_running = false;
      persist();
      finish(std::unexpected(core_error(ErrorCode::core_start_failed,
                                        "恢复先前Xray核心失败")));
      return;
    }
    if (id == active_process) {
      active_process.reset();
      status.active.reset();
      status.state = CoreState::failed;
      status.desired_running = false;
      status.last_error = core_error(ErrorCode::core_start_failed, "Xray核心意外退出");
      persist();
      if (phase == Phase::idle) output(LogStream::server, "Xray core exited unexpectedly\n");
    }
  }

  void candidate_failed(AppError error) {
    attempt_error = std::move(error);
    if (candidate_process) {
      const auto id = *candidate_process;
      candidate_process.reset();
      close_timer(id);
      (void)process->terminate(id, options.stop_grace);
    }
    if (candidate_record) discard_record(candidate_record);
    if (previous_record && !restore_attempted) {
      restore_attempted = true;
      start_candidate(true);
      return;
    }
    status.state = CoreState::failed;
    status.active.reset();
    status.desired_running = false;
    status.last_error = attempt_error;
    persistent.active.reset();
    persistent.desired_running = false;
    (void)store->save_runtime(persistent);
    finish(std::unexpected(*attempt_error));
  }

  Result<Id> activate(ActivationRequest request, Completion<ActivationResult> callback) {
    if (phase != Phase::idle) return std::unexpected(core_error(ErrorCode::busy, "核心正在处理另一个动作"));
    completion = std::move(callback);
    activation_mode = request.mode;
    candidate_record.reset();
    previous_record = status.active;
    restore_attempted = false;
    stop_requested = false;
    attempt_error.reset();

    if (auto* build = std::get_if<BuildRequest>(&request.input)) {
      auto assembled = assemble(build->input);
      if (!assembled) { completion(std::unexpected(assembled.error())); completion = {}; return Id{}; }
      const auto run_id = new_run_id();
      auto directory = writer->materialize(*assembled, DirectoryPurpose::candidate, run_id);
      if (!directory) { completion(std::unexpected(directory.error())); completion = {}; return Id{}; }
      const auto& profile = build->input.settings.profile;
      const auto cwd = profile.environment.cwd.empty() ? std::filesystem::current_path() : profile.environment.cwd;
      RunRecord record{run_id, profile.id, build->core.id,
          RunSpec{build->core.path, directory->confdir, cwd, profile.environment.process_environment},
          profile.selected, assembled->index, now_millis()};
      candidate_record = std::move(record);
    } else {
      auto replay = std::get<ReplayRequest>(std::move(request.input));
      replay.record.spec.binary = replay.core.path;
      candidate_record = std::move(replay.record);
    }
    phase = request.mode == ActivationMode::check_only ? Phase::preview_check : Phase::checking;
    if (request.mode != ActivationMode::check_only)
      status.state = status.active ? CoreState::running : CoreState::starting;
    const auto command_id = process->execute(check_command(candidate_record->spec), options.check_timeout,
        [this](Result<ProcessResult> result) { check_ready(std::move(result)); });
    if (!command_id) {
      phase = Phase::idle;
      auto error = command_id.error();
      auto done = std::move(completion);
      if (done) done(std::unexpected(std::move(error)));
      return Id{};
    }
    active_command = *command_id;
    return *command_id;
  }

  Result<Id> stop(Completion<ActivationResult> callback) {
    if (phase == Phase::idle) {
      stop_completion = std::move(callback);
      if (active_process) {
        phase = Phase::stopping;
        status.state = CoreState::stopping;
        auto stopped = process->terminate(*active_process, options.stop_grace);
        if (!stopped) return std::unexpected(stopped.error());
      } else {
        complete_stop();
      }
      return Id{};
    }
    stop_requested = true;
    stop_completion = std::move(callback);
    if (active_command) (void)process->cancel_command(*active_command);
    if (candidate_process) {
      phase = Phase::stopping;
      (void)process->terminate(*candidate_process, options.stop_grace);
    } else if (active_process) {
      phase = Phase::stopping;
      status.state = CoreState::stopping;
      (void)process->terminate(*active_process, options.stop_grace);
    }
    return Id{};
  }

  uv_loop_t* loop;
  Store* store;
  ProcessService* process;
  RuntimeWriter* writer;
  LifecycleOptions options;
  Phase phase = Phase::idle;
  RuntimePersistent persistent;
  RuntimeStatus status;
  std::optional<ProcessId> active_process;
  std::optional<ProcessId> candidate_process;
  std::optional<ProcessId> restoration_process;
  std::optional<Id> active_command;
  std::optional<RunRecord> candidate_record;
  std::optional<RunRecord> previous_record;
  std::optional<AppError> attempt_error;
  std::unordered_map<ProcessId, Observation*> observation_timers;
  Completion<ActivationResult> completion;
  Completion<ActivationResult> stop_completion;
  std::function<void(LogStream, std::string)> output_callback;
  ActivationMode activation_mode = ActivationMode::start_or_replace;
  bool restore_attempted = false;
  bool stop_requested = false;
};

CoreManager::CoreManager(uv_loop_t& loop, Store& store, ProcessService& process,
                         RuntimeWriter& writer, LifecycleOptions options)
    : impl_(std::make_unique<Impl>(loop, store, process, writer, options)) {}
CoreManager::~CoreManager() = default;
bool CoreManager::busy() const { return impl_->phase != Impl::Phase::idle; }
RuntimeStatus CoreManager::status() const {
  auto value = impl_->status;
  if (impl_->active_process) value.pid = impl_->process->pid(*impl_->active_process);
  return value;
}
void CoreManager::on_output(std::function<void(LogStream, std::string)> callback) {
  impl_->output_callback = std::move(callback);
}
Result<Id> CoreManager::activate(ActivationRequest request, Completion<ActivationResult> callback) {
  return impl_->activate(std::move(request), std::move(callback));
}
Result<Id> CoreManager::stop(Completion<ActivationResult> callback) {
  return impl_->stop(std::move(callback));
}
Result<Id> CoreManager::restore_after_server_start(Completion<ActivationResult> callback) {
  auto loaded = impl_->store->load_runtime();
  if (!loaded) return std::unexpected(loaded.error());
  if (!loaded->desired_running || !loaded->active) {
    callback(ActivationResult{ActivationEffect::stopped, std::nullopt, std::nullopt});
    return Id{};
  }
  auto core = impl_->store->get_core(loaded->active->core_id);
  if (!core) return std::unexpected(core.error());
  return activate(ActivationRequest{ActivationInput{ReplayRequest{*loaded->active, *core}},
                                    ActivationMode::preserve_running_state}, std::move(callback));
}
void CoreManager::shutdown(std::function<void()> drained) {
  if (impl_->active_process || impl_->phase != Impl::Phase::idle) {
    auto result = stop([callback = std::move(drained)](Result<ActivationResult>) mutable {
      if (callback) callback();
    });
    if (!result && drained) drained();
  } else {
    if (drained) drained();
  }
}

}  // namespace xrm
