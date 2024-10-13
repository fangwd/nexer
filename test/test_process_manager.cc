#include <assert.h>

#include <algorithm>

#include "process_manager.h"
#include "string_buffer.h"
#include "runner.h"
#include "helper.h"
#include "logger.h"
#include <iostream>
#include "timer.h"

// gcov writes on stderr
#define SKIP_STDERR() if (fd != 1) return
#define PUSH_OUTPUT()                                     \
    do {                                                  \
        if (fd == 1) {                                    \
            std::string line = Trim(std::string(s, len)); \
            if (line.size() > 0) {                        \
                output.push_back(std::move(line));        \
            }                                             \
        }                                                 \
    } while (0)

#define AssertSortEqual(x, y)                             \
    do {                                                  \
        std::sort(x.begin(), x.end());                    \
        std::sort(y.begin(), y.end());                    \
        if (x != y) {                                     \
            std::cerr << "Expected: " << Join(x) << '\n'; \
            std::cerr << "  Actual: " << Join(y) << '\n'; \
            assert(0);                                    \
        }                                                 \
    } while (0)

namespace nexer {
namespace test {

std::string Trim(std::string str);
static std::string Join(const std::vector<std::string>& strings);

static void TestSimple() {
    SetScenario("simple");

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App app = {
        .command = {
            .file = exename,
            .args = std::vector<std::string>({"helper", "sleep"}),
            .env = std::vector<std::string>({"NAME=a", "FOO=bar"})
        },
    };
    std::string output;
    manager.Require(app, [&](Process* process, int error) {
        assert(error == 0);
        assert(!process); // timer takes 100ms and app is not sleeping
    });
    manager.OnProcessData([&](const Process*, int fd, const char *s, size_t len) {
        SKIP_STDERR();
        output.append(s, len);
    });
    loop.Run();
    assert(output == "a\n");
}

static void TestSimpleBad() {
    SetScenario("simple");

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App app = {
        .command = {
            .file = "bad-command",
            .args = std::vector<std::string>({"helper", "sleep"}),
            .env = std::vector<std::string>({"NAME=a"})
        },
    };
    std::string output;
    manager.Require(app, [&](Process* process, int error) {
        assert(error == -ENOENT);
        assert(!process);
    });
    manager.OnProcessData([&](const Process*, int fd, const char *s, size_t len) {
        SKIP_STDERR();
        output.append(s, len);
    });
    loop.Run();
    assert(output == "");
}

static void TestSimpleWithSleep() {
    SetScenario("simple-with-sleep");

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App app = {
        .command = {
            .file = exename,
            .args = std::vector<std::string>({"helper", "sleep"}),
            .env = std::vector<std::string>({"NAME=a"})
        },
    };
    std::string output;
    manager.Require(app, [&](Process* process, int error) {
        assert(error == 0);
        process->OnData([&](int fd, const char *s, size_t len) {
            SKIP_STDERR();
            output.append(s, len);
        });
    });
    // This shouldn't cause app to run twice
    manager.Require(app, [&](Process* process, int error) {});
    manager.OnProcessData([&](const Process*, int fd, const char *s, size_t len) {
        SKIP_STDERR();
        output.append(s, len);
    });
    loop.Run();
    assert(output == "a\na\n");
}

static void TestChecker() {
    Config config;
    const std::string code = R"conf({
        apps: [
            {
                name: a,
                command: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=b ]
                }
                max_start_time: 1000
            }
        ]
    })conf";
    assert(Config::Parse(config, code));

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");

        SetScenario("check");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == 0);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        std::vector<std::string> expected({"a", "b", "b"});
        AssertSortEqual(output, expected);
    }

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");

        SetScenario("check-fail");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == DEFAULT_EXIT_CODE);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        // default exit code is 2 and message ""
        std::vector<std::string> expected({"a", "b"});
        AssertSortEqual(expected, output);
    }

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");

        SetScenario("check-retry");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == 0);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        std::vector<std::string> expected({"a", "b", "b", "b"});
        AssertSortEqual(expected, output);
    }

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");

        SetScenario("check-retry-failed");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == DEFAULT_EXIT_CODE);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        std::vector<std::string> expected({"a", "b", "b", "b"});
        AssertSortEqual(expected, output);
    }

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");
        auto timeout = app.checker->timeout;

        // checker sleeps 500ms
        app.checker->timeout = 200;

        SetScenario("check-timeout");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == SIGTERM);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        std::vector<std::string> expected({ "a" });
        AssertSortEqual(expected, output);

        app.checker->timeout = timeout;
    }

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");
        auto file = app.checker->file;

        app.checker->file = "bad-command";

        SetScenario("check-error");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == -ENOENT);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        std::vector<std::string> expected({ "a" });
        AssertSortEqual(expected, output);

        app.checker->file = file;
    }

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");

        SetScenario("check-no-start");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            assert(error == 0);
        });

        manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        std::vector<std::string> expected({ "b" });
        AssertSortEqual(expected, output);
    }

}

static void TestCheckerKill() {
    Config config;
    const std::string code = R"conf({
        apps: [
            {
                name: a,
                command: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=b ]
                }
                max_start_time: 1000
            }
        ]
    })conf";
    assert(Config::Parse(config, code));

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App& app = *config.GetApp("a");

    SetScenario("check-kill");

    std::vector<std::string> output;

    manager.Require(app, [&](Process* process, int error) {
        assert(error == 0);
    });

    auto& timer = Timer::Create(loop, 500);
    timer.OnTick([&]() {
        manager.Require(app, [&](Process* process, int error) {
            assert(error == 0);
        });
        timer.Close();
    });
    timer.Start();

    manager.OnProcessData([&](const Process*, int fd, const char* s, size_t len) {
        PUSH_OUTPUT();
    });

    loop.Run();

    std::vector<std::string> expected({"a", "b", "b", "b", "b", "b"});
    AssertSortEqual(expected, output);
}

static void TestPreambleSimple() {
    Config config;
    const std::string code = R"conf({
        apps: [
            {
                name: a,
                command: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=b ]
                }
                preamble: [
                    {
                        name: p,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p ]
                        }
                    },
                ]
            },
        ]
    })conf";
    assert(Config::Parse(config, code));

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App& app = *config.GetApp("a");

    SetScenario("preamble-simple");

    std::vector<std::string> output;

    manager.Require(app, [&](Process* process, int error) {
        assert(error == 0);
    });

    manager.OnProcessData( [&](const Process*, int fd, const char* s, size_t len) {
        PUSH_OUTPUT();
    });

    loop.Run();

    std::vector<std::string> expected({"a", "b", "b", "p"});
    AssertSortEqual(expected, output);
}

static void TestPreambleMulti() {
    Config config;
    const std::string code = R"conf({
        apps: [
            {
                name: a,
                command: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=b ]
                }
                preamble: [
                    {
                        name: p1,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p1 ]
                        }
                    },
                    {
                        name: p2,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p2 ]
                        }
                    },
                ]
            },
        ]
    })conf";
    assert(Config::Parse(config, code));

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App& app = *config.GetApp("a");

    SetScenario("preamble-multi");

    std::vector<std::string> output;

    manager.Require(app, [&](Process* process, int error) {
        assert(error == 0);
    });

    manager.OnProcessData( [&](const Process*, int fd, const char* s, size_t len) {
        PUSH_OUTPUT();
    });

    loop.Run();

    std::vector<std::string> expected({"a", "b", "b", "p1", "p2"});
    AssertSortEqual(expected, output);
}

static void TestPreambleWithChecker() {
    Config config;
    const std::string code = R"conf({
        apps: [
            {
                name: a,
                command: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=b ]
                }
                max_start_time: 2000
                preamble: [
                    {
                        name: p1,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p1 ]
                        }
                        checker: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=b1 ]
                        }
                    },
                    {
                        name: p2,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p2 ]
                        }
                        checker: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=b2 ]
                        }
                     },
                ]
            },
        ]
    })conf";
    assert(Config::Parse(config, code));

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App& app = *config.GetApp("a");

    SetScenario("preamble-with-checker");

    std::vector<std::string> output;

    manager.Require(app, [&](Process* process, int error) {
        if (error) {
            log_critical("App started with error %d", error);
            assert(0);
        }
    });

    manager.OnProcessData( [&](const Process*, int fd, const char* s, size_t len) {
        PUSH_OUTPUT();
    });

    loop.Run();

    std::vector<std::string> expected({"a", "b", "b", "b1", "b1", "b2", "b2", "b2", "p1", "p2"});
    AssertSortEqual(expected, output);

    {
        nexer::EventLoop loop;
        nexer::ProcessManager manager(loop);
        config::App& app = *config.GetApp("a");
        std::string file = app.preamble[0]->command.file;

        ((config::App*)app.preamble[0])->command.file = "bad-command";

        SetScenario("preamble-with-checker");

        std::vector<std::string> output;

        manager.Require(app, [&](Process* process, int error) {
            // 1 preamble failed
            assert(error == 1);
        });

        manager.OnProcessData( [&](const Process*, int fd, const char* s, size_t len) {
            PUSH_OUTPUT();
        });

        loop.Run();

        // a shouldn't have been started, b and b1 should have been executed once
        std::vector<std::string> expected({"b", "b1", "b2", "b2", "b2", "p2"});
        AssertSortEqual(expected, output);

        ((config::App*)app.preamble[0])->command.file = file;
    }
}

static void TestPreambleFailure() {
    Config config;
    const std::string code = R"conf({
        apps: [
            {
                name: a,
                command: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test
                    args: [ helper, sleep ]
                    env:  [ NAME=b ]
                }
                preamble: [
                    {
                        name: p1,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p1 ]
                        }
                        checker: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=b1 ]
                        }
                        preamble: [
                            {
                                name: p3,
                                command: {
                                    file: ./build/run_test
                                    args: [ helper, sleep ]
                                    env:  [ NAME=p3 ]
                                }
                                checker: {
                                    file: ./build/run_test
                                    args: [ helper, sleep ]
                                    env:  [ NAME=b3 ]
                                }
                                max_start_time: 500
                            }
                        ]
                    },
                    {
                        name: p2,
                        command: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=p2 ]
                        }
                        checker: {
                            file: ./build/run_test
                            args: [ helper, sleep ]
                            env:  [ NAME=b2 ]
                        }
                     },
                ]
            },
        ]
    })conf";
    assert(Config::Parse(config, code));

    nexer::EventLoop loop;
    nexer::ProcessManager manager(loop);
    config::App& app = *config.GetApp("a");

    SetScenario("preamble-failure");

    std::vector<std::string> output;

    manager.Require(app, [&](Process* process, int error) {
        assert(error == 1);
    });

    manager.OnProcessData( [&](const Process*, int fd, const char* s, size_t len) {
        PUSH_OUTPUT();
    });

    loop.Run();

    // a and p1 shouldn't have been started, b and b1 should have been executed once
    std::vector<std::string> expected({"b", "b1", "b2", "b2", "b3", "b3", "b3", "p2", "p3"});
    AssertSortEqual(expected, output);
}

static void TestPreamble() {
    TestPreambleSimple();
    TestPreambleMulti();
    TestPreambleWithChecker();
    TestPreambleFailure();
}

void TestProcessManager() {
    TestSimple();
    TestSimpleBad();
    TestSimpleWithSleep();
    TestChecker();
    TestCheckerKill();
    TestPreamble();
}

std::string Trim(std::string str) {
    str.erase(std::find_if_not(str.rbegin(), str.rend(), [](int c) {
        return std::isspace(c);
    }).base(), str.end());
    return str;
}

static std::string Join(const std::vector<std::string>& strings) {
    std::string result;
    for (size_t i = 0; i < strings.size(); ++i) {
        if (i > 0) {
            result += " ";
        }
        result += strings[i];
    }
    return result;
}

}  // namespace test
}  // namespace nexer
