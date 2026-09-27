// XRM: Handles broken stdout pipes without hiding unrelated write errors.

#pragma once

#include <cerrno>
#include <csignal>
#include <pthread.h>

namespace xrm {

// libuv's Unix pipe writes and iostream writes can raise SIGPIPE before their
// normal EPIPE result is inspected. Defer that signal only in this thread and
// only during the write boundary, then consume the new pending signal. The
// caller still must handle its actual I/O result; no process-wide ignore.
class PipeWriteScope {
public:
  PipeWriteScope() {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    masked_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    sigset_t pending{};
    sigpending(&pending);
    already_pending_ = sigismember(&pending, SIGPIPE) == 1;
  }
  ~PipeWriteScope() {
    if (!masked_)
      return;
    if (!already_pending_) {
      timespec immediate{};
      while (sigtimedwait(&blocked_, nullptr, &immediate) < 0 &&
             errno == EINTR) {
      }
    }
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }
  PipeWriteScope(const PipeWriteScope &) = delete;
  PipeWriteScope &operator=(const PipeWriteScope &) = delete;

private:
  sigset_t blocked_{}, previous_{};
  bool masked_ = false;
  bool already_pending_ = false;
};

} // namespace xrm
