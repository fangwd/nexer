#include <assert.h>

#include "helper.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"

namespace nexer {
namespace test {

// -- config parsing --

static void should_parse_auto_start_true() {
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true
        }]
    })conf";
    assert(Config::parse(config, code));
    auto *app = config.app("a");
    assert(app != nullptr);
    assert(app->auto_start == true);
}

static void should_parse_auto_start_false() {
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: false
        }]
    })conf";
    assert(Config::parse(config, code));
    auto *app = config.app("a");
    assert(app != nullptr);
    assert(app->auto_start == false);
}

static void should_default_auto_start_and_keep_alive() {
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] }
        }]
    })conf";
    assert(Config::parse(config, code));
    auto *app = config.app("a");
    assert(app != nullptr);
    assert(app->auto_start == false);
    assert(app->keep_alive == false);
}

static void should_parse_keep_alive() {
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            keep_alive: true
        }]
    })conf";
    assert(Config::parse(config, code));
    auto *app = config.app("a");
    assert(app != nullptr);
    assert(app->keep_alive == true);
}

static void should_derive_restart_policy_from_keep_alive() {
    Config config;
    const char *code = R"conf({
        apps: [
            { name: a, command: { file: x }, keep_alive: true },
            { name: b, command: { file: x } },
            { name: c, command: { file: x }, keep_alive: true,
              restart: { policy: never, delay_ms: 5, max_delay_ms: 50,
                         max_retries: 2, window_ms: 1000 } },
            { name: d, command: { file: x }, restart: { policy: always } }
        ]
    })conf";
    assert(Config::parse(config, code));
    auto &a = config.app("a")->restart;
    assert(a.policy == config::Restart::Policy::OnFailure);
    assert(a.delay_ms == 1000 && a.max_delay_ms == 30000);
    assert(a.max_retries == 5 && a.window_ms == 60000);
    assert(config.app("b")->restart.policy == config::Restart::Policy::Never);
    auto &c = config.app("c")->restart;
    assert(c.policy == config::Restart::Policy::Never);
    assert(c.delay_ms == 5 && c.max_delay_ms == 50);
    assert(c.max_retries == 2 && c.window_ms == 1000);
    assert(config.app("d")->restart.policy == config::Restart::Policy::Always);
}

static void should_reject_invalid_restart_config() {
    const char *bad[] = {
        R"conf({ apps: [{ name: a, command: { file: x }, restart: { policy: sometimes } }] })conf",
        R"conf({ apps: [{ name: a, command: { file: x }, restart: { delay_ms: -1 } }] })conf",
        R"conf({ apps: [{ name: a, command: { file: x }, restart: { delay_ms: 500, max_delay_ms: 100 } }] })conf",
        R"conf({ apps: [{ name: a, command: { file: x }, restart: { max_retries: -2 } }] })conf",
        R"conf({ apps: [{ name: a, command: { file: x }, restart: { window_ms: 0 } }] })conf",
        R"conf({ apps: [{ name: a, command: { file: x }, restart: { unknown: 1 } }] })conf",
    };
    for (auto *code : bad) {
        Config config;
        assert(!Config::parse(config, code));
    }
}

// -- auto_start behaviour --

static void should_auto_start_app() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true
        }]
    })conf";
    assert(Config::parse(config, code));

    SetScenario("simple");

    ProcessManager manager(loop);
    bool started = false;

    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [&](std::shared_ptr<Process>, int error) {
            assert(error == 0);
            started = true;
        });
    }

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto t = weak_timer.lock())
            t->close();
    });
    timer->start(false);

    loop.run();
    assert(started);
}

static void should_not_auto_start_without_flag() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] }
        }]
    })conf";
    assert(Config::parse(config, code));

    SetScenario("simple");

    ProcessManager manager(loop);
    bool started = false;

    manager.OnProcessStart([&](std::shared_ptr<Process>) { started = true; });

    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [&](std::shared_ptr<Process>, int) {});
    }

    auto timer = Timer::create(loop, 150);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto t = weak_timer.lock())
            t->close();
    });
    timer->start(false);

    loop.run();
    assert(!started);
}

static void should_not_double_start_with_proxy() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true,
            max_start_time: 300
        }],
        proxies: [{
            listen: 19500,
            upstream: { host: '127.0.0.1', port: 19501, app: a }
        }]
    })conf";
    assert(Config::parse(config, code));

    SetScenario("simple-with-sleep");

    ProcessManager manager(loop);
    int start_count = 0;
    int require_count = 0;

    manager.OnProcessStart([&](std::shared_ptr<Process> process) {
        if (process && process->name() == "a") {
            start_count++;
        }
    });

    // Auto-start (like nexer.cc does)
    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [&](std::shared_ptr<Process>, int error) {
            assert(error == 0);
            require_count++;
        });
    }

    // Proxy-triggered require (simulating an incoming connection)
    auto *upstream_app = config.proxies()[0].upstream.app;
    manager.Require(*upstream_app, [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        require_count++;
    });

    auto timer = Timer::create(loop, 400);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto t = weak_timer.lock())
            t->close();
    });
    timer->start(false);

    loop.run();

    // Process started exactly once, but both callbacks were invoked
    assert(start_count == 1);
    assert(require_count == 2);
}

static void should_handle_auto_start_failure() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true,
            max_start_time: 300
        }]
    })conf";
    assert(Config::parse(config, code));

    SetScenario("simple-fail");

    ProcessManager manager(loop);
    int callbacks = 0;
    int got_error = -1;

    // Without a checker a successful spawn means ready; the nonzero exit that
    // follows is observed as `failed` rather than reported to the callback.
    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [&](std::shared_ptr<Process>, int error) {
            callbacks++;
            got_error = error;
        });
    }

    manager.OnProcessData(
        [](std::shared_ptr<Process>, int, const char *, size_t) {});

    auto timer = Timer::create(loop, 250);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto t = weak_timer.lock())
            t->close();
    });
    timer->start(false);

    loop.run();
    assert(callbacks == 1);
    assert(got_error == 0);
    auto snapshot = manager.snapshot(*config.app("a"));
    assert(snapshot.observed == ObservedState::Failed);
    assert(snapshot.last_exit.present && snapshot.last_exit.code == 1);
    assert(snapshot.last_error == 1);
}

// -- keep_alive behaviour --

static void should_restart_keep_alive_app_on_exit() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true,
            keep_alive: true,
            restart: { delay_ms: 20 }
        }]
    })conf";
    assert(Config::parse(config, code));
    assert(config.app("a")->restart.policy ==
           config::Restart::Policy::OnFailure);

    // "keep-alive" scenario: step 0 exits immediately with code 1 (triggers
    // restart), step 1 sleeps 300ms then exits with 0 (no further restart).
    SetScenario("keep-alive");

    ProcessManager manager(loop);
    int start_count = 0;

    manager.OnProcessStart([&](std::shared_ptr<Process> process) {
        if (process && process->name() == "a") {
            start_count++;
        }
    });

    manager.OnProcessData(
        [](std::shared_ptr<Process>, int, const char *, size_t) {});

    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [](std::shared_ptr<Process>, int) {});
    }

    // Give enough time for the first run to exit and the restart to begin.
    auto timer = Timer::create(loop, 500);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([&, weak_timer] {
        manager.Shutdown();
        if (auto t = weak_timer.lock())
            t->close();
    });
    timer->start(false);

    loop.run();

    // Started at least twice: initial start + at least one restart.
    assert(start_count >= 2);
}

static void should_not_restart_without_keep_alive() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true
        }]
    })conf";
    assert(Config::parse(config, code));

    SetScenario("simple-fail");

    ProcessManager manager(loop);
    int start_count = 0;

    manager.OnProcessStart([&](std::shared_ptr<Process> process) {
        if (process && process->name() == "a") {
            start_count++;
        }
    });

    manager.OnProcessData(
        [](std::shared_ptr<Process>, int, const char *, size_t) {});

    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [](std::shared_ptr<Process>, int) {});
    }

    auto timer = Timer::create(loop, 300);
    auto weak_timer = std::weak_ptr<Timer>(timer);
    timer->on_tick([weak_timer] {
        if (auto t = weak_timer.lock())
            t->close();
    });
    timer->start(false);

    loop.run();

    // Started exactly once; no restart.
    assert(start_count == 1);
}

static void should_not_restart_keep_alive_during_shutdown() {
    EventLoop loop;
    Config config;
    const char *code = R"conf({
        apps: [{
            name: a,
            command: { file: ./build/run_tests, args: [helper, sleep], env: [NAME=a] },
            auto_start: true,
            keep_alive: true
        }]
    })conf";
    assert(Config::parse(config, code));

    // Process sleeps 300ms, long enough for us to shut down while it's running.
    SetScenario("simple-with-sleep");

    ProcessManager manager(loop);
    int start_count = 0;

    manager.OnProcessStart([&](std::shared_ptr<Process> process) {
        if (process && process->name() == "a") {
            start_count++;
        }
    });

    manager.OnProcessData(
        [](std::shared_ptr<Process>, int, const char *, size_t) {});

    for (auto *app : config.apps()) {
        if (!app || !app->auto_start)
            continue;
        manager.Require(*app, [](std::shared_ptr<Process>, int) {});
    }

    // Shut down quickly while the process is still running.
    auto shutdown_timer = Timer::create(loop, 50);
    auto weak_shutdown = std::weak_ptr<Timer>(shutdown_timer);
    shutdown_timer->on_tick([&, weak_shutdown] {
        manager.Shutdown();
        if (auto t = weak_shutdown.lock())
            t->close();
    });
    shutdown_timer->start(false);

    // Safety timer to stop the loop.
    auto safety = Timer::create(loop, 500);
    auto weak_safety = std::weak_ptr<Timer>(safety);
    safety->on_tick([weak_safety] {
        if (auto t = weak_safety.lock())
            t->close();
    });
    safety->start(false);

    loop.run();

    // Process started exactly once; shutdown must not trigger a restart.
    assert(start_count == 1);
}

void TestAutoStart() {
    should_parse_auto_start_true();
    should_parse_auto_start_false();
    should_default_auto_start_and_keep_alive();
    should_parse_keep_alive();
    should_derive_restart_policy_from_keep_alive();
    should_reject_invalid_restart_config();
    should_auto_start_app();
    should_not_auto_start_without_flag();
    should_not_double_start_with_proxy();
    should_handle_auto_start_failure();
    should_restart_keep_alive_app_on_exit();
    should_not_restart_without_keep_alive();
    should_not_restart_keep_alive_during_shutdown();
}

} // namespace test
} // namespace nexer
