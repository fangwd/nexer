#include "nexer/runtime/process_manager.h"

#include <algorithm>
#include <signal.h>
#include <sstream>

#include "uv.h"

namespace nexer {

static constexpr uint64_t kCheckIntervalMs = 100;
static constexpr uint64_t kGroupPollMs = 50;
static constexpr uint64_t kAfterKillWaitMs = 5000;

const char *to_string(DesiredState state) {
    return state == DesiredState::Running ? "running" : "stopped";
}

const char *to_string(ObservedState state) {
    switch (state) {
    case ObservedState::Starting:
        return "starting";
    case ObservedState::Ready:
        return "ready";
    case ObservedState::Backoff:
        return "backoff";
    case ObservedState::Stopping:
        return "stopping";
    case ObservedState::Stopped:
        return "stopped";
    case ObservedState::Completed:
        return "completed";
    case ObservedState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string error_string(int error) {
    if (error == 0) {
        return "ok";
    }
    if (error == kErrorStopped) {
        return "stopped by operator";
    }
    if (error == kErrorShuttingDown) {
        return "runtime is shutting down";
    }
    if (error == kErrorTimeout) {
        return "readiness timeout";
    }
    if (error == kErrorCancelled) {
        return "cancelled";
    }
    if (error == kErrorNotAcquired) {
        return "external resource not acquired by this run";
    }
    if (error == kErrorNoStopCommand) {
        return "external resource has no stop_command";
    }
    if (error == kErrorRetriesExhausted) {
        return "restart budget exhausted";
    }
    if (error < 0) {
        return uv_strerror(error);
    }
    return "exited with status " + std::to_string(error);
}

ProcessManager::ProcessManager(EventLoop &loop)
    : loop_(loop), callback_owner_(std::make_shared<int>(0)) {
    on_process_start_.set_owner(callback_owner_);
    on_process_exit_.set_owner(callback_owner_);
    on_process_data_.set_owner(callback_owner_);
    on_process_error_.set_owner(callback_owner_);
    on_state_change_.set_owner(callback_owner_);
}

// Fallback cleanup only: the owner is expected to call Shutdown() and run the
// loop until it completes. Here we can only signal and release handles.
ProcessManager::~ProcessManager() {
    shutting_down_ = true;
    for (auto &entry : app_map_) {
        auto &app = entry.second;
        CancelPending(app);
        if (app.kill_timer) {
            app.kill_timer->close();
            app.kill_timer.reset();
        }
        if (app.group_timer) {
            app.group_timer->close();
            app.group_timer.reset();
        }
        if (app.process && app.process->is_running()) {
            app.process->kill_group(SIGTERM);
        }
        if (app.exiting) {
            app.exiting->kill_group(SIGKILL);
        }
        if (app.stopper && app.stopper->is_running()) {
            app.stopper->kill_group(SIGTERM);
        }
        app.start_waiters.clear();
        app.stop_waiters.clear();
        app.process.reset();
        app.exiting.reset();
    }
    for (auto &reaper : reapers_) {
        reaper->timer->close();
        reaper->process->kill_group(SIGKILL);
    }
    reapers_.clear();
    callback_owner_.reset();
    on_process_start_.clear();
    on_process_exit_.clear();
    on_process_data_.clear();
    on_process_error_.clear();
    on_state_change_.clear();
}

ProcessManager::App &ProcessManager::GetApp(const config::App &config) {
    auto it = app_map_.find(&config);
    if (it != app_map_.end()) {
        return it->second;
    }
    auto &app = app_map_[&config];
    app.config = &config;
    return app;
}

ProcessSnapshot ProcessManager::Snapshot(const App &app) const {
    ProcessSnapshot snapshot;
    snapshot.app = app.config;
    snapshot.desired = app.desired;
    snapshot.observed = app.observed;
    snapshot.pid =
        app.process && app.process->is_running() ? app.process->pid() : -1;
    snapshot.ready = app.ready;
    snapshot.acquired = app.acquired;
    snapshot.started_at_ms = app.launched_at_ms;
    snapshot.launches = app.launches;
    snapshot.retries = static_cast<int>(app.retry_times.size());
    snapshot.next_retry_at_ms = app.next_retry_at_ms;
    snapshot.last_error = app.last_error;
    snapshot.last_exit = app.last_exit;
    for (auto *file : {&app.stdout_file, &app.stderr_file}) {
        if (*file && !(*file)->error().empty()) {
            snapshot.output_error = (*file)->path() + ": " + (*file)->error();
            break;
        }
    }
    return snapshot;
}

ProcessSnapshot ProcessManager::snapshot(const config::App &config) {
    return Snapshot(GetApp(config));
}

void ProcessManager::SetObserved(App &app, ObservedState state) {
    if (app.observed == state) {
        return;
    }
    log_debug("%s: %s -> %s", str(app), to_string(app.observed),
              to_string(state));
    app.observed = state;
    if (state != ObservedState::Ready) {
        app.ready = false;
    }
    Emit(app);
}

void ProcessManager::Emit(const App &app) {
    on_state_change_.invoke(Snapshot(app));
}

void ProcessManager::InvokeAll(std::vector<Callback> &waiters,
                               std::shared_ptr<Process> process, int error) {
    auto callbacks = std::move(waiters);
    waiters.clear();
    for (auto &callback : callbacks) {
        if (callback) {
            callback(process, error);
        }
    }
}

void ProcessManager::Require(const config::App &config, Callback then) {
    Ensure(GetApp(config), std::move(then), false);
}

void ProcessManager::Start(const config::App &config, Callback then) {
    Ensure(GetApp(config), std::move(then), true);
}

void ProcessManager::Ensure(App &app, Callback then, bool explicit_request) {
    if (shutting_down_) {
        then(nullptr, kErrorShuttingDown);
        return;
    }
    if (explicit_request) {
        app.stop_requested = false;
        ResetRetryBudget(app);
    } else if (app.stop_requested) {
        log_debug("%s: require rejected (stopped by operator)", str(app));
        then(nullptr, kErrorStopped);
        return;
    }
    app.desired = DesiredState::Running;

    if (app.exit_pending) {
        app.start_waiters.push_back(std::move(then));
        return;
    }

    switch (app.observed) {
    case ObservedState::Ready: {
        if (!app.config->checker) {
            then(app.process, 0);
            return;
        }
        app.start_waiters.push_back(std::move(then));
        if (app.start_waiters.size() > 1) {
            return;
        }
        auto *app_ptr = &app;
        auto generation = app.generation;
        RunCheck(app, generation, [this, app_ptr, generation](int error) {
            auto &app = *app_ptr;
            if (app.generation != generation ||
                app.observed != ObservedState::Ready) {
                return;
            }
            if (error == 0) {
                InvokeAll(app.start_waiters, app.process, 0);
                return;
            }
            log_info("%s: no longer ready (%s)", str(app),
                     error_string(error).c_str());
            app.ready = false;
            if (app.process && app.process->is_running()) {
                app.pending_failure = error;
                Terminate(app);
            } else {
                BeginStart(app);
            }
        });
        return;
    }
    case ObservedState::Starting:
    case ObservedState::Backoff:
    case ObservedState::Stopping:
        app.start_waiters.push_back(std::move(then));
        return;
    case ObservedState::Completed:
        // A completed task satisfies dependants until explicitly rerun.
        if (app.config->type == config::App::Type::Task && !explicit_request) {
            then(nullptr, 0);
            return;
        }
        app.start_waiters.push_back(std::move(then));
        BeginStart(app);
        return;
    case ObservedState::Failed:
        if (app.exhausted) {
            log_debug("%s: require rejected (restart budget exhausted)",
                      str(app));
            then(nullptr, kErrorRetriesExhausted);
            return;
        }
        app.start_waiters.push_back(std::move(then));
        BeginStart(app);
        return;
    case ObservedState::Stopped:
        app.start_waiters.push_back(std::move(then));
        BeginStart(app);
        return;
    }
}

void ProcessManager::Stop(const config::App &config, Callback then) {
    auto &app = GetApp(config);
    app.stop_requested = true;
    app.desired = DesiredState::Stopped;
    app.pending_failure = 0;
    CancelPending(app);
    InvokeAll(app.start_waiters, nullptr, kErrorStopped);

    if (Terminating(app)) {
        app.stop_waiters.push_back(std::move(then));
        if (app.observed != ObservedState::Stopping) {
            if (app.exit_pending || app.stopper) {
                SetObserved(app, ObservedState::Stopping);
            } else {
                Terminate(app);
            }
        }
        return;
    }
    if (app.config->type == config::App::Type::External) {
        StopExternal(app, std::move(then));
        return;
    }
    if (app.observed != ObservedState::Completed &&
        app.observed != ObservedState::Failed) {
        SetObserved(app, ObservedState::Stopped);
    } else {
        Emit(app);
    }
    then(nullptr, 0);
}

// Only a resource launched by this run is stopped; an already available one
// is left alone, and without a stop_command it simply remains running.
void ProcessManager::StopExternal(App &app, Callback then) {
    if (!app.acquired) {
        log_info("%s: external resource was not acquired by this run, "
                 "leaving it running",
                 str(app));
    } else if (!app.config->stop_command) {
        log_warn("%s: external resource remains running (no stop_command)",
                 str(app));
    } else {
        app.stop_waiters.push_back(std::move(then));
        RunStopCommand(app);
        return;
    }
    if (app.observed != ObservedState::Failed) {
        SetObserved(app, ObservedState::Stopped);
    } else {
        Emit(app);
    }
    then(nullptr, 0);
}

void ProcessManager::RunStopCommand(App &app) {
    SetObserved(app, ObservedState::Stopping);
    auto *command = app.config->stop_command;
    auto process = Process::create(loop_, *command);
    process->set_name("stop");
    process->set_own_process_group(true);
    app.stopper = process;
    log_info("%s: running stop_command", str(app));

    std::weak_ptr<Process> weak_process = process;
    std::weak_ptr<void> alive = callback_owner_;
    auto *app_ptr = &app;

    process->on_data(
        [this, alive, weak_process](int fd, const char *s, size_t len) {
            if (alive.expired()) {
                return;
            }
            if (auto process = weak_process.lock()) {
                on_process_data_.invoke(process, fd, s, len);
            }
        });
    process->on_error([this, alive, app_ptr, weak_process](int error) {
        if (alive.expired()) {
            return;
        }
        auto process = weak_process.lock();
        auto &app = *app_ptr;
        if (!process || app.stopper != process || process->is_running()) {
            return;
        }
        on_process_error_.invoke(process, error);
        StopCommandDone(app, error, true);
    });
    process->on_exit(
        [this, alive, app_ptr, weak_process](int64_t status, int signal) {
            if (alive.expired()) {
                return;
            }
            auto process = weak_process.lock();
            auto &app = *app_ptr;
            if (!process || app.stopper != process) {
                return;
            }
            status = status ? status : signal;
            on_process_exit_.invoke(process, status, signal);
            int error = static_cast<int>(status);
            if (!process->group_alive()) {
                StopCommandDone(app, error, true);
                return;
            }
            log_info("%s: descendants of stop_command remain, terminating "
                     "them",
                     str(app));
            process->kill_group(SIGTERM);
            if (!app.kill_timer) {
                StartKillTimer(app, process);
            }
            AwaitGroup(app, process, [this, app_ptr, error](bool clean) {
                StopCommandDone(*app_ptr, error, clean);
            });
        });
    if (process->start()) {
        orphan_reaper_.track(process->pid(), force_after_ms_);
        on_process_start_.invoke(process);
        StartKillTimer(app, process);
    }
}

void ProcessManager::StopCommandDone(App &app, int error, bool group_clean) {
    app.stopper.reset();
    app.killed_at_ms = 0;
    if (app.kill_timer) {
        app.kill_timer->close();
        app.kill_timer.reset();
    }
    if (error == 0) {
        log_info("%s: external resource stopped", str(app));
        app.acquired = false;
        app.cleanup_failed = !group_clean;
        SetObserved(app, ObservedState::Stopped);
        InvokeAll(app.stop_waiters, nullptr, 0);
        if (shutting_down_) {
            ContinueShutdown();
        } else if (app.desired == DesiredState::Running &&
                   !app.stop_requested) {
            BeginStart(app);
        }
        return;
    }
    log_error("%s: stop_command failed (%s); external resource may still be "
              "running",
              str(app), error_string(error).c_str());
    app.last_error = error;
    app.cleanup_failed = true;
    SetObserved(app, ObservedState::Failed);
    InvokeAll(app.stop_waiters, nullptr, error);
    InvokeAll(app.start_waiters, nullptr, error);
    if (shutting_down_) {
        ContinueShutdown();
    }
}

void ProcessManager::Restart(const config::App &config, Callback then) {
    auto &app = GetApp(config);
    if (shutting_down_) {
        then(nullptr, kErrorShuttingDown);
        return;
    }
    app.stop_requested = false;
    app.desired = DesiredState::Running;
    app.pending_failure = 0;
    ResetRetryBudget(app);
    CancelPending(app);

    if (Terminating(app)) {
        app.start_waiters.push_back(std::move(then));
        if (app.observed != ObservedState::Stopping) {
            if (app.exit_pending || app.stopper) {
                SetObserved(app, ObservedState::Stopping);
            } else {
                Terminate(app);
            }
        }
        return;
    }
    if (app.config->type == config::App::Type::External) {
        if (!app.acquired) {
            then(nullptr, kErrorNotAcquired);
            return;
        }
        if (!app.config->stop_command) {
            then(nullptr, kErrorNoStopCommand);
            return;
        }
        app.start_waiters.push_back(std::move(then));
        RunStopCommand(app);
        return;
    }
    app.start_waiters.push_back(std::move(then));
    BeginStart(app);
}

// `stopper` stays set until the stop command and its descendants are gone.
bool ProcessManager::Terminating(const App &app) {
    return app.exit_pending || (app.process && app.process->is_running()) ||
           app.stopper != nullptr;
}

bool ProcessManager::PendingShutdownWork(const App &app) const {
    if (Terminating(app)) {
        return true;
    }
    return app.config->type == config::App::Type::External && app.acquired &&
           app.config->stop_command && app.observed != ObservedState::Failed;
}

void ProcessManager::Shutdown(std::function<void(bool)> done) {
    if (done) {
        shutdown_waiters_.push_back(std::move(done));
    }
    if (shutting_down_) {
        ContinueShutdown();
        return;
    }
    shutting_down_ = true;
    log_debug("process manager: shutting down");

    for (auto &entry : app_map_) {
        auto &app = entry.second;
        app.desired = DesiredState::Stopped;
        app.pending_failure = 0;
        CancelPending(app);
        InvokeAll(app.start_waiters, nullptr, kErrorCancelled);
        if (PendingShutdownWork(app)) {
            continue;
        }
        if (app.config->type == config::App::Type::External && app.acquired) {
            log_warn("%s: external resource remains running (no stop_command)",
                     str(app));
        }
        if (app.observed != ObservedState::Completed &&
            app.observed != ObservedState::Failed) {
            SetObserved(app, ObservedState::Stopped);
        }
    }
    ContinueShutdown();
}

bool ProcessManager::HasDependantTerminating(const App &app) const {
    for (auto &entry : app_map_) {
        auto &other = entry.second;
        if (&other == &app || !PendingShutdownWork(other)) {
            continue;
        }
        auto &preamble = other.config->preamble;
        if (std::find(preamble.begin(), preamble.end(), app.config) !=
            preamble.end()) {
            return true;
        }
    }
    return false;
}

// Terminates every app whose dependants are gone, then completes the
// shutdown once nothing owned remains.
void ProcessManager::ContinueShutdown() {
    if (!shutting_down_) {
        return;
    }
    bool pending = false;
    for (auto &entry : app_map_) {
        auto &app = entry.second;
        if (!PendingShutdownWork(app)) {
            continue;
        }
        pending = true;
        if (app.observed == ObservedState::Stopping ||
            HasDependantTerminating(app)) {
            continue;
        }
        if (app.exit_pending || app.stopper) {
            SetObserved(app, ObservedState::Stopping);
        } else if (app.process && app.process->is_running()) {
            Terminate(app);
        } else {
            RunStopCommand(app);
        }
    }
    if (pending || !reapers_.empty()) {
        return;
    }
    bool clean = !reap_failed_;
    for (auto &entry : app_map_) {
        if (entry.second.cleanup_failed) {
            clean = false;
        }
    }
    auto waiters = std::move(shutdown_waiters_);
    shutdown_waiters_.clear();
    for (auto &done : waiters) {
        done(clean);
    }
}

void ProcessManager::ResetRetryBudget(App &app) {
    app.retry_times.clear();
    app.next_retry_at_ms = 0;
    app.exhausted = false;
}

int ProcessManager::PruneRetries(App &app) {
    auto now = Timer::now();
    auto window = app.config->restart.window_ms;
    while (!app.retry_times.empty() &&
           now - app.retry_times.front() >= window) {
        app.retry_times.pop_front();
    }
    return static_cast<int>(app.retry_times.size());
}

// Invalidates the current start attempt: pending checks, retries and preamble
// continuations are dropped. Does not touch the owned process or waiters.
void ProcessManager::CancelPending(App &app) {
    app.generation++;
    CancelCheck(app);
    if (app.retry_timer) {
        app.retry_timer->close();
        app.retry_timer.reset();
    }
    app.next_retry_at_ms = 0;
}

void ProcessManager::CancelCheck(App &app) {
    if (app.check_timer) {
        app.check_timer->close();
        app.check_timer.reset();
    }
    if (app.checker) {
        if (app.checker->is_running() || app.checker->group_alive()) {
            ReapGroup(app.checker, SIGKILL);
        }
        app.checker.reset();
    }
}

// Terminates a helper's process group in the background: SIGTERM (or the
// given signal) now, SIGKILL after force_after_ms_, bounded wait after that.
void ProcessManager::ReapGroup(std::shared_ptr<Process> process, int signal) {
    for (auto &existing : reapers_) {
        if (existing->process == process) {
            return;
        }
    }
    auto reaper = std::make_shared<Reaper>();
    reaper->process = process;
    reaper->started_ms = Timer::now();
    process->kill_group(signal);
    if (signal == SIGKILL) {
        reaper->killed_at_ms = reaper->started_ms;
    }

    std::weak_ptr<void> alive = callback_owner_;
    std::weak_ptr<Reaper> weak_reaper = reaper;
    auto timer = Timer::create(loop_, kGroupPollMs);
    reaper->timer = timer;
    timer->on_tick([this, alive, weak_reaper] {
        if (alive.expired()) {
            return;
        }
        auto reaper = weak_reaper.lock();
        if (!reaper) {
            return;
        }
        auto now = Timer::now();
        bool gone = !reaper->process->group_alive();
        if (!gone && reaper->killed_at_ms == 0 &&
            now - reaper->started_ms >= force_after_ms_) {
            log_error("%s: process group %d ignored SIGTERM, sending SIGKILL",
                      reaper->process->name().c_str(), reaper->process->pid());
            reaper->process->kill_group(SIGKILL);
            reaper->killed_at_ms = now;
        }
        bool gave_up = !gone && reaper->killed_at_ms != 0 &&
                       now - reaper->killed_at_ms >= kAfterKillWaitMs;
        if (!gone && !gave_up) {
            return;
        }
        if (gave_up) {
            log_error("%s: descendants of pid %d survived SIGKILL",
                      reaper->process->name().c_str(), reaper->process->pid());
            reap_failed_ = true;
        }
        reaper->timer->close();
        reapers_.erase(std::remove(reapers_.begin(), reapers_.end(), reaper),
                       reapers_.end());
        ContinueShutdown();
    });
    timer->start();
    reapers_.push_back(std::move(reaper));
}

void ProcessManager::BeginStart(App &app) {
    CancelPending(app);
    auto generation = app.generation;
    app.ready = false;
    app.last_error = 0;
    app.pending_failure = 0;
    app.launched_this_attempt = false;
    SetObserved(app, ObservedState::Starting);

    auto *app_ptr = &app;
    std::weak_ptr<void> alive = callback_owner_;
    CheckPreamble(app, generation,
                  [this, alive, app_ptr, generation](int error) {
                      if (alive.expired()) {
                          return;
                      }
                      auto &app = *app_ptr;
                      if (app.generation != generation) {
                          return;
                      }
                      if (error != 0) {
                          log_error("%s: dependency failed (%s)", str(app),
                                    error_string(error).c_str());
                          FinishStart(app, error);
                          return;
                      }
                      Launch(app);
                  });
}

void ProcessManager::CheckPreamble(App &app, uint64_t generation,
                                   std::function<void(int)> then) {
    auto &preamble = app.config->preamble;
    if (preamble.empty()) {
        then(0);
        return;
    }

    struct Pending {
        size_t remaining;
        int error = 0;
        std::function<void(int)> then;
    };
    auto pending = std::make_shared<Pending>();
    pending->remaining = preamble.size();
    pending->then = std::move(then);

    log_debug("%s: requiring %zu dependencies", str(app), preamble.size());
    for (auto *dependency : preamble) {
        Require(*dependency, [pending](std::shared_ptr<Process>, int error) {
            if (error != 0 && pending->error == 0) {
                pending->error = error;
            }
            if (--pending->remaining == 0) {
                pending->then(pending->error);
            }
        });
    }
    (void)generation;
}

// A passing checker means the resource is already available, so nothing is
// launched; otherwise the command is spawned and readiness polled.
void ProcessManager::Launch(App &app) {
    if (!app.config->checker) {
        Spawn(app);
        return;
    }
    auto generation = app.generation;
    auto *app_ptr = &app;
    RunCheck(app, generation, [this, app_ptr, generation](int error) {
        auto &app = *app_ptr;
        if (app.generation != generation) {
            return;
        }
        if (error == 0) {
            log_debug("%s: already available, not launching", str(app));
            FinishStart(app, 0);
            return;
        }
        if (app.config->command.file.empty()) {
            FinishStart(app, error);
            return;
        }
        Spawn(app);
    });
}

void ProcessManager::Spawn(App &app) {
    auto &command = app.config->command;
    auto *app_ptr = &app;
    std::weak_ptr<void> alive = callback_owner_;

    if (command.file.empty()) {
        FinishStart(app, 0);
        return;
    }

    auto process = Process::create(loop_, command);
    process->set_name(str(app));
    process->set_own_process_group(true);
    std::weak_ptr<Process> weak_process = process;
    OpenOutputFiles(app);

    process->on_data([this, alive, app_ptr, weak_process](int fd, const char *s,
                                                          size_t len) {
        if (alive.expired()) {
            return;
        }
        if (auto process = weak_process.lock()) {
            WriteOutput(*app_ptr, fd, s, len);
            on_process_data_.invoke(process, fd, s, len);
        }
    });

    process->on_error([this, alive, app_ptr, weak_process](int error) {
        if (alive.expired()) {
            return;
        }
        auto process = weak_process.lock();
        auto &app = *app_ptr;
        if (!process || app.process != process || process->is_running()) {
            return;
        }
        log_error("%s: failed to spawn (%s)", str(app), uv_strerror(error));
        app.process.reset();
        on_process_error_.invoke(process, error);
        HandleFailure(app, error);
    });

    process->on_exit(
        [this, alive, app_ptr, weak_process](int64_t status, int signal) {
            if (alive.expired()) {
                return;
            }
            auto process = weak_process.lock();
            auto &app = *app_ptr;
            if (!process || app.process != process) {
                return;
            }
            HandleExit(app, process, status, signal);
        });

    app.process = process;
    app.launched_at_ms = Timer::now();
    app.launches++;
    app.launched_this_attempt = true;
    log_debug("%s: launching", str(app));
    if (!process->start()) {
        return;
    }
    orphan_reaper_.track(process->pid(), force_after_ms_);
    on_process_start_.invoke(process);
    Emit(app);

    if (app.config->type == config::App::Type::Task) {
        return; // success is the zero exit
    }
    if (!app.config->checker) {
        FinishStart(app, 0);
        return;
    }
    ScheduleCheck(app, kCheckIntervalMs);
}

// Files are opened once per app and appended to across restarts.
void ProcessManager::OpenOutputFiles(App &app) {
    auto &logs = app.config->logs;
    if (!logs.enabled() || app.stdout_file || app.stderr_file) {
        return;
    }
    if (!logs.stdout_file.empty()) {
        app.stdout_file = std::make_shared<OutputFile>(
            logs.stdout_file, logs.max_bytes, logs.max_files);
    }
    if (!logs.stderr_file.empty()) {
        app.stderr_file =
            logs.stderr_file == logs.stdout_file
                ? app.stdout_file
                : std::make_shared<OutputFile>(logs.stderr_file, logs.max_bytes,
                                               logs.max_files);
    }
}

void ProcessManager::WriteOutput(App &app, int fd, const char *data,
                                 size_t len) {
    auto &file = fd == 2 ? app.stderr_file : app.stdout_file;
    if (file) {
        file->write(data, len);
    }
}

void ProcessManager::ScheduleCheck(App &app, uint64_t delay_ms) {
    auto generation = app.generation;
    auto *app_ptr = &app;
    std::weak_ptr<void> alive = callback_owner_;

    if (app.check_timer) {
        app.check_timer->close();
    }
    auto timer = Timer::create(loop_, delay_ms);
    app.check_timer = timer;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([this, alive, app_ptr, generation, weak_timer] {
        if (alive.expired()) {
            return;
        }
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        auto &app = *app_ptr;
        if (app.generation != generation) {
            return;
        }
        app.check_timer.reset();
        RunCheck(app, generation, [this, app_ptr, generation](int error) {
            auto &app = *app_ptr;
            if (app.generation != generation) {
                return;
            }
            if (error == 0) {
                FinishStart(app, 0);
                return;
            }
            app.last_error = error;
            auto elapsed = Timer::now() - app.launched_at_ms;
            auto limit = static_cast<uint64_t>(app.config->max_start_time);
            if (limit > 0 && elapsed >= limit) {
                log_error("%s: not ready after %llums (%s)", str(app),
                          (unsigned long long)elapsed,
                          error_string(error).c_str());
                if (app.process && app.process->is_running()) {
                    app.pending_failure = kErrorTimeout;
                    Terminate(app);
                } else {
                    HandleFailure(app, kErrorTimeout);
                }
                return;
            }
            ScheduleCheck(app, kCheckIntervalMs);
        });
    });
    timer->start(false);
}

void ProcessManager::RunCheck(App &app, uint64_t generation,
                              std::function<void(int)> then) {
    auto *checker_config = app.config->checker;
    if (!checker_config) {
        then(0);
        return;
    }

    auto process = Process::create(loop_, *checker_config);
    process->set_name("checker");
    process->set_own_process_group(true);
    app.checker = process;

    std::weak_ptr<Process> weak_process = process;
    std::weak_ptr<void> alive = callback_owner_;
    auto *app_ptr = &app;

    auto once = std::make_shared<bool>(false);
    auto finish = [this, alive, app_ptr, generation, weak_process, once,
                   then](int code) {
        if (*once || alive.expired()) {
            return;
        }
        *once = true;
        auto &app = *app_ptr;
        if (auto process = weak_process.lock()) {
            if (app.checker == process) {
                app.checker.reset();
            }
        }
        if (app.generation != generation) {
            return;
        }
        then(code);
    };

    process->on_data(
        [this, alive, weak_process](int fd, const char *s, size_t len) {
            if (alive.expired()) {
                return;
            }
            if (auto process = weak_process.lock()) {
                on_process_data_.invoke(process, fd, s, len);
            }
        });

    process->on_error([this, alive, weak_process, finish](int error) {
        if (alive.expired()) {
            return;
        }
        if (auto process = weak_process.lock()) {
            if (!process->is_running()) {
                on_process_error_.invoke(process, error);
                finish(error);
            }
        }
    });

    process->on_exit([this, alive, app_ptr, weak_process,
                      finish](int64_t status, int signal) {
        if (alive.expired()) {
            return;
        }
        auto process = weak_process.lock();
        status = status ? status : signal;
        on_process_exit_.invoke(process, status, signal);
        if (process && process->group_alive()) {
            log_info("%s: descendants of checker pid %d remain, "
                     "terminating them",
                     str(*app_ptr), process->pid());
            ReapGroup(process, SIGTERM);
        }
        finish(static_cast<int>(status));
    });

    if (process->start()) {
        orphan_reaper_.track(process->pid(), force_after_ms_);
        on_process_start_.invoke(process);
    }
}

void ProcessManager::FinishStart(App &app, int error) {
    if (error == 0) {
        app.ready = true;
        app.last_error = 0;
        if (app.config->type == config::App::Type::External &&
            app.launched_this_attempt) {
            app.acquired = true;
            log_info("%s: external resource acquired", str(app));
        }
        log_info("%s: ready", str(app));
        SetObserved(app, ObservedState::Ready);
        InvokeAll(app.start_waiters, app.process, 0);
        return;
    }
    app.ready = false;
    app.last_error = error;
    log_error("%s: failed to start (%s)", str(app),
              error_string(error).c_str());
    SetObserved(app, ObservedState::Failed);
    InvokeAll(app.start_waiters, app.process, error);
}

void ProcessManager::Terminate(App &app) {
    auto process = app.process;
    SetObserved(app, ObservedState::Stopping);
    log_debug("%s: sending SIGTERM to process group %d", str(app),
              process->pid());
    int status = process->kill_group(SIGTERM);
    if (status != 0 && status != UV_ESRCH) {
        log_error("%s: failed to send SIGTERM: %s", str(app),
                  uv_strerror(status));
    }
    StartKillTimer(app, process);
}

// After force_after_ms_ the whole group is SIGKILLed; AwaitGroup bounds the
// wait for descendants after that.
void ProcessManager::StartKillTimer(App &app,
                                    std::shared_ptr<Process> process) {
    if (app.kill_timer) {
        app.kill_timer->close();
    }
    auto timer = Timer::create(loop_, force_after_ms_);
    app.kill_timer = timer;
    std::weak_ptr<Timer> weak_timer = timer;
    std::weak_ptr<Process> weak_process = process;
    std::weak_ptr<void> alive = callback_owner_;
    auto *app_ptr = &app;
    timer->on_tick([this, alive, app_ptr, weak_timer, weak_process] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (alive.expired()) {
            return;
        }
        auto &app = *app_ptr;
        app.kill_timer.reset();
        auto process = weak_process.lock();
        bool current =
            process && (app.process == process || app.stopper == process ||
                        (app.exit_pending && app.exiting == process));
        if (!current || !process->group_alive()) {
            return;
        }
        log_error("%s: process group %d ignored SIGTERM, sending SIGKILL",
                  str(app), process->pid());
        app.killed_at_ms = Timer::now();
        int status = process->kill_group(SIGKILL);
        if (status != 0 && status != UV_ESRCH) {
            log_error("%s: failed to send SIGKILL: %s", str(app),
                      uv_strerror(status));
        }
    });
    timer->start(false);
}

void ProcessManager::HandleExit(App &app, std::shared_ptr<Process> process,
                                int64_t status, int signal) {
    int error = static_cast<int>(status ? status : signal);
    log_info("%s: pid %d exited (%s)", str(app), process->pid(),
             error_string(error).c_str());

    app.last_exit = {true, status, signal, Timer::now()};
    app.process.reset();
    on_process_exit_.invoke(process, status ? status : signal, signal);

    app.exit_pending = true;
    app.exiting = process;
    app.pending_status = status;
    app.pending_signal = signal;
    if (!process->group_alive()) {
        FinalizeExit(app);
        return;
    }
    log_info("%s: descendants of pid %d remain, terminating them", str(app),
             process->pid());
    auto *app_ptr = &app;
    process->kill_group(SIGTERM);
    if (!app.kill_timer) {
        StartKillTimer(app, process);
    }
    Emit(app);
    AwaitGroup(app, process, [this, app_ptr](bool clean) {
        if (!clean) {
            app_ptr->cleanup_failed = true;
        }
        FinalizeExit(*app_ptr);
    });
}

void ProcessManager::AwaitGroup(App &app, std::shared_ptr<Process> process,
                                std::function<void(bool)> done) {
    auto *app_ptr = &app;
    std::weak_ptr<void> alive = callback_owner_;
    if (app.group_timer) {
        app.group_timer->close();
    }
    auto timer = Timer::create(loop_, kGroupPollMs);
    app.group_timer = timer;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([this, alive, app_ptr, process, done, weak_timer] {
        if (alive.expired()) {
            return;
        }
        auto &app = *app_ptr;
        bool gone = !process->group_alive();
        bool gave_up = !gone && app.killed_at_ms != 0 &&
                       Timer::now() - app.killed_at_ms >= kAfterKillWaitMs;
        if (!gone && !gave_up) {
            return;
        }
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        app.group_timer.reset();
        if (gave_up) {
            log_error("%s: descendants of pid %d survived SIGKILL", str(app),
                      process->pid());
        }
        done(!gave_up);
    });
    timer->start();
}

void ProcessManager::FinalizeExit(App &app) {
    auto process = app.exiting;
    int64_t status = app.pending_status;
    int signal = app.pending_signal;
    int error = static_cast<int>(status ? status : signal);
    app.exit_pending = false;
    app.exiting.reset();
    app.killed_at_ms = 0;
    if (app.kill_timer) {
        app.kill_timer->close();
        app.kill_timer.reset();
    }

    bool external = app.config->type == config::App::Type::External;
    if (shutting_down_ || app.observed == ObservedState::Stopping) {
        if (!shutting_down_ && app.pending_failure != 0) {
            int failure = app.pending_failure;
            app.pending_failure = 0;
            app.ready = false;
            HandleFailure(app, failure);
            return;
        }
        if (external && app.acquired && app.config->stop_command) {
            RunStopCommand(app);
            return;
        }
        SetObserved(app, ObservedState::Stopped);
        InvokeAll(app.stop_waiters, process, 0);
        if (shutting_down_) {
            ContinueShutdown();
        } else if (app.desired == DesiredState::Running &&
                   !app.stop_requested) {
            BeginStart(app);
        }
        return;
    }

    // A launcher finishing after its resource became ready is informational.
    if (external && app.observed == ObservedState::Ready) {
        if (error != 0) {
            log_warn("%s: launcher exited after the resource became ready "
                     "(%s)",
                     str(app), error_string(error).c_str());
        }
        Emit(app);
        return;
    }

    // A command that completes before its checker passes may still bring the
    // resource up (e.g. `docker start`), so keep polling until the deadline.
    if (error == 0 && app.observed == ObservedState::Starting &&
        app.config->checker) {
        Emit(app);
        return;
    }

    CancelPending(app);
    app.ready = false;
    if (error == 0) {
        if (app.config->restart.policy == config::Restart::Policy::Always) {
            if (ScheduleRetry(app)) {
                return;
            }
            if (app.exhausted) {
                HandleFailure(app, kErrorRetriesExhausted);
                return;
            }
        }
        app.last_error = 0;
        SetObserved(app, ObservedState::Completed);
        InvokeAll(app.start_waiters, nullptr, 0);
        return;
    }
    HandleFailure(app, error);
}

void ProcessManager::HandleFailure(App &app, int error) {
    auto policy = app.config->restart.policy;
    if (policy != config::Restart::Policy::Never && ScheduleRetry(app)) {
        return;
    }
    if (app.exhausted) {
        error = kErrorRetriesExhausted;
    }
    app.last_error = error;
    SetObserved(app, ObservedState::Failed);
    InvokeAll(app.start_waiters, nullptr, error);
}

bool ProcessManager::ScheduleRetry(App &app) {
    if (shutting_down_ || app.stop_requested) {
        return false;
    }
    auto &restart = app.config->restart;
    int used = PruneRetries(app);
    if (used >= restart.max_retries) {
        log_error("%s: restart budget exhausted (%d retries within %llums); "
                  "an explicit start or restart resets it",
                  str(app), used, (unsigned long long)restart.window_ms);
        app.exhausted = true;
        return false;
    }

    uint64_t delay = restart.delay_ms;
    for (int i = 0; i < used && delay < restart.max_delay_ms; i++) {
        delay *= 2;
    }
    delay = std::min(delay, restart.max_delay_ms);

    app.next_retry_at_ms = Timer::now() + delay;
    log_info("%s: restarting in %llums (retry %d of %d)", str(app),
             (unsigned long long)delay, used + 1, restart.max_retries);
    SetObserved(app, ObservedState::Backoff);

    auto generation = app.generation;
    auto *app_ptr = &app;
    std::weak_ptr<void> alive = callback_owner_;
    auto timer = Timer::create(loop_, delay);
    app.retry_timer = timer;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([this, alive, app_ptr, generation, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (alive.expired()) {
            return;
        }
        auto &app = *app_ptr;
        if (app.generation != generation) {
            return;
        }
        app.retry_timer.reset();
        app.retry_times.push_back(Timer::now());
        BeginStart(app);
    });
    timer->start(false);
    return true;
}

const char *ProcessManager::str(const config::Command &cmd) {
    auto it = str_map_.find(&cmd);
    if (it != str_map_.end()) {
        return it->second->c_str();
    }

    std::stringstream ss;
    size_t n = 0;

    // Only keys: values may hold credentials and this label reaches logs.
    for (auto &p : cmd.env) {
        if (n++ > 0) {
            ss << ' ';
        }
        ss << p.substr(0, p.find('='));
    }

    if (cmd.env.size() > 0) {
        ss << ' ';
    }

    ss << cmd.file;

    for (auto &p : cmd.args) {
        ss << ' ';
        ss << p;
    }

    auto str_ptr = std::make_shared<std::string>(ss.str());
    str_map_.emplace(&cmd, str_ptr);

    return str_ptr->c_str();
}

const char *ProcessManager::str(const config::App &app) {
    if (app.name.size() > 0) {
        return app.name.c_str();
    }
    return str(app.command);
}

const char *ProcessManager::str(const App &app) { return str(*app.config); }

} // namespace nexer
