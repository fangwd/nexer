#include <assert.h>

#include <errno.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "helper.h"
#include "nexer/core/logger.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"
#include "runner.h"
#include "uv.h"
#include <iostream>

namespace nexer {
namespace test {

std::string sample_config_no_checker = R"conf({
    proxies: [
      {
        listen: 19500,
        upstream: {
            host: '127.0.0.1',
            port: 19501,
            app: {
                name: a,
                command: {
                    file: ./build/run_tests,
                    args: [ helper, sleep ],
                    env: [ NAME=a ]
                }
                max_start_time: 300
            }
        }
      }
    ]
})conf";

static void should_start_process_without_checker() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_no_checker));

    SetScenario("simple-with-sleep");

    auto &app = *config.proxies()[0].upstream.app;

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    auto SKIP_STDERR = [&] {};

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == 0);
        assert(process); // process started immediately
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) {
        if (process->name() != "a")
            return;
        SKIP_STDERR();
        output.append(s, len);
    });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        // uv_stop(loop);
    });
    timer->start();

    loop.run();

    assert(output.size() > 0);
}

static void should_report_ready_on_spawn_without_checker() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_no_checker));

    SetScenario("simple");

    auto &app = *config.proxies()[0].upstream.app;

    ProcessManager manager(loop);

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == 0);
        assert(process); // a service without a checker is ready once spawned
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) {});

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        // uv_stop(loop);
    });
    timer->start();

    loop.run();
}

std::string sample_config_with_checker = R"conf({
    proxies: [
      {
        listen: 19500,
        upstream: {
            host: '127.0.0.1',
            port: 19501,
            app: {
                name: a,
                command: {
                    file: ./build/run_tests,
                    args: [ helper, sleep ],
                    env: [ NAME=a ]
                }
                checker: {
                    file: ./build/run_tests,
                    args: [ helper, sleep ],
                    env: [ NAME=b ]
                }
                max_start_time: 300
            }
        }
      }
    ]
})conf";

static void should_wait_for_checker() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_with_checker));

    SetScenario("simple-check");

    auto &app = *config.proxies()[0].upstream.app;

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    auto SKIP_STDERR = [&] {};

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == 0);
        assert(!process);
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) {
        if (process->name() == "checker") {
            return;
        }
        SKIP_STDERR();
        output.append(s, len);
    });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
    });
    timer->start();

    loop.run();

    assert(output.size() == 0);
}

static void should_start_process_if_checker_fails() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_with_checker));

    SetScenario("simple-fail");

    auto &app = *config.proxies()[0].upstream.app;

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    auto SKIP_STDERR = [&] {};

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == 1); // command exited 1 before the checker passed
        if (process) {
            process->on_data([&](int fd, const char *s, size_t len) {
                SKIP_STDERR();
                output.append(s, len);
            });
        }
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) {
        if (process->name() != "a")
            return;
        SKIP_STDERR();
        output.append(s, len);
    });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        // uv_stop(loop);
    });
    timer->start();

    loop.run();

    assert(output.size() > 0);
}

static void should_start_process_if_checker_crashes() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_with_checker));

    SetScenario("crash");

    auto &app = *config.proxies()[0].upstream.app;

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    auto SKIP_STDERR = [&] {};

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == 0); // checker eventually passes, so final error is 0
        if (process) {
            process->on_data([&](int fd, const char *s, size_t len) {
                SKIP_STDERR();
                output.append(s, len);
            });
        }
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) {
        if (process->name() != "a")
            return;
        PUSH_OUTPUT();
    });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        // uv_stop(loop);
    });
    timer->start();

    loop.run();

    assert(output.size() > 0);
}

#define SIGTERM 15

static void should_return_error_if_checker_fails_and_no_command() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_with_checker));

    SetScenario("simple-fail-check");

    auto &app = *config.proxies()[0].upstream.app;

    // Config objects are const pointers in ProcessManager usage, but for test
    // setup we can cast.
    auto *app_mutable = const_cast<config::App *>(&app);
    app_mutable->command.file = "";
    app_mutable->command.args.clear();
    app_mutable->command.env.clear();

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(
            error ==
            DEFAULT_EXIT_CODE); // simple-fail returns DEFAULT_EXIT_CODE (127)
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) { PUSH_OUTPUT(); });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
    });
    timer->start();

    loop.run();

    assert(output.size() > 0); // Checker output
}

static void should_invoke_checker_callback_once_on_spawn_failure() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_with_checker));

    auto &app = *config.proxies()[0].upstream.app;
    auto *app_mutable = const_cast<config::App *>(&app);
    if (app_mutable->checker) {
        app_mutable->checker->file = "./build/run_tests_DOES_NOT_EXIST";
        app_mutable->checker->args.clear();
        app_mutable->checker->env.clear();
    }
    app_mutable->command.file = "";
    app_mutable->command.args.clear();
    app_mutable->command.env.clear();

    ProcessManager manager(loop);
    int callback_count = 0;
    int last_error = 0;

    auto timer = Timer::create(loop, 300);
    auto weak_timer = std::weak_ptr<Timer>(timer);

    manager.Require(app, [&](std::shared_ptr<Process>, int error) {
        callback_count++;
        last_error = error;
        if (auto t = weak_timer.lock()) {
            t->close();
        }
    });

    timer->on_tick([weak_timer] {
        if (auto t = weak_timer.lock()) {
            t->close();
        }
    });
    timer->start(false);

    loop.run();

    assert(callback_count == 1);
    assert(last_error != 0);
}

std::string sample_config_preamble = R"conf({
    proxies: [
      {
        listen: 19500,
        upstream: {
            host: '127.0.0.1',
            port: 19501,
            app: {
                name: a,
                command: {
                    file: ./build/run_tests,
                    args: [ helper, sleep ],
                    env: [ NAME=a ]
                }
                max_start_time: 300
            }
        }
      }
    ],
    apps: [
        {
            name: 'preamble_app',
            checker: {
                file: ./build/run_tests,
                args: [ helper, sleep ],
                env: [ NAME=p ]
            }
            max_start_time: 300
        }
    ]
})conf";

static void should_wait_for_preamble() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_preamble));

    auto &app = *config.proxies()[0].upstream.app;
    // Find the preamble app in the apps list
    config::App *preamble = nullptr;
    for (auto *a : config.apps()) {
        if (a->name == "preamble_app") {
            preamble = a;
            break;
        }
    }
    assert(preamble != nullptr);

    // Manually link preamble
    auto *app_mutable = const_cast<config::App *>(&app);
    app_mutable->preamble.push_back(preamble);

    SetScenario("preamble-simple");

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == 0);
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) { PUSH_OUTPUT(); });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
    });
    timer->start();

    loop.run();

    // Preamble checker runs (output), then main app starts (output)
    assert(output.size() > 0);
}

static void should_fail_if_preamble_fails() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_preamble));

    auto &app = *config.proxies()[0].upstream.app;
    config::App *preamble = nullptr;
    for (auto *a : config.apps()) {
        if (a->name == "preamble_app") {
            preamble = a;
            break;
        }
    }
    assert(preamble != nullptr);

    auto *app_mutable = const_cast<config::App *>(&app);
    app_mutable->preamble.push_back(preamble);

    SetScenario("simple-fail-check");

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == DEFAULT_EXIT_CODE); // Preamble failed
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) { PUSH_OUTPUT(); });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
    });
    timer->start();

    loop.run();

    assert(output.size() > 0);
}

static void should_fail_if_preamble_crashes() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_preamble));

    auto &app = *config.proxies()[0].upstream.app;
    config::App *preamble = nullptr;
    for (auto *a : config.apps()) {
        if (a->name == "preamble_app") {
            preamble = a;
            break;
        }
    }
    assert(preamble != nullptr);

    auto *app_mutable = const_cast<config::App *>(&app);
    app_mutable->preamble.push_back(preamble);

    SetScenario("crash");

    ProcessManager manager(loop);
    std::string output;

    auto PUSH_OUTPUT = [&] { output += "1"; };

    manager.Require(app, [&](std::shared_ptr<Process> process, int error) {
        assert(error == SIGTERM); // Crash signal
    });

    manager.OnProcessData([&](std::shared_ptr<Process> process, int fd,
                              const char *s, size_t len) { PUSH_OUTPUT(); });

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
    });
    timer->start();

    loop.run();

    assert(output.size() > 0);
}

static void should_shutdown_reap_child_processes() {
    EventLoop loop;
    Config config;
    assert(Config::parse(config, sample_config_no_checker));

    SetScenario("simple-with-sleep");

    auto &app = *config.proxies()[0].upstream.app;

    ProcessManager manager(loop);

    int pid = 0;
    bool require_invoked = false;
    bool process_gone = false;

    manager.OnProcessStart([&](std::shared_ptr<Process> process) {
        if (process && process->name() == "a") {
            pid = process->pid();
        }
    });

    manager.Require(app, [&](std::shared_ptr<Process>, int error) {
        assert(error == 0 || error == UV_ECANCELED);
        require_invoked = true;
    });

    auto shutdown_timer = Timer::create(loop, 50);
    auto weak_shutdown = std::weak_ptr<Timer>(shutdown_timer);
    shutdown_timer->on_tick([&] {
        if (pid == 0) {
            return;
        }
        manager.Shutdown();
        if (auto timer = weak_shutdown.lock()) {
            timer->stop();
            timer->close();
        }
    });
    shutdown_timer->start();

    std::weak_ptr<Timer> weak_safety;

    auto poll_timer = Timer::create(loop, 50);
    auto weak_poll = std::weak_ptr<Timer>(poll_timer);
    poll_timer->on_tick([&] {
        if (pid == 0) {
            return;
        }
        if (kill(pid, 0) != 0) {
            int err = errno;
            if (err == ESRCH) {
                process_gone = true;
                if (auto timer = weak_poll.lock()) {
                    timer->stop();
                    timer->close();
                }
                if (auto safety = weak_safety.lock()) {
                    safety->stop();
                    safety->close();
                }
                uv_stop(loop);
            }
        }
    });
    poll_timer->start();

    auto safety_timer = Timer::create(loop, 400);
    weak_safety = std::weak_ptr<Timer>(safety_timer);
    safety_timer->on_tick([&] {
        if (auto safety = weak_safety.lock()) {
            safety->stop();
            safety->close();
        }
        uv_stop(loop);
    });
    safety_timer->start(false);

    loop.run();

    assert(require_invoked);
    assert(pid != 0);
    assert(process_gone);
}

// -- lifecycle authority --

namespace {

struct Lifecycle {
    EventLoop loop;
    Config config;
    std::unique_ptr<ProcessManager> manager;
    std::map<std::string, int> launches;
    std::map<std::string, std::string> output;
    std::vector<ProcessSnapshot> changes;

    Lifecycle(const char *code, const char *scenario) {
        assert(Config::parse(config, code));
        SetScenario(scenario);
        manager = std::make_unique<ProcessManager>(loop);
        manager->set_force_after_ms(200);
        manager->OnProcessStart([this](std::shared_ptr<Process> process) {
            if (process) {
                launches[process->name()]++;
            }
        });
        manager->OnProcessData([this](std::shared_ptr<Process> process, int,
                                      const char *data, size_t len) {
            if (process) {
                output[process->name()].append(data, len);
            }
        });
        manager->OnStateChange([this](const ProcessSnapshot &snapshot) {
            changes.push_back(snapshot);
        });
    }

    config::App &app(const char *name = "a") { return *config.app(name); }

    ProcessSnapshot snapshot(const char *name = "a") {
        return manager->snapshot(app(name));
    }

    // Runs the loop until `done` holds or the timeout passes.
    bool RunUntil(std::function<bool()> done, uint64_t timeout_ms = 3000) {
        bool satisfied = false;
        auto started = Timer::now();
        auto timer = Timer::create(loop, 5);
        std::weak_ptr<Timer> weak_timer = timer;
        timer->on_tick([&, weak_timer] {
            satisfied = done();
            if (satisfied || Timer::now() - started >= timeout_ms) {
                if (auto t = weak_timer.lock()) {
                    t->close();
                }
                loop.stop();
            }
        });
        timer->start();
        loop.run();
        return satisfied;
    }

    void RunFor(uint64_t ms) {
        auto started = Timer::now();
        RunUntil([&] { return Timer::now() - started >= ms; }, ms + 1000);
    }

    void Finish() {
        bool done = false;
        manager->Shutdown([&](bool clean) {
            assert(clean);
            done = true;
        });
        assert(RunUntil([&] { return done; }));
    }
};

struct TempMarker {
    std::string path;
    TempMarker() {
        static int counter = 0;
        path = (std::filesystem::temp_directory_path() /
                ("nexer-marker-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter++)))
                   .string();
        std::remove(path.c_str());
    }
    ~TempMarker() { std::remove(path.c_str()); }
    void Create() {
        FILE *f = fopen(path.c_str(), "w");
        assert(f);
        fclose(f);
    }
    bool Exists() const { return access(path.c_str(), F_OK) == 0; }
};

const char *kSleepApp = R"conf({
    apps: [{
        name: a,
        command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] }
    }]
})conf";

} // namespace

static void should_share_concurrent_requires() {
    Lifecycle t(kSleepApp, "simple-with-sleep");
    int callbacks = 0;
    for (int i = 0; i < 3; i++) {
        t.manager->Require(t.app(), [&](std::shared_ptr<Process> p, int error) {
            assert(error == 0 && p);
            callbacks++;
        });
    }
    assert(callbacks == 3);
    assert(t.launches["a"] == 1);
    assert(t.snapshot().observed == ObservedState::Ready);
    assert(t.snapshot().desired == DesiredState::Running);
    assert(t.snapshot().pid > 0);
    t.Finish();
    assert(t.snapshot().observed == ObservedState::Stopped);
    assert(t.snapshot().last_exit.present);
}

static void should_block_requires_after_deliberate_stop() {
    Lifecycle t(kSleepApp, "simple-with-sleep");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    int pid = t.snapshot().pid;
    assert(pid > 0);

    int stop_calls = 0;
    t.manager->Stop(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        stop_calls++;
    });
    assert(t.snapshot().observed == ObservedState::Stopping);
    assert(t.snapshot().desired == DesiredState::Stopped);
    assert(t.RunUntil([&] { return stop_calls == 1; }));
    auto stopped = t.snapshot();
    assert(stopped.observed == ObservedState::Stopped);
    assert(!stopped.ready);
    assert(stopped.pid == -1);
    assert(stopped.last_exit.present && stopped.last_exit.signal == SIGTERM);
    assert(kill(pid, 0) != 0);

    // Proxy-style requires cannot undo the stop.
    int require_error = 0;
    t.manager->Require(t.app(), [&](std::shared_ptr<Process> p, int error) {
        assert(!p);
        require_error = error;
    });
    assert(require_error == kErrorStopped);
    t.RunFor(50);
    assert(t.launches["a"] == 1);
    assert(t.snapshot().observed == ObservedState::Stopped);

    // An explicit start lifts it.
    int start_calls = 0;
    t.manager->Start(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        start_calls++;
    });
    assert(start_calls == 1);
    assert(t.launches["a"] == 2);
    assert(t.snapshot().desired == DesiredState::Running);
    t.Finish();
}

static void should_fail_require_when_dependency_is_stopped() {
    const char *code = R"conf({
        apps: [
            { name: p, command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=p] } },
            { name: a, command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
              preamble: [p] }
        ]
    })conf";
    Lifecycle t(code, "preamble-simple");
    t.manager->Stop(t.app("p"), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    int error = 0;
    t.manager->Require(t.app("a"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(error == kErrorStopped);
    assert(t.launches["a"] == 0 && t.launches["p"] == 0);
    assert(t.snapshot("a").observed == ObservedState::Failed);
    assert(t.snapshot("a").last_error == kErrorStopped);
    t.Finish();
}

static void should_restart_after_exit_and_while_running() {
    Lifecycle t(kSleepApp, "admin-restart");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    int first_pid = t.snapshot().pid;
    assert(first_pid > 0);

    int restarts = 0;
    t.manager->Restart(t.app(), [&](std::shared_ptr<Process> p, int error) {
        assert(error == 0 && p);
        restarts++;
    });
    assert(t.snapshot().observed == ObservedState::Stopping);
    assert(t.RunUntil([&] { return restarts == 1; }));
    assert(t.launches["a"] == 2);
    assert(t.snapshot().observed == ObservedState::Ready);
    assert(t.snapshot().pid > 0 && t.snapshot().pid != first_pid);
    assert(t.snapshot().launches == 2);
    t.Finish();

    // Restart also reruns a command that already exited zero.
    Lifecycle u(kSleepApp, "simple");
    u.manager->Require(u.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(u.RunUntil(
        [&] { return u.snapshot().observed == ObservedState::Completed; }));
    int calls = 0;
    u.manager->Restart(u.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        calls++;
    });
    assert(calls == 1);
    assert(u.launches["a"] == 2);
    u.Finish();
}

static void should_invoke_each_callback_once_across_repeated_operations() {
    Lifecycle t(kSleepApp, "simple-with-sleep");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    std::vector<int> results(4, -1000);
    std::vector<int> counts(4, 0);
    auto record = [&](int index) {
        return [&, index](std::shared_ptr<Process>, int error) {
            results[index] = error;
            counts[index]++;
        };
    };
    t.manager->Stop(t.app(), record(0));
    t.manager->Stop(t.app(), record(1));
    t.manager->Restart(t.app(), record(2));
    t.manager->Stop(t.app(), record(3));
    assert(results[2] == kErrorStopped);
    assert(t.RunUntil([&] { return counts[0] && counts[1] && counts[3]; }));
    for (int i = 0; i < 4; i++) {
        assert(counts[i] == 1);
    }
    assert(results[0] == 0 && results[1] == 0 && results[3] == 0);
    t.RunFor(50);
    assert(t.launches["a"] == 1);
    assert(t.snapshot().observed == ObservedState::Stopped);
    t.Finish();
}

static const char *kRetryApp = R"conf({
    apps: [{
        name: a,
        command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
        restart: { policy: on_failure, delay_ms: 10, max_delay_ms: 40,
                   max_retries: 2, window_ms: 60000 }
    }]
})conf";

static void should_retry_with_backoff_until_exhausted() {
    Lifecycle t(kRetryApp, "simple-fail");
    int calls = 0;
    t.manager->Require(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        calls++;
    });
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Failed; }));
    assert(calls == 1);
    assert(t.launches["a"] == 3);
    auto failed = t.snapshot();
    assert(failed.retries == 2);
    assert(failed.last_error != 0);
    bool saw_backoff = false;
    for (auto &change : t.changes) {
        if (change.observed == ObservedState::Backoff) {
            saw_backoff = true;
            assert(change.next_retry_at_ms > 0);
        }
    }
    assert(saw_backoff);

    assert(failed.last_error == kErrorRetriesExhausted);

    // Stays failed on its own; proxy-style requires cannot relaunch it.
    t.RunFor(100);
    assert(t.launches["a"] == 3);
    int require_error = 0;
    t.manager->Require(t.app(), [&](std::shared_ptr<Process>, int error) {
        require_error = error;
    });
    assert(require_error == kErrorRetriesExhausted);
    t.RunFor(50);
    assert(t.launches["a"] == 3);

    // An explicit start resets the budget.
    t.manager->Start(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.launches["a"] == 4);
    assert(t.snapshot().retries == 0);
    t.Finish();
}

static void should_stop_during_backoff_without_relaunch() {
    Lifecycle t(kRetryApp, "simple-fail");
    const_cast<config::App &>(t.app()).restart.delay_ms = 150;
    const_cast<config::App &>(t.app()).restart.max_delay_ms = 150;
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int) {});
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Backoff; }));
    int stop_error = -1;
    t.manager->Stop(t.app(), [&](std::shared_ptr<Process>, int error) {
        stop_error = error;
    });
    assert(stop_error == 0);
    assert(t.snapshot().observed == ObservedState::Stopped);
    assert(t.snapshot().next_retry_at_ms == 0);
    t.RunFor(300);
    assert(t.launches["a"] == 1);
    t.Finish();
}

static void should_report_readiness_timeout() {
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            checker: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=b] },
            max_start_time: 150
        }]
    })conf";
    Lifecycle t(code, "simple-with-sleep-fail");
    int error = 0;
    int calls = 0;
    t.manager->Require(t.app(), [&](std::shared_ptr<Process>, int e) {
        error = e;
        calls++;
    });
    assert(t.RunUntil([&] { return calls == 1; }));
    assert(error == kErrorTimeout);
    assert(t.snapshot().observed == ObservedState::Failed);
    assert(t.snapshot().last_error == kErrorTimeout);
    t.Finish();
}

static void should_reject_starts_during_shutdown() {
    Lifecycle t(kSleepApp, "simple-with-sleep");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    bool done = false;
    t.manager->Shutdown([&](bool clean) {
        assert(clean);
        done = true;
    });
    int error = 0;
    t.manager->Require(t.app(),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(error == kErrorShuttingDown);
    t.manager->Start(t.app(),
                     [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(error == kErrorShuttingDown);
    t.manager->Restart(t.app(),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(error == kErrorShuttingDown);
    assert(t.RunUntil([&] { return done; }));
    assert(t.launches["a"] == 1);
    assert(t.snapshot().observed == ObservedState::Stopped);
}

// -- process groups --

static std::string TreeConfig(const char *name, const char *flags,
                              const char *preamble = "") {
    std::string code = "{ apps: [{ name: ";
    code += name;
    code += ", command: { file: ./build/run_tests, args: [helper, tree";
    if (*flags) {
        code += ", ";
        code += flags;
    }
    code += "] }";
    if (*preamble) {
        code += ", preamble: [";
        code += preamble;
        code += "]";
    }
    code += " }] }";
    return code;
}

static int ChildPid(const std::string &output) {
    int leader = 0;
    int child = 0;
    if (sscanf(output.c_str(), "leader=%d child=%d", &leader, &child) != 2) {
        return 0;
    }
    return child;
}

static bool ProcessGone(int pid) { return kill(pid, 0) != 0 && errno == ESRCH; }

static void should_terminate_descendants_with_the_leader() {
    Lifecycle t(TreeConfig("a", "").c_str(), "simple");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil([&] { return ChildPid(t.output["a"]) != 0; }));
    int child = ChildPid(t.output["a"]);
    int leader = t.snapshot().pid;
    assert(leader > 0 && child > 0);

    int stopped = 0;
    t.manager->Stop(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        stopped++;
    });
    assert(t.RunUntil([&] { return stopped == 1; }));
    assert(t.RunUntil([&] { return ProcessGone(child); }));
    assert(ProcessGone(leader));
    assert(t.snapshot().observed == ObservedState::Stopped);
    t.Finish();
}

static void should_clean_descendants_after_leader_exits_early() {
    Lifecycle t(TreeConfig("a", "exit-early").c_str(), "simple");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Completed; }));
    int child = ChildPid(t.output["a"]);
    assert(child > 0);
    assert(t.RunUntil([&] { return ProcessGone(child); }));
    assert(t.snapshot().last_exit.code == 0);
    t.Finish();
}

static void should_escalate_to_sigkill_for_descendants_ignoring_term() {
    Lifecycle t(TreeConfig("a", "child-ignore-term").c_str(), "simple");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil([&] { return ChildPid(t.output["a"]) != 0; }));
    int child = ChildPid(t.output["a"]);

    auto started = Timer::now();
    int stopped = 0;
    t.manager->Stop(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        stopped++;
    });
    assert(t.RunUntil([&] { return stopped == 1; }));
    assert(Timer::now() - started >= 200);
    assert(t.RunUntil([&] { return ProcessGone(child); }));
    assert(t.snapshot().observed == ObservedState::Stopped);
    t.Finish();
}

static void should_shutdown_dependants_before_dependencies() {
    const char *code = R"conf({
        apps: [
            { name: p, command: { file: ./build/run_tests, args: [helper, tree] } },
            { name: a, command: { file: ./build/run_tests, args: [helper, tree] },
              preamble: [p] }
        ]
    })conf";
    Lifecycle t(code, "simple");
    t.manager->Require(t.app("a"), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.snapshot("a").pid > 0 && t.snapshot("p").pid > 0);
    t.changes.clear();

    bool done = false;
    bool clean = false;
    t.manager->Shutdown([&](bool ok) {
        clean = ok;
        done = true;
    });
    assert(t.RunUntil([&] { return done; }));
    assert(clean);

    size_t a_stopped = 0;
    size_t p_stopping = 0;
    for (size_t i = 0; i < t.changes.size(); i++) {
        auto &change = t.changes[i];
        if (change.app->name == "a" &&
            change.observed == ObservedState::Stopped && !a_stopped) {
            a_stopped = i + 1;
        }
        if (change.app->name == "p" &&
            change.observed == ObservedState::Stopping && !p_stopping) {
            p_stopping = i + 1;
        }
    }
    assert(a_stopped && p_stopping && a_stopped < p_stopping);
    assert(t.snapshot("a").observed == ObservedState::Stopped);
    assert(t.snapshot("p").observed == ObservedState::Stopped);
}

// -- tasks and external resources --

static void should_complete_tasks_and_not_rerun_them_for_dependants() {
    const char *code = R"conf({
        apps: [
            { name: m, type: task,
              command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] } },
            { name: a, command: { file: ./build/run_tests, args: [helper, tree] },
              preamble: [m] },
            { name: b, command: { file: ./build/run_tests, args: [helper, tree] },
              preamble: [m] }
        ]
    })conf";
    Lifecycle t(code, "task-rerun");
    int task_calls = 0;
    t.manager->Require(t.app("m"), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        task_calls++;
    });
    assert(task_calls == 0); // success is the zero exit, not the spawn
    assert(t.snapshot("m").observed == ObservedState::Starting);
    assert(t.snapshot("m").pid > 0);

    // Two dependants share the single run.
    int deps = 0;
    for (auto *name : {"a", "b"}) {
        t.manager->Require(t.app(name), [&](std::shared_ptr<Process>, int e) {
            assert(e == 0);
            deps++;
        });
    }
    assert(t.RunUntil([&] { return deps == 2; }));
    assert(task_calls == 1);
    assert(t.launches["m"] == 1);
    assert(t.snapshot("m").observed == ObservedState::Completed);

    // Later requires (including dependant restarts) are satisfied at once.
    int again = -1;
    t.manager->Require(t.app("m"),
                       [&](std::shared_ptr<Process>, int e) { again = e; });
    assert(again == 0);
    int restarted = 0;
    t.manager->Restart(t.app("a"), [&](std::shared_ptr<Process>, int e) {
        assert(e == 0);
        restarted++;
    });
    assert(t.RunUntil([&] { return restarted == 1; }));
    assert(t.launches["m"] == 1);

    // Only an explicit start/restart reruns it.
    int rerun = 0;
    t.manager->Start(t.app("m"), [&](std::shared_ptr<Process>, int e) {
        assert(e == 0);
        rerun++;
    });
    assert(t.RunUntil([&] { return rerun == 1; }));
    assert(t.launches["m"] == 2);
    t.Finish();
}

static void should_fail_tasks_on_nonzero_exit_and_cancel_on_stop() {
    const char *code = R"conf({
        apps: [{ name: m, type: task,
                 command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] } }]
    })conf";
    Lifecycle t(code, "simple-fail");
    int error = -1;
    t.manager->Require(t.app("m"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(error == 1);
    assert(t.snapshot("m").observed == ObservedState::Failed);
    assert(t.launches["m"] == 1);

    Lifecycle u(TreeConfig("m", "", "").c_str(), "simple");
    const_cast<config::App &>(u.app("m")).type = config::App::Type::Task;
    int calls = 0;
    u.manager->Require(u.app("m"), [&](std::shared_ptr<Process>, int e) {
        assert(e == kErrorStopped);
        calls++;
    });
    assert(u.RunUntil([&] { return ChildPid(u.output["m"]) != 0; }));
    int stopped = 0;
    u.manager->Stop(u.app("m"), [&](std::shared_ptr<Process>, int e) {
        assert(e == 0);
        stopped++;
    });
    assert(calls == 1);
    assert(u.RunUntil([&] { return stopped == 1; }));
    assert(u.snapshot("m").observed == ObservedState::Stopped);
    u.Finish();
}

static std::string ExternalConfig(const std::string &marker, bool launcher,
                                  bool stop_command,
                                  const char *stop_exit = "0") {
    std::string code = "{ apps: [{ name: e, type: external, checker: { file: "
                       "sh, args: [-c, 'test -f " +
                       marker + "'] }";
    if (launcher) {
        code += ", command: { file: sh, args: [-c, 'touch " + marker + "'] }";
    }
    if (stop_command) {
        code += ", stop_command: { file: sh, args: [-c, 'rm -f " + marker +
                "; exit " + stop_exit + "'] }";
    }
    code += " }] }";
    return code;
}

static void should_treat_available_external_resource_as_not_owned() {
    TempMarker marker;
    marker.Create();
    Lifecycle t(ExternalConfig(marker.path, true, true).c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(error == 0);
    auto ready = t.snapshot("e");
    assert(ready.observed == ObservedState::Ready && !ready.acquired);
    assert(t.launches.count("e") == 0);

    int restart = -1;
    t.manager->Restart(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { restart = e; });
    assert(restart == kErrorNotAcquired);
    assert(t.snapshot("e").observed == ObservedState::Ready);

    int stopped = -1;
    t.manager->Stop(t.app("e"),
                    [&](std::shared_ptr<Process>, int e) { stopped = e; });
    assert(stopped == 0);
    assert(t.snapshot("e").observed == ObservedState::Stopped);
    assert(!t.snapshot("e").ready);
    assert(marker.Exists()); // the stop_command did not run
    t.Finish();
    assert(marker.Exists());
}

static void should_acquire_launched_external_resource_and_stop_it() {
    TempMarker marker;
    Lifecycle t(ExternalConfig(marker.path, true, true).c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(error == 0);
    assert(marker.Exists());
    auto ready = t.snapshot("e");
    assert(ready.observed == ObservedState::Ready && ready.acquired);
    assert(ready.pid == -1); // the launcher already exited

    int restart = -1;
    t.manager->Restart(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { restart = e; });
    assert(t.RunUntil([&] { return restart != -1; }));
    assert(restart == 0);
    assert(t.snapshot("e").acquired);

    int stopped = -1;
    t.manager->Stop(t.app("e"),
                    [&](std::shared_ptr<Process>, int e) { stopped = e; });
    assert(t.snapshot("e").observed == ObservedState::Stopping);
    assert(t.RunUntil([&] { return stopped != -1; }));
    assert(stopped == 0);
    assert(!marker.Exists());
    auto done = t.snapshot("e");
    assert(done.observed == ObservedState::Stopped && !done.acquired);
    t.Finish();
}

static void should_surface_stop_command_failures() {
    TempMarker marker;
    Lifecycle t(ExternalConfig(marker.path, true, true, "3").c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(t.snapshot("e").acquired);

    int stopped = -1;
    t.manager->Stop(t.app("e"),
                    [&](std::shared_ptr<Process>, int e) { stopped = e; });
    assert(t.RunUntil([&] { return stopped != -1; }));
    assert(stopped == 3);
    assert(t.snapshot("e").observed == ObservedState::Failed);
    assert(t.snapshot("e").last_error == 3);

    bool done = false;
    bool clean = true;
    t.manager->Shutdown([&](bool ok) {
        clean = ok;
        done = true;
    });
    assert(t.RunUntil([&] { return done; }));
    assert(!clean);
}

static void should_run_stop_command_at_shutdown_only_when_acquired() {
    TempMarker acquired_marker;
    TempMarker available_marker;
    available_marker.Create();
    auto external = [](const char *name, const std::string &marker) {
        return "{ name: " + std::string(name) +
               ", type: external, checker: { file: sh, args: [-c, 'test -f " +
               marker + "'] }, command: { file: sh, args: [-c, 'touch " +
               marker + "'] }, stop_command: { file: sh, args: [-c, 'rm -f " +
               marker + "'] } }";
    };
    std::string code = "{ apps: [" + external("x", acquired_marker.path) +
                       ", " + external("e", available_marker.path) + "] }";
    Lifecycle t(code.c_str(), "simple");
    int done_requires = 0;
    for (auto *name : {"x", "e"}) {
        t.manager->Require(t.app(name), [&](std::shared_ptr<Process>, int e) {
            assert(e == 0);
            done_requires++;
        });
    }
    assert(t.RunUntil([&] { return done_requires == 2; }));
    assert(t.snapshot("x").acquired && !t.snapshot("e").acquired);

    bool done = false;
    t.manager->Shutdown([&](bool clean) {
        assert(clean);
        done = true;
    });
    assert(t.RunUntil([&] { return done; }));
    assert(!acquired_marker.Exists());
    assert(available_marker.Exists());
    assert(t.snapshot("x").observed == ObservedState::Stopped);
    assert(t.snapshot("e").observed == ObservedState::Stopped);
}

static void should_report_external_resource_without_stop_command() {
    TempMarker marker;
    Lifecycle t(ExternalConfig(marker.path, true, false).c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(t.snapshot("e").acquired);
    int restart = -1;
    t.manager->Restart(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { restart = e; });
    assert(restart == kErrorNoStopCommand);
    int stopped = -1;
    t.manager->Stop(t.app("e"),
                    [&](std::shared_ptr<Process>, int e) { stopped = e; });
    assert(stopped == 0);
    assert(marker.Exists());
    assert(t.snapshot("e").observed == ObservedState::Stopped);
    t.Finish();
    assert(marker.Exists());
}

static void should_fail_external_resource_when_launcher_fails() {
    TempMarker marker;
    std::string code = "{ apps: [{ name: e, type: external, checker: { file: "
                       "sh, args: [-c, 'test -f " +
                       marker.path +
                       "'] }, command: { file: sh, args: [-c, 'exit 7'] } }] }";
    Lifecycle t(code.c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(error == 7);
    assert(t.snapshot("e").observed == ObservedState::Failed);
    assert(!t.snapshot("e").acquired);
    t.Finish();
}

// -- output files --

static std::string ReadFile(const std::string &path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void should_write_and_rotate_output_files() {
    TempMarker out;
    TempMarker err;
    std::string code = "{ apps: [{ name: a, command: { file: "
                       "./build/run_tests, args: [helper, "
                       "lines] }, logs: { stdout_file: '" +
                       out.path + "', stderr_file: '" + err.path +
                       "', max_bytes: 40, max_files: 2 } }] }";
    auto check_bounded = [&] {
        // Current file plus at most max_files rotated copies, each within
        // max_bytes, and the newest line in the current file.
        int files = 0;
        for (auto suffix : {"", ".1", ".2"}) {
            auto path = out.path + suffix;
            if (access(path.c_str(), F_OK) == 0) {
                files++;
                assert(ReadFile(path).size() <= 40);
            }
        }
        assert(files == 3);
        assert(access((out.path + ".3").c_str(), F_OK) != 0);
        auto current = ReadFile(out.path);
        assert(current.size() >= 8);
        assert(current.compare(current.size() - 8, 8, "line-20\n") == 0);
    };

    // The helper writes 20 lines of 8 bytes in separate writes.
    Lifecycle t(code.c_str(), "simple");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Completed; }));
    assert(t.snapshot().output_error.empty());
    check_bounded();
    assert(access(err.path.c_str(), F_OK) != 0); // nothing written to stderr

    // A restart appends to the current file and rotation stays bounded.
    int restarted = 0;
    t.manager->Restart(t.app(), [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        restarted++;
    });
    assert(t.RunUntil([&] {
        return restarted == 1 &&
               t.snapshot().observed == ObservedState::Completed;
    }));
    check_bounded();
    assert(t.launches["a"] == 2);
    t.Finish();
    for (auto suffix : {".1", ".2"}) {
        std::remove((out.path + suffix).c_str());
    }
}

static void should_surface_output_disk_failures() {
    std::string code = "{ apps: [{ name: a, command: { file: "
                       "./build/run_tests, args: [helper, "
                       "sleep], env: [NAME=a] }, logs: { stdout_file: "
                       "'/nonexistent-dir/nexer/out.log' } }] }";
    Lifecycle t(code.c_str(), "simple");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Completed; }));
    assert(t.output["a"].find("a") != std::string::npos); // tail still fed
    auto error = t.snapshot().output_error;
    assert(error.find("/nonexistent-dir/nexer/out.log") != std::string::npos);
    assert(error.find("cannot open") != std::string::npos);
    t.Finish();
}

static void should_fail_when_always_policy_exhausts_on_zero_exit() {
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            restart: { policy: always, delay_ms: 10, max_retries: 1 }
        }]
    })conf";
    Lifecycle t(code, "task-rerun");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Failed; }));
    assert(t.launches["a"] == 2);
    assert(t.snapshot().last_error == kErrorRetriesExhausted);
    assert(t.snapshot().last_exit.code == 0);
    t.Finish();
}

static void should_escalate_stop_command_ignoring_term() {
    TempMarker marker;
    std::string code =
        "{ apps: [{ name: e, type: external, checker: { file: sh, args: [-c, "
        "'test -f " +
        marker.path +
        "'] }, command: { file: sh, args: [-c, "
        "'touch " +
        marker.path +
        "'] }, stop_command: { file: sh, args: [-c, "
        "'trap \"\" TERM; sleep 30'] } }] }";
    Lifecycle t(code.c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(t.snapshot("e").acquired);

    auto started = Timer::now();
    int stopped = -1;
    t.manager->Stop(t.app("e"),
                    [&](std::shared_ptr<Process>, int e) { stopped = e; });
    assert(t.RunUntil([&] { return stopped != -1; }));
    auto elapsed = Timer::now() - started;
    assert(elapsed >= 200 && elapsed < 3000);
    assert(stopped == SIGKILL);
    assert(t.snapshot("e").observed == ObservedState::Failed);
    bool done = false;
    bool clean = true;
    t.manager->Shutdown([&](bool ok) {
        clean = ok;
        done = true;
    });
    assert(t.RunUntil([&] { return done; }));
    assert(!clean);
}

static void should_clean_descendants_of_stop_command() {
    TempMarker marker;
    std::string code =
        "{ apps: [{ name: e, type: external, checker: { file: sh, args: [-c, "
        "'test -f " +
        marker.path +
        "'] }, command: { file: sh, args: [-c, "
        "'touch " +
        marker.path +
        "'] }, stop_command: { file: sh, args: [-c, "
        "'sleep 30 & echo child=$!; rm -f " +
        marker.path + "'] } }] }";
    Lifecycle t(code.c_str(), "simple");
    int error = -1;
    t.manager->Require(t.app("e"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));

    int stopped = -1;
    t.manager->Stop(t.app("e"),
                    [&](std::shared_ptr<Process>, int e) { stopped = e; });
    assert(t.RunUntil([&] { return stopped != -1; }));
    assert(stopped == 0);
    int child = 0;
    sscanf(t.output["stop"].c_str(), "child=%d", &child);
    assert(child > 0);
    assert(t.RunUntil([&] { return ProcessGone(child); }));
    assert(!marker.Exists());
    bool done = false;
    t.manager->Shutdown([&](bool clean) {
        assert(clean);
        done = true;
    });
    assert(t.RunUntil([&] { return done; }));
}

static void should_enforce_max_bytes_on_large_and_resumed_writes() {
    TempMarker out;
    {
        std::ofstream pre(out.path);
        pre << std::string(90, 'y');
    }
    std::string code =
        "{ apps: [{ name: a, type: task, command: { file: sh, args: [-c, "
        "'printf \"%0250d\" 0'] }, logs: { stdout_file: '" +
        out.path + "', max_bytes: 100, max_files: 3 } }] }";
    Lifecycle t(code.c_str(), "simple");
    t.manager->Require(t.app(), [](std::shared_ptr<Process>, int error) {
        assert(error == 0);
    });
    assert(t.RunUntil(
        [&] { return t.snapshot().observed == ObservedState::Completed; }));
    // The existing 90 bytes rotate out first, then 250 bytes split 100/100/50.
    assert(ReadFile(out.path).size() == 50);
    assert(ReadFile(out.path + ".1").size() == 100);
    assert(ReadFile(out.path + ".2").size() == 100);
    assert(ReadFile(out.path + ".3") == std::string(90, 'y'));
    t.Finish();
    for (auto suffix : {".1", ".2", ".3"}) {
        std::remove((out.path + suffix).c_str());
    }
}

static std::string CheckerOnlyConfig(const char *script) {
    return std::string(
               "{ apps: [{ name: c, checker: { file: sh, args: [-c, '") +
           script + "'] } }] }";
}

static void should_clean_checker_descendants() {
    Lifecycle t(CheckerOnlyConfig("sleep 30 & echo child=$!; exit 0").c_str(),
                "simple");
    int error = -1;
    t.manager->Require(t.app("c"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    assert(error == 0 && t.snapshot("c").observed == ObservedState::Ready);
    int child = 0;
    sscanf(t.output["checker"].c_str(), "child=%d", &child);
    assert(child > 0);
    assert(t.RunUntil([&] { return ProcessGone(child); }));
    t.Finish();
}

static void should_wait_for_checker_descendants_at_shutdown() {
    // The grandchild ignores SIGTERM, so cleanup needs the SIGKILL escalation.
    Lifecycle t(
        CheckerOnlyConfig("(trap \"\" TERM; sleep 30) & echo child=$!; exit 0")
            .c_str(),
        "simple");
    int error = -1;
    t.manager->Require(t.app("c"),
                       [&](std::shared_ptr<Process>, int e) { error = e; });
    assert(t.RunUntil([&] { return error != -1; }));
    int child = 0;
    sscanf(t.output["checker"].c_str(), "child=%d", &child);
    assert(child > 0);
    assert(!ProcessGone(child));

    auto started = Timer::now();
    bool done = false;
    bool clean = false;
    t.manager->Shutdown([&](bool ok) {
        clean = ok;
        done = true;
        assert(ProcessGone(child));
    });
    assert(!done); // waits for the reaped group
    assert(t.RunUntil([&] { return done; }));
    assert(clean);
    assert(Timer::now() - started >= 200);
}

void TestProcessManager() {
    should_clean_checker_descendants();
    should_wait_for_checker_descendants_at_shutdown();
    should_fail_when_always_policy_exhausts_on_zero_exit();
    should_escalate_stop_command_ignoring_term();
    should_clean_descendants_of_stop_command();
    should_enforce_max_bytes_on_large_and_resumed_writes();
    should_write_and_rotate_output_files();
    should_surface_output_disk_failures();
    should_complete_tasks_and_not_rerun_them_for_dependants();
    should_fail_tasks_on_nonzero_exit_and_cancel_on_stop();
    should_treat_available_external_resource_as_not_owned();
    should_acquire_launched_external_resource_and_stop_it();
    should_surface_stop_command_failures();
    should_run_stop_command_at_shutdown_only_when_acquired();
    should_report_external_resource_without_stop_command();
    should_fail_external_resource_when_launcher_fails();
    should_terminate_descendants_with_the_leader();
    should_clean_descendants_after_leader_exits_early();
    should_escalate_to_sigkill_for_descendants_ignoring_term();
    should_shutdown_dependants_before_dependencies();
    should_share_concurrent_requires();
    should_block_requires_after_deliberate_stop();
    should_fail_require_when_dependency_is_stopped();
    should_restart_after_exit_and_while_running();
    should_invoke_each_callback_once_across_repeated_operations();
    should_retry_with_backoff_until_exhausted();
    should_stop_during_backoff_without_relaunch();
    should_report_readiness_timeout();
    should_reject_starts_during_shutdown();
    should_start_process_without_checker();
    should_report_ready_on_spawn_without_checker();
    should_wait_for_checker();
    should_start_process_if_checker_fails();
    should_start_process_if_checker_crashes();
    should_return_error_if_checker_fails_and_no_command();
    should_invoke_checker_callback_once_on_spawn_failure();
    should_wait_for_preamble();
    should_fail_if_preamble_fails();
    should_fail_if_preamble_crashes();
    should_shutdown_reap_child_processes();
}

} // namespace test
} // namespace nexer
