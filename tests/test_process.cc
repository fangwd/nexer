#include <assert.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include "nexer/core/logger.h"
#include "nexer/runtime/process.h"
#include "runner.h"

namespace nexer {
namespace test {

namespace {

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const char *prefix) {
        auto base = std::filesystem::temp_directory_path();
        auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        std::stringstream ss;
        ss << prefix << '-' << ticks << '-' << std::this_thread::get_id();
        path = base / ss.str();
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        assert(!ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

static void should_read_stdout_stderr() {
    nexer::EventLoop loop;
    std::string output, error_output;

    auto process = nexer::Process::create(loop, exename);
    process->push_arg("helper");
    process->push_arg("hello");
    process->on_data([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        } else {
            error_output.append(s, len);
        }
    });
    process->start();
    loop.run();
    assert(output == "hello");
    assert(error_output == "world");
    assert(!process->is_running());
    assert(process->exit_code() == 0);
    assert(process->term_signal() == 0);
}

static void should_write_stdin() {
    nexer::EventLoop loop;
    std::string output, error_output;

    auto process = nexer::Process::create(loop, exename);
    process->push_arg("helper");
    process->push_arg("stdin");
    // Avoid capturing the shared_ptr in callbacks; otherwise the FunctionList
    // keeps the process alive after the event loop stops, triggering leaks.
    auto weak_process = std::weak_ptr<Process>(process);
    process->on_data([weak_process, &output,
                      &error_output](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
            if (output == "Message: ") {
                std::string message = "Hello, world.\n";
                if (auto process = weak_process.lock()) {
                    process->input(message.data(), message.size());
                }
            }
        } else {
            error_output.append(s, len);
        }
    });
    process->on_exit([&](int64_t exit_status, int term_signal) {
        // uv_stop(loop);
    });
    process->start();
    loop.run();
    if (output != "Message: Your message is 'Hello, world.'\n") {
        log_fatal("%s\n", output.data());
        assert(0);
    }
    assert(error_output == "no error\n");
    assert(!process->is_running());
    assert(process->exit_code() == 0);
    assert(process->term_signal() == 0);
}

static void should_timeout() {
    nexer::EventLoop loop;
    std::string output;
    bool exited = false;

    auto process = nexer::Process::create(loop, exename);
    process->push_arg("helper");
    process->push_arg("stdin");
    process->on_data(
        [&](int fd, const char *s, size_t len) { output.append(s, len); });
    process->on_exit(
        [&](int64_t exit_status, int term_signal) { exited = true; });
    process->set_timeout(100);
    process->start();
    loop.run();
    assert(output == "Message: ");
    assert(exited);
    assert(process->term_signal() == SIGTERM);
}

static void should_not_timeout() {
    nexer::EventLoop loop;
    std::string output;
    bool exited = false;

    auto process = nexer::Process::create(loop, exename);
    process->push_arg("helper");
    process->push_arg("hello");
    process->on_data(
        [&](int fd, const char *s, size_t len) { output.append(s, len); });
    process->on_exit(
        [&](int64_t exit_status, int term_signal) { exited = true; });
    process->set_timeout(500);
    process->start();
    loop.run();
    assert(output == "helloworld");
    assert(exited);
    assert(process->term_signal() == 0);
}

static void kill_after_1s(std::shared_ptr<nexer::Process> process) {
    uv_sleep(50);
    process->kill(SIGINT);
}

static void should_respond_signal() {
    nexer::EventLoop loop;
    std::string output, error_output;
    int signal;
    auto process = nexer::Process::create(loop, exename);
    process->push_arg("helper");
    process->push_arg("stdin");
    process->on_data([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        } else {
            error_output.append(s, len);
        }
    });
    process->on_exit([&](int64_t exit_status, int term_signal) {
        signal = term_signal;
        // uv_stop(loop);
    });
    process->start();
    std::thread th(kill_after_1s, process);
    loop.run();
    th.join();
    assert(output == "Message: ");
    assert(signal == SIGINT);
    assert(!process->is_running());
    assert(process->exit_code() == 0);
    assert(process->term_signal() == SIGINT);
}

static void should_set_env() {
    nexer::EventLoop loop;
    std::string output, error_output;

    auto process = nexer::Process::create(loop, exename);
    process->push_arg("helper");
    process->push_arg("set-env");
    process->on_data([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        } else {
            error_output.append(s, len);
        }
    });
    process->set_env("ENV1=env1");
    process->set_env("ENV3=");
    process->set_env("ENV4");
    process->start();
    loop.run();
    assert(output == "ENV1=env1ENV2=nullENV3=ENV4=null");
    assert(!process->is_running());
    assert(process->exit_code() == 0);
    assert(process->term_signal() == 0);
}

static void test_start_failing() {
    nexer::EventLoop loop;
    int on_exit_called = 0;
    int on_error_called = 0;
    auto process = nexer::Process::create(loop, "some-unknown-command");
    process->on_exit([&](int64_t status, int signal) { on_exit_called++; });
    process->on_error([&](int err) { on_error_called++; });
    assert(!process->start());
    loop.run();
    assert(on_error_called == 1);
    assert(on_exit_called == 0);
}

static void test_timeout_without_starting() {
    nexer::EventLoop loop;
    int on_exit_called = 0;
    auto process = nexer::Process::create(loop, "some-unknown-command");
    process->on_exit([&](int64_t status, int signal) { on_exit_called++; });
    process->set_timeout(100);
    loop.run();
    assert(on_exit_called == 1);
}

static void should_honor_configured_cwd() {
    TempDir temp_dir("nexer-cwd");

    nexer::EventLoop loop;
    std::string output;
    bool exited = false;

    nexer::config::Command cmd;
    cmd.file = std::filesystem::absolute(exename).string();
    cmd.args = {"helper", "print-cwd"};
    cmd.cwd = temp_dir.path.string();

    auto process = nexer::Process::create(loop, cmd);
    process->on_data([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        }
    });
    process->on_exit([&](int64_t, int) { exited = true; });

    assert(process->start());
    loop.run();

    assert(exited);
    auto expected_path = std::filesystem::canonical(temp_dir.path);
    auto actual_path =
        std::filesystem::canonical(std::filesystem::path(output));
    assert(actual_path == expected_path);
    assert(!process->is_running());
    assert(process->exit_code() == 0);
    assert(process->term_signal() == 0);
}

static void should_resolve_relative_executable_with_cwd() {
    TempDir temp_dir("nexer-rel");

    auto relative_exe = temp_dir.path / "nexer-helper-relative";
    std::error_code ec;
    bool copied = std::filesystem::copy_file(
        exename, relative_exe,
        std::filesystem::copy_options::overwrite_existing, ec);
    assert(!ec);
    assert(copied);

    nexer::EventLoop loop;
    std::string output;
    bool exited = false;

    nexer::config::Command cmd;
    cmd.file = std::string("./") + relative_exe.filename().string();
    cmd.args = {"helper", "print-cwd"};
    cmd.cwd = temp_dir.path.string();

    auto process = nexer::Process::create(loop, cmd);
    process->on_data([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        }
    });
    process->on_exit([&](int64_t, int) { exited = true; });

    assert(process->start());
    loop.run();

    assert(exited);
    auto expected_path = std::filesystem::canonical(temp_dir.path);
    auto actual_path =
        std::filesystem::canonical(std::filesystem::path(output));
    assert(actual_path == expected_path);
    assert(!process->is_running());
    assert(process->exit_code() == 0);
    assert(process->term_signal() == 0);
}

static void should_replace_inherited_environment() {
    setenv("ENV1", "inherited", 1);
    setenv("ENV2", "inherited", 1);
    unsetenv("ENV3");
    nexer::EventLoop loop;
    std::string output;
    config::Command command;
    command.file = exename;
    command.args = {"helper", "set-env"};
    command.env = {"ENV1=override", "ENV3=added"};
    auto process = nexer::Process::create(loop, command);
    process->on_data([&](int fd, const char *s, size_t len) {
        if (fd == 1) {
            output.append(s, len);
        }
    });
    process->start();
    loop.run();
    unsetenv("ENV1");
    unsetenv("ENV2");
    assert(output == "ENV1=overrideENV2=inheritedENV3=addedENV4=null");
}

void TestProcess() {
    should_replace_inherited_environment();
    should_read_stdout_stderr();
    should_write_stdin();
    should_timeout();
    should_not_timeout();
    should_respond_signal();
    should_set_env();
    test_start_failing();
    test_timeout_without_starting();
    should_honor_configured_cwd();
    should_resolve_relative_executable_with_cwd();
}

} // namespace test
} // namespace nexer
