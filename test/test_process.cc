#include <assert.h>

#include <string>
#include <thread>

#include "process.h"
#include "runner.h"
#include "logger.h"

namespace nexer {
namespace test {

static void should_read_stdout_stderr() {
    nexer::EventLoop loop;
    std::string output, error_output;

    nexer::Process& process = nexer::Process::Create(loop, exename);
    process.PushArg("helper");
    process.PushArg("hello");
    process.OnData([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        } else {
            error_output.append(s, len);
        }
    });
    process.Start();
    loop.Run();
    assert(output == "hello");
    assert(error_output == "world");
    assert(!process.IsRunning());
    assert(process.GetExitCode() == 0);
    assert(process.GetTermSignal() == 0);
}

static void should_write_stdin() {
    nexer::EventLoop loop;
    std::string output, error_output;

    nexer::Process& process = nexer::Process::Create(loop, exename);
    process.PushArg("helper");
    process.PushArg("stdin");
    process.OnData([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
            if (output == "Message: ") {
                std::string message = "Hello, world.\n";
                process.Input(message.data(), message.size());
            }
        } else {
            error_output.append(s, len);
        }
    });
    process.OnExit([&](int64_t exit_status, int term_signal) {
        // uv_stop(loop);
    });
    process.Start();
    loop.Run();
    if (output != "Message: Your message is 'Hello, world.'\n") {
        log_fatal("%s\n", output.data());
        assert(0);
    }
    assert(error_output == "no error\n");
    assert(!process.IsRunning());
    assert(process.GetExitCode() == 0);
    assert(process.GetTermSignal() == 0);
}

static void should_timeout() {
    nexer::EventLoop loop;
    std::string output;
    bool exited = false;

    nexer::Process &process = nexer::Process::Create(loop, exename);
    process.PushArg("helper");
    process.PushArg("stdin");
    process.OnData([&](int fd, const char *s, size_t len) {
        output.append(s, len);
    });
    process.OnExit([&](int64_t exit_status, int term_signal) {
        exited = true;
    });
    process.SetTimeout(1000);
    process.Start();
    loop.Run();
    assert(output == "Message: ");
    assert(exited);
    assert(process.GetTermSignal() == SIGTERM);
}

static void should_not_timeout() {
    nexer::EventLoop loop;
    std::string output;
    bool exited = false;

    nexer::Process &process = nexer::Process::Create(loop, exename);
    process.PushArg("helper");
    process.PushArg("hello");
    process.OnData([&](int fd, const char *s, size_t len) {
        output.append(s, len);
    });
    process.OnExit([&](int64_t exit_status, int term_signal) {
        exited = true;
    });
    process.SetTimeout(10000);
    process.Start();
    loop.Run();
    assert(output == "helloworld");
    assert(exited);
    assert(process.GetTermSignal() == 0);
}

static void kill_after_1s(nexer::Process *process) {
    uv_sleep(1000);
    process->Kill(SIGINT);
}

static void should_respond_signal() {
    nexer::EventLoop loop;
    std::string output, error_output;
    int signal;
    nexer::Process& process = nexer::Process::Create(loop, exename);
    process.PushArg("helper");
    process.PushArg("stdin");
    process.OnData([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        } else {
            error_output.append(s, len);
        }
    });
    process.OnExit([&](int64_t exit_status, int term_signal) {
        signal = term_signal;
        // uv_stop(loop);
    });
    process.Start();
    std::thread th(kill_after_1s, &process);
    loop.Run();
    th.join();
    assert(output == "Message: ");
    assert(signal == SIGINT);
    assert(!process.IsRunning());
    assert(process.GetExitCode() == 0);
    assert(process.GetTermSignal() == SIGINT);
}

static void should_set_env() {
    nexer::EventLoop loop;
    std::string output, error_output;

    nexer::Process& process = nexer::Process::Create(loop, exename);
    process.PushArg("helper");
    process.PushArg("set-env");
    process.OnData([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        } else {
            error_output.append(s, len);
        }
    });
    process.SetEnv("ENV1=env1");
    process.SetEnv("ENV3=");
    process.SetEnv("ENV4");
    process.Start();
    loop.Run();
    assert(output == "ENV1=env1ENV2=nullENV3=ENV4=null");
    assert(!process.IsRunning());
    assert(process.GetExitCode() == 0);
    assert(process.GetTermSignal() == 0);
}

static void test_start_failing() {
    nexer::EventLoop loop;
    int on_exit_called = 0;
    auto& process = nexer::Process::Create(loop, "some-unknown-command");
    process.OnExit([&](int64_t status, int signal) {
        on_exit_called++;
    });
    assert(!process.Start());
    loop.Run();
    assert(on_exit_called == 1);
}

static void test_timeout_without_starting() {
    nexer::EventLoop loop;
    int on_exit_called = 0;
    auto& process = nexer::Process::Create(loop, "some-unknown-command");
    process.OnExit([&](int64_t status, int signal) {
        on_exit_called++;
    });
    process.SetTimeout(1000);
    loop.Run();
    assert(on_exit_called == 1);
}

void TestProcess() {
    should_read_stdout_stderr();
    should_timeout();
    should_not_timeout();
    should_write_stdin();
    should_respond_signal();
    should_set_env();
    test_start_failing();
    test_timeout_without_starting();
}

}  // namespace test
}  // namespace nexer
