#ifndef NEXER_PROCESS_MANAGER_H_
#define NEXER_PROCESS_MANAGER_H_

#include "nexer/core/event_loop.h"
#include "nexer/core/function_list.h"
#include "nexer/core/timer.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/orphan_reaper.h"
#include "nexer/runtime/output_file.h"
#include "nexer/runtime/process.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nexer {

enum class DesiredState { Running, Stopped };

enum class ObservedState {
    Starting,
    Ready,
    Backoff,
    Stopping,
    Stopped,
    Completed,
    Failed,
};

const char *to_string(DesiredState);
const char *to_string(ObservedState);

// Operation errors other than libuv codes and positive child exit codes.
constexpr int kErrorStopped = UV_EPERM;
constexpr int kErrorShuttingDown = UV_ESHUTDOWN;
constexpr int kErrorTimeout = UV_ETIMEDOUT;
constexpr int kErrorCancelled = UV_ECANCELED;
// External resource was not launched by this run, so it cannot be restarted.
constexpr int kErrorNotAcquired = UV_EBUSY;
// External resource has no stop_command.
constexpr int kErrorNoStopCommand = UV_ENOTSUP;
// Automatic restart budget used up; cleared by an explicit start/restart.
constexpr int kErrorRetriesExhausted = UV_EAGAIN;

// Human readable description of an operation error or exit status.
std::string error_string(int error);

struct ProcessExit {
    bool present = false;
    int64_t code = 0;
    int signal = 0;
    uint64_t at_ms = 0;
};

struct ProcessSnapshot {
    const config::App *app = nullptr;
    DesiredState desired = DesiredState::Stopped;
    ObservedState observed = ObservedState::Stopped;
    int pid = -1;
    bool ready = false;
    // External resource launched by this run (and not stopped since).
    bool acquired = false;
    uint64_t started_at_ms = 0;
    uint64_t launches = 0;
    int retries = 0;
    uint64_t next_retry_at_ms = 0;
    int last_error = 0;
    ProcessExit last_exit;
    // Last disk failure of the configured stdout/stderr files, empty when
    // writing works (or no files are configured).
    std::string output_error;
};

class ProcessManager {
  public:
    typedef std::function<void(std::shared_ptr<Process>, int)> Callback;
    typedef Callback AfterProcessCheck;

  private:
    struct App {
        const config::App *config = nullptr;
        DesiredState desired = DesiredState::Stopped;
        ObservedState observed = ObservedState::Stopped;
        bool stop_requested = false;
        bool ready = false;
        std::shared_ptr<Process> process;
        std::shared_ptr<Process> checker;
        std::shared_ptr<Process> stopper;
        std::shared_ptr<OutputFile> stdout_file;
        std::shared_ptr<OutputFile> stderr_file;
        bool acquired = false;
        bool launched_this_attempt = false;
        std::shared_ptr<Timer> check_timer;
        std::shared_ptr<Timer> retry_timer;
        std::shared_ptr<Timer> kill_timer;
        std::shared_ptr<Timer> group_timer;
        // Set once the leader exited while members of its process group
        // remain; the exit is finalized when the group is gone.
        bool exit_pending = false;
        std::shared_ptr<Process> exiting;
        int64_t pending_status = 0;
        int pending_signal = 0;
        uint64_t killed_at_ms = 0;
        bool cleanup_failed = false;
        uint64_t generation = 0;
        uint64_t launched_at_ms = 0;
        uint64_t launches = 0;
        int last_error = 0;
        int pending_failure = 0;
        std::deque<uint64_t> retry_times;
        uint64_t next_retry_at_ms = 0;
        bool exhausted = false;
        ProcessExit last_exit;
        std::vector<Callback> start_waiters;
        std::vector<Callback> stop_waiters;
    };

    EventLoop &loop_;
    std::map<const config::App *, App> app_map_;
    std::map<const config::Command *, std::shared_ptr<std::string>> str_map_;

    std::shared_ptr<void> callback_owner_;
    FunctionList<void, std::shared_ptr<Process>> on_process_start_;
    FunctionList<void, std::shared_ptr<Process>, int64_t, int> on_process_exit_;
    FunctionList<void, std::shared_ptr<Process>, int, const char *, size_t>
        on_process_data_;
    FunctionList<void, std::shared_ptr<Process>, int> on_process_error_;
    FunctionList<void, const ProcessSnapshot &> on_state_change_;

    // Process groups of finished helpers (checkers) whose descendants are
    // still being terminated; shutdown waits for them.
    struct Reaper {
        std::shared_ptr<Process> process;
        std::shared_ptr<Timer> timer;
        uint64_t started_ms = 0;
        uint64_t killed_at_ms = 0;
    };
    std::vector<std::shared_ptr<Reaper>> reapers_;
    bool reap_failed_ = false;

    bool shutting_down_ = false;
    std::vector<std::function<void(bool)>> shutdown_waiters_;
    uint64_t force_after_ms_ = 30000;
    // Covers every owned process group in case this process dies before
    // stopping it.
    OrphanReaper orphan_reaper_;

    App &GetApp(const config::App &);
    ProcessSnapshot Snapshot(const App &) const;

    void Ensure(App &, Callback, bool explicit_request);
    void StopExternal(App &, Callback);
    void OpenOutputFiles(App &);
    void WriteOutput(App &, int fd, const char *data, size_t len);
    void RunStopCommand(App &);
    void StopCommandDone(App &, int error, bool group_clean);
    bool PendingShutdownWork(const App &) const;
    void BeginStart(App &);
    void Launch(App &);
    void Spawn(App &);
    void ScheduleCheck(App &, uint64_t delay_ms);
    void RunCheck(App &, uint64_t generation, std::function<void(int)>);
    void FinishStart(App &, int error);
    void HandleExit(App &, std::shared_ptr<Process>, int64_t status,
                    int signal);
    void FinalizeExit(App &);
    // Polls until the process group is gone (bounded after SIGKILL) and
    // reports whether cleanup was clean.
    void AwaitGroup(App &, std::shared_ptr<Process>,
                    std::function<void(bool clean)>);
    void StartKillTimer(App &, std::shared_ptr<Process>);
    void HandleFailure(App &, int error);
    bool ScheduleRetry(App &);
    void CancelPending(App &);
    void CancelCheck(App &);
    void ReapGroup(std::shared_ptr<Process>, int signal);
    void Terminate(App &);
    void CheckPreamble(App &, uint64_t generation, std::function<void(int)>);
    void SetObserved(App &, ObservedState);
    void Emit(const App &);
    void ContinueShutdown();
    bool HasDependantTerminating(const App &) const;
    static bool Terminating(const App &);
    void ResetRetryBudget(App &);
    int PruneRetries(App &);

    static void InvokeAll(std::vector<Callback> &, std::shared_ptr<Process>,
                          int error);

    const char *str(const config::Command &);
    const char *str(const config::App &);
    const char *str(const App &);

  public:
    explicit ProcessManager(EventLoop &);
    ~ProcessManager();

    // Maximum wait after SIGTERM before a stopped process is SIGKILLed.
    inline void set_force_after_ms(uint64_t ms) { force_after_ms_ = ms; }

    inline auto
    OnProcessStart(std::function<void(std::shared_ptr<Process>)> fn) {
        return on_process_start_.add(fn);
    }
    inline auto OnProcessExit(
        std::function<void(std::shared_ptr<Process>, int64_t, int)> fn) {
        return on_process_exit_.add(fn);
    }
    inline auto OnProcessData(
        std::function<void(std::shared_ptr<Process>, int, const char *, size_t)>
            fn) {
        return on_process_data_.add(fn);
    }
    inline auto
    OnProcessError(std::function<void(std::shared_ptr<Process>, int)> fn) {
        return on_process_error_.add(fn);
    }
    inline auto OnStateChange(std::function<void(const ProcessSnapshot &)> fn) {
        return on_state_change_.add(fn);
    }

    // Ensures the app is ready unless it was deliberately stopped. Used by
    // proxies and dependencies; concurrent calls share one operation.
    void Require(const config::App &, Callback);
    // Explicit operator intent: clears a deliberate stop, resets the restart
    // budget and ensures the app is ready.
    void Start(const config::App &, Callback);
    // Records a deliberate stop, cancels pending work and terminates the
    // owned process. Blocks automatic restarts and requires until Start or
    // Restart.
    void Stop(const config::App &, Callback);
    // Stops (if running) then starts, even when the old child already exited.
    void Restart(const config::App &, Callback);
    // Rejects new starts, fails pending operations and terminates every owned
    // process group, dependants before their dependencies. `done` receives
    // false when owned children remained after the bounded SIGKILL wait.
    void Shutdown(std::function<void(bool clean)> done = {});

    inline bool shutting_down() const { return shutting_down_; }

    ProcessSnapshot snapshot(const config::App &);
};

} // namespace nexer

#endif // NEXER_PROCESS_MANAGER_H_
