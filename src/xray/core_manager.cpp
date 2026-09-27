// XRM: Owns the Xray process and manages start, stop, and recovery.

#include "xray/core_manager.hpp"

#include "infra/store.hpp"
#include "xray/process_adapter.hpp"
#include "xray/runtime_assembler.hpp"

#include <uv.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <utility>

namespace xrm {
namespace {

AppError core_error(ErrorCode code, std::string message,
                    std::optional<std::string> subject = {}) {
  return AppError{code, std::move(message), std::move(subject), std::nullopt};
}

UnixMillis now_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Id new_run_id() {
  static std::atomic<std::uint64_t> sequence{1};
  return "run-" + std::to_string(now_millis()) + "-" +
         std::to_string(sequence.fetch_add(1));
}

ProcessSpec command_for(const RunSpec &run,
                        std::vector<std::string> arguments) {
  return ProcessSpec{run.binary, std::move(arguments), run.cwd,
                     run.environment};
}

} // namespace

ProcessSpec check_command(const RunSpec &run) {
  return command_for(run, {"run", "-test", "-confdir", run.confdir.string()});
}

ProcessSpec run_command(const RunSpec &run) {
  return command_for(run, {"run", "-confdir", run.confdir.string()});
}

ProcessSpec dump_command(const RunSpec &run) {
  return command_for(run, {"run", "-dump", "-confdir", run.confdir.string()});
}

struct CoreManager::Impl {
  enum class Phase {
    idle,
    preparing,
    checking,
    stopping_old,
    starting,
    observing,
    restoring,
    stopping,
    preview_check,
    stopping_candidate_restore,
    retry_wait
  };
  struct Observation {
    Impl *owner = nullptr;
    uv_timer_t timer{};
    ProcessId process = 0;
  };
  struct RetryTimer {
    Impl *owner = nullptr;
    uv_timer_t timer{};
  };
  struct Preparation {
    uv_work_t request{};
    Impl *owner = nullptr;
    BuildRequest build;
    std::optional<RunRecord> record;
    std::optional<AppError> error;
  };

  Impl(uv_loop_t &loop_value, Store &store_value, ProcessService &process_value,
       RuntimeWriter &writer_value, LifecycleOptions option_value)
      : loop(&loop_value), store(&store_value), process(&process_value),
        writer(&writer_value), options(option_value) {
    auto loaded = store->load_runtime();
    if (loaded)
      persistent = std::move(*loaded);
    if (persistent.active) {
      status.active = persistent.active;
      status.desired_running = persistent.desired_running;
      status.state = CoreState::failed;
      status.last_error =
          core_error(ErrorCode::result_unknown, "服务重启后尚未恢复核心进程",
                     persistent.active->run_id);
    }
  }

  static void close_observation(uv_handle_t *handle) {
    delete static_cast<Observation *>(handle->data);
  }
  static void on_observation(uv_timer_t *timer) {
    auto *observation = static_cast<Observation *>(timer->data);
    auto *owner = observation->owner;
    const auto process_id = observation->process;
    const auto found = owner->observation_timers.find(process_id);
    if (found != owner->observation_timers.end() &&
        found->second == observation)
      owner->observation_timers.erase(found);
    uv_timer_stop(timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(timer)))
      uv_close(reinterpret_cast<uv_handle_t *>(timer), close_observation);
    owner->observed(process_id);
  }

  Result<void> begin_observation(ProcessId process_id) {
    auto *observation = new Observation;
    observation->owner = this;
    observation->process = process_id;
    int status_code = uv_timer_init(loop, &observation->timer);
    if (status_code != 0) {
      delete observation;
      return std::unexpected(
          core_error(ErrorCode::io_error, "初始化核心观察计时器失败"));
    }
    observation->timer.data = observation;
    status_code =
        uv_timer_start(&observation->timer, on_observation,
                       static_cast<std::uint64_t>(std::max<std::int64_t>(
                           1, options.process_observation.count())),
                       0);
    if (status_code != 0) {
      uv_close(reinterpret_cast<uv_handle_t *>(&observation->timer),
               close_observation);
      return std::unexpected(
          core_error(ErrorCode::io_error, "启动核心观察计时器失败"));
    }
    observation_timers[process_id] = observation;
    return {};
  }

  void close_timer(ProcessId id) {
    const auto found = observation_timers.find(id);
    if (found == observation_timers.end())
      return;
    auto *observation = found->second;
    observation_timers.erase(found);
    uv_timer_stop(&observation->timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&observation->timer)))
      uv_close(reinterpret_cast<uv_handle_t *>(&observation->timer),
               close_observation);
  }

  void output(LogStream stream, std::string bytes) {
    if (output_callback && !bytes.empty())
      output_callback(stream, std::move(bytes));
  }

  void observe(LifecycleOptions::Phase value) {
    if (options.phase_observer)
      options.phase_observer(value);
  }

  void finish(Result<ActivationResult> result) {
    phase = Phase::idle;
    active_command.reset();
    auto callback = std::move(completion);
    if (callback)
      callback(std::move(result));
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
      if (output_closed_callback) {
        output_closed_callback(LogStream::core_stdout);
        output_closed_callback(LogStream::core_stderr);
      }
    }
    if (stop_requested) {
      if (completion)
        completion(std::unexpected(
            core_error(ErrorCode::cancelled, "连接请求被用户停止动作取消")));
      completion = {};
      discard_record(candidate_record);
      candidate_record.reset();
      stop_after_pending_check();
      return;
    }
    if (!result || result->exit.exit_code != 0 ||
        result->exit.term_signal != 0) {
      status.last_error =
          result ? core_error(ErrorCode::core_check_failed,
                              "Xray候选配置检查失败", result->stderr_text)
                 : result.error();
      if (activation_mode != ActivationMode::check_only) {
        if (status.active && active_process)
          status.state = CoreState::running;
        else
          status.state = CoreState::failed;
      }
      discard_record(candidate_record);
      finish(std::unexpected(*status.last_error));
      return;
    }
    if (activation_mode == ActivationMode::check_only) {
      discard_record(candidate_record);
      finish(ActivationResult{ActivationEffect::checked_only, status.active,
                              std::nullopt});
      return;
    }
    if (active_process) {
      phase = Phase::stopping_old;
      status.state = CoreState::starting;
      observe(LifecycleOptions::Phase::stopping_old);
      if (stop_requested)
        return;
      const auto id = *active_process;
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
    auto saved = store->save_runtime(persistent);
    if (!saved && !stop_intent_error)
      stop_intent_error = saved.error();
    status.last_error = stop_intent_error;
    auto activation_callback = std::move(completion);
    if (activation_callback)
      activation_callback(std::unexpected(
          core_error(ErrorCode::cancelled, "连接请求已被停止")));
    phase = Phase::idle;
    auto callback = std::move(stop_completion);
    auto error = std::move(stop_intent_error);
    stop_intent_error.reset();
    if (callback)
      callback(ActivationResult{ActivationEffect::stopped, std::nullopt,
                                std::move(error)});
  }

  static void prepare_work(uv_work_t *request) {
    auto *preparation = static_cast<Preparation *>(request->data);
    auto &build = preparation->build;
    const auto &profile = build.input.settings.profile;
    if (!build.assets.empty() &&
        (profile.environment.asset_dir ||
         profile.environment.process_environment.contains(
             "XRAY_LOCATION_ASSET"))) {
      preparation->error = core_error(
          ErrorCode::conflict, "不能用Geodata快照覆盖已有的Profile资源目录");
      return;
    }
    if (profile.environment.asset_dir &&
        profile.environment.process_environment.contains(
            "XRAY_LOCATION_ASSET") &&
        profile.environment.process_environment.at("XRAY_LOCATION_ASSET") !=
            profile.environment.asset_dir->string()) {
      preparation->error =
          core_error(ErrorCode::conflict,
                     "Profile资源目录与进程XRAY_LOCATION_ASSET设置冲突");
      return;
    }
    auto assembled = assemble(build.input);
    if (!assembled) {
      preparation->error = assembled.error();
      return;
    }
    const auto run_id = new_run_id();
    std::vector<RuntimeWriter::AssetSnapshot> assets;
    assets.reserve(build.assets.size());
    for (const auto &asset : build.assets)
      assets.push_back({asset.immutable_source, asset.runtime_name});
    auto directory = preparation->owner->writer->materialize(
        *assembled, DirectoryPurpose::candidate, run_id, assets);
    if (!directory) {
      preparation->error = directory.error();
      return;
    }
    const auto cwd = profile.environment.cwd.empty()
                         ? std::filesystem::current_path()
                         : profile.environment.cwd;
    auto environment = profile.environment.process_environment;
    if (!assets.empty())
      environment["XRAY_LOCATION_ASSET"] = directory->confdir.string();
    else if (profile.environment.asset_dir &&
             !environment.contains("XRAY_LOCATION_ASSET"))
      environment["XRAY_LOCATION_ASSET"] =
          profile.environment.asset_dir->string();
    preparation->record = RunRecord{run_id,
                                    profile.id,
                                    build.core.id,
                                    RunSpec{build.core.path, directory->confdir,
                                            cwd, std::move(environment)},
                                    profile.selected,
                                    assembled->index,
                                    now_millis()};
  }

  static void prepared(uv_work_t *request, int status_code) {
    std::unique_ptr<Preparation> preparation(
        static_cast<Preparation *>(request->data));
    auto *owner = preparation->owner;
    owner->preparing = false;
    if (status_code < 0 && !preparation->error)
      preparation->error =
          core_error(ErrorCode::io_error, "运行配置准备任务失败");
    if (preparation->error) {
      if (owner->stop_requested)
        owner->complete_stop();
      else
        owner->finish(std::unexpected(*preparation->error));
      return;
    }
    owner->candidate_record = std::move(preparation->record);
    if (owner->stop_requested) {
      owner->discard_record(owner->candidate_record);
      owner->candidate_record.reset();
      owner->complete_stop();
      return;
    }
    (void)owner->begin_check();
  }

  Result<void> prepare(BuildRequest build) {
    auto *preparation = new Preparation;
    preparation->owner = this;
    preparation->build = std::move(build);
    preparation->request.data = preparation;
    preparing = true;
    phase = Phase::preparing;
    const int queued =
        uv_queue_work(loop, &preparation->request, prepare_work, prepared);
    if (queued != 0) {
      preparing = false;
      phase = Phase::idle;
      delete preparation;
      return std::unexpected(core_error(ErrorCode::io_error,
                                        "提交运行配置准备任务失败: " +
                                            std::string(uv_strerror(queued))));
    }
    observe(LifecycleOptions::Phase::prepare);
    return {};
  }

  Result<Id> begin_check() {
    if (stop_requested) {
      discard_record(candidate_record);
      candidate_record.reset();
      complete_stop();
      return Id{};
    }
    phase = activation_mode == ActivationMode::check_only ? Phase::preview_check
                                                          : Phase::checking;
    if (activation_mode != ActivationMode::check_only)
      status.state = status.active ? CoreState::running : CoreState::starting;
    const auto command_id = process->execute(
        check_command(candidate_record->spec), options.check_timeout,
        [this](Result<ProcessResult> result) {
          check_ready(std::move(result));
        });
    if (!command_id) {
      phase = Phase::idle;
      auto error = command_id.error();
      auto done = std::move(completion);
      if (done)
        done(std::unexpected(std::move(error)));
      return Id{};
    }
    active_command = *command_id;
    observe(LifecycleOptions::Phase::checking);
    return *command_id;
  }

  void discard_record(const std::optional<RunRecord> &record) {
    if (!record || record->run_id.empty())
      return;
    PreparedDirectory directory{record->run_id, DirectoryPurpose::candidate,
                                record->spec.confdir, record->index};
    const std::array<Id, 3> protected_ids{
        status.active ? status.active->run_id : Id{},
        persistent.previous ? persistent.previous->run_id : Id{},
        previous_record ? previous_record->run_id : Id{}};
    (void)writer->discard(directory, protected_ids);
  }

  void start_candidate(bool restoring) {
    const auto run = restoring ? *previous_record : *candidate_record;
    phase = restoring ? Phase::restoring : Phase::starting;
    status.state = CoreState::starting;
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
    callbacks.output_closed = [this](ProcessId, LogStream stream) {
      if (output_closed_callback)
        output_closed_callback(stream);
    };
    auto spawned = process->spawn(run_command(run.spec), std::move(callbacks));
    if (!spawned) {
      candidate_failed(
          core_error(ErrorCode::core_start_failed,
                     "启动Xray核心失败: " + spawned.error().message));
      return;
    }
    if (restoring)
      restoration_process = *spawned;
    else
      candidate_process = *spawned;
    auto observation = begin_observation(*spawned);
    if (!observation) {
      (void)process->terminate(*spawned, options.stop_grace);
      candidate_failed(observation.error());
      return;
    }
    if (restoring)
      observe(LifecycleOptions::Phase::restoring_old);
    else
      observe(LifecycleOptions::Phase::starting_new);
  }

  void observed(ProcessId id) {
    if (id == candidate_process && phase == Phase::starting) {
      const bool was_retrying = retrying;
      const auto retired_active = previous_record;
      const auto retired_previous = persistent.previous;
      if (!retrying) {
        // The old input is held by previous_record until the candidate has
        // started and its RunRecord has committed. It is not a permanent
        // history record once that switch succeeds.
        persistent.previous.reset();
        persistent.active = candidate_record;
        persistent.desired_running = true;
        auto saved = store->save_runtime(persistent);
        if (!saved) {
          attempt_error = saved.error();
          status.last_error = saved.error();
          phase = Phase::stopping_candidate_restore;
          restore_after_candidate_stop = true;
          (void)process->terminate(id, options.stop_grace);
          return;
        }
      } else {
        persistent.active = candidate_record;
        persistent.desired_running = true;
        (void)store->save_runtime(persistent);
      }
      active_process = id;
      candidate_process.reset();
      status.active = candidate_record;
      status.state = CoreState::running;
      status.desired_running = true;
      status.health = RuntimeHealth::process_only;
      status.last_error.reset();
      retrying = false;
      if (!was_retrying)
        retry_attempt = 0;
      if (!was_retrying) {
        previous_record.reset();
        discard_record(retired_active);
        if (!retired_active || !retired_previous ||
            retired_active->run_id != retired_previous->run_id)
          discard_record(retired_previous);
      }
      if (completion)
        finish(ActivationResult{ActivationEffect::running_new, status.active,
                                std::nullopt});
      else
        phase = Phase::idle;
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
      persistent.previous.reset();
      persistent.desired_running = true;
      auto saved = store->save_runtime(persistent);
      if (!saved)
        status.last_error = saved.error();
      auto result = ActivationResult{ActivationEffect::restored_old,
                                     status.active, attempt_error};
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
    if (phase == Phase::stopping_candidate_restore && id == candidate_process) {
      candidate_process.reset();
      if (stop_requested) {
        complete_stop();
      } else if (restore_after_candidate_stop) {
        restore_after_candidate_stop = false;
        continue_candidate_failure();
      }
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
      if (retrying) {
        retrying = false;
        if (status.active)
          schedule_retry(*status.active);
        else
          phase = Phase::idle;
        return;
      }
      candidate_failed(core_error(
          ErrorCode::core_start_failed,
          "Xray核心在稳定运行前退出 (exit=" + std::to_string(exit.exit_code) +
              ", signal=" + std::to_string(exit.term_signal) + ")"));
      return;
    }
    if (restoring && id == restoration_process) {
      restoration_process.reset();
      status.active.reset();
      status.state = CoreState::failed;
      status.desired_running = false;
      persist();
      finish(std::unexpected(
          core_error(ErrorCode::core_start_failed, "恢复先前Xray核心失败")));
      return;
    }
    if (id == active_process) {
      active_process.reset();
      const auto crashed = status.active;
      status.state = CoreState::failed;
      status.desired_running = true;
      status.last_error =
          core_error(ErrorCode::core_start_failed, "Xray核心意外退出");
      if (crashed && phase == Phase::idle)
        schedule_retry(*crashed);
      else
        persist();
      if (phase != Phase::stopping)
        output(LogStream::server, "Xray core exited unexpectedly\n");
    }
  }

  void candidate_failed(AppError error) {
    attempt_error = std::move(error);
    if (candidate_process) {
      const auto id = *candidate_process;
      close_timer(id);
      phase = Phase::stopping_candidate_restore;
      restore_after_candidate_stop = true;
      (void)process->terminate(id, options.stop_grace);
      return;
    }
    continue_candidate_failure();
  }

  static void close_retry_handle(uv_handle_t *handle) {
    delete static_cast<RetryTimer *>(handle->data);
  }

  static void retry_timer_fired(uv_timer_t *timer) {
    auto *retry = static_cast<RetryTimer *>(timer->data);
    auto *owner = retry->owner;
    owner->retry_timer = nullptr;
    uv_timer_stop(timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(timer)))
      uv_close(reinterpret_cast<uv_handle_t *>(timer), close_retry_handle);
    if (owner->stop_requested || !owner->status.desired_running) {
      owner->complete_stop();
      return;
    }
    if (!owner->status.active) {
      owner->phase = Phase::idle;
      return;
    }
    owner->candidate_record = owner->status.active;
    owner->previous_record.reset();
    owner->retrying = true;
    owner->start_candidate(false);
  }

  void cancel_retry_timer() {
    if (!retry_timer)
      return;
    auto *retry = retry_timer;
    retry_timer = nullptr;
    uv_timer_stop(&retry->timer);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&retry->timer)))
      uv_close(reinterpret_cast<uv_handle_t *>(&retry->timer),
               close_retry_handle);
  }

  void schedule_retry(const RunRecord &record) {
    if (stop_requested || !status.desired_running)
      return;
    if (retry_attempt >= options.retry_delay.size()) {
      phase = Phase::idle;
      status.state = CoreState::failed;
      status.active = record;
      persistent.active = record;
      persistent.desired_running = true;
      auto saved = store->save_runtime(persistent);
      if (!saved)
        status.last_error = saved.error();
      output(LogStream::server, "Xray core exhausted automatic restarts\n");
      return;
    }
    auto *retry = new RetryTimer;
    retry->owner = this;
    const int initialized = uv_timer_init(loop, &retry->timer);
    if (initialized != 0) {
      delete retry;
      status.state = CoreState::failed;
      status.last_error =
          core_error(ErrorCode::io_error, "初始化核心恢复计时器失败");
      phase = Phase::idle;
      return;
    }
    retry->timer.data = retry;
    retry_timer = retry;
    status.active = record;
    persistent.active = record;
    persistent.desired_running = true;
    auto saved = store->save_runtime(persistent);
    if (!saved)
      status.last_error = saved.error();
    phase = Phase::retry_wait;
    const auto delay = options.retry_delay[retry_attempt++];
    const int started = uv_timer_start(
        &retry->timer, retry_timer_fired,
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, delay.count())),
        0);
    if (started != 0) {
      cancel_retry_timer();
      phase = Phase::idle;
      status.state = CoreState::failed;
      status.last_error =
          core_error(ErrorCode::io_error, "启动核心恢复计时器失败");
      return;
    }
    observe(LifecycleOptions::Phase::retry_wait);
  }

  void continue_candidate_failure() {
    if (candidate_record)
      discard_record(candidate_record);
    if (retrying) {
      retrying = false;
      if (status.active)
        schedule_retry(*status.active);
      else
        phase = Phase::idle;
      return;
    }
    if (stop_requested) {
      complete_stop();
      return;
    }
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
    auto saved = store->save_runtime(persistent);
    if (!saved)
      status.last_error = saved.error();
    finish(std::unexpected(*attempt_error));
  }

  Result<Id> activate(ActivationRequest request,
                      Completion<ActivationResult> callback) {
    if (phase != Phase::idle)
      return std::unexpected(
          core_error(ErrorCode::busy, "核心正在处理另一个动作"));
    completion = std::move(callback);
    activation_mode = request.mode;
    candidate_record.reset();
    previous_record = status.active;
    restore_attempted = false;
    stop_requested = false;
    stop_intent_error.reset();
    attempt_error.reset();
    retrying = false;
    restore_after_candidate_stop = false;

    if (auto *build = std::get_if<BuildRequest>(&request.input)) {
      auto queued = prepare(std::move(*build));
      if (!queued) {
        auto done = std::move(completion);
        if (done)
          done(std::unexpected(queued.error()));
        phase = Phase::idle;
      }
      return Id{};
    } else {
      auto replay = std::get<ReplayRequest>(std::move(request.input));
      replay.record.spec.binary = replay.core.path;
      replay.record.core_id = replay.core.id;
      candidate_record = std::move(replay.record);
    }
    return begin_check();
  }

  Result<Id> stop(Completion<ActivationResult> callback) {
    stop_requested = true;
    stop_completion = std::move(callback);
    status.desired_running = false;
    persistent.desired_running = false;
    auto saved = store->save_runtime(persistent);
    if (!saved && !stop_intent_error)
      stop_intent_error = saved.error();
    if (phase == Phase::idle) {
      if (active_process) {
        phase = Phase::stopping;
        status.state = CoreState::stopping;
        (void)process->terminate(*active_process, options.stop_grace);
      } else {
        complete_stop();
      }
      return Id{};
    }
    if (phase == Phase::retry_wait) {
      cancel_retry_timer();
      complete_stop();
      return Id{};
    }
    if (active_command) {
      (void)process->cancel_command(*active_command);
      return Id{};
    }
    if (candidate_process) {
      phase = Phase::stopping;
      (void)process->terminate(*candidate_process, options.stop_grace);
    } else if (restoration_process) {
      phase = Phase::stopping;
      (void)process->terminate(*restoration_process, options.stop_grace);
    } else if (active_process) {
      phase = Phase::stopping;
      status.state = CoreState::stopping;
      (void)process->terminate(*active_process, options.stop_grace);
    } else if (!preparing && !active_command) {
      complete_stop();
    }
    return Id{};
  }

  uv_loop_t *loop;
  Store *store;
  ProcessService *process;
  RuntimeWriter *writer;
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
  std::unordered_map<ProcessId, Observation *> observation_timers;
  Completion<ActivationResult> completion;
  Completion<ActivationResult> stop_completion;
  std::function<void(LogStream, std::string)> output_callback;
  std::function<void(LogStream)> output_closed_callback;
  ActivationMode activation_mode = ActivationMode::start_or_replace;
  bool restore_attempted = false;
  bool stop_requested = false;
  bool preparing = false;
  bool retrying = false;
  bool restore_after_candidate_stop = false;
  std::size_t retry_attempt = 0;
  RetryTimer *retry_timer = nullptr;
  std::optional<AppError> stop_intent_error;
};

CoreManager::CoreManager(uv_loop_t &loop, Store &store, ProcessService &process,
                         RuntimeWriter &writer, LifecycleOptions options)
    : impl_(std::make_unique<Impl>(loop, store, process, writer, options)) {}
CoreManager::~CoreManager() = default;
bool CoreManager::busy() const { return impl_->phase != Impl::Phase::idle; }
RuntimeStatus CoreManager::status() const {
  auto value = impl_->status;
  if (impl_->active_process)
    value.pid = impl_->process->pid(*impl_->active_process);
  return value;
}
void CoreManager::on_output(
    std::function<void(LogStream, std::string)> callback) {
  impl_->output_callback = std::move(callback);
}
void CoreManager::on_output_closed(std::function<void(LogStream)> callback) {
  impl_->output_closed_callback = std::move(callback);
}
Result<Id> CoreManager::activate(ActivationRequest request,
                                 Completion<ActivationResult> callback) {
  return impl_->activate(std::move(request), std::move(callback));
}
Result<Id> CoreManager::stop(Completion<ActivationResult> callback) {
  return impl_->stop(std::move(callback));
}
Result<Id>
CoreManager::restore_after_server_start(Completion<ActivationResult> callback) {
  auto loaded = impl_->store->load_runtime();
  if (!loaded)
    return std::unexpected(loaded.error());
  if (!loaded->desired_running || !loaded->active) {
    callback(ActivationResult{ActivationEffect::stopped, std::nullopt,
                              std::nullopt});
    return Id{};
  }
  auto core = impl_->store->get_core(loaded->active->core_id);
  if (!core)
    return std::unexpected(core.error());
  return activate(
      ActivationRequest{ActivationInput{ReplayRequest{*loaded->active, *core}},
                        ActivationMode::preserve_running_state},
      std::move(callback));
}
void CoreManager::shutdown(std::function<void()> drained) {
  if (impl_->active_process || impl_->phase != Impl::Phase::idle) {
    // stop() accepts the request and invokes its completion after the owned
    // process drains; it has no rejected-stop callback path.
    (void)stop(
        [callback = std::move(drained)](Result<ActivationResult>) mutable {
          if (callback)
            callback();
        });
  } else {
    if (drained)
      drained();
  }
}

} // namespace xrm
