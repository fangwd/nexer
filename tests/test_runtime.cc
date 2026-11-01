// Subprocess integration coverage: drives an actual nexer process with
// temporary configs and loopback ports.
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "helper.h"
#include "jsini.hpp"
#include "nexer/core/timer.h"
#include "nexer/http/http_client.h"
#include "nexer/runtime/process.h"

namespace nexer {
namespace test {

namespace {

constexpr const char *kTokenEnv = "NEXER_ADMIN_TOKEN";
constexpr const char *kToken = "runtime-token";

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const char *prefix) {
        auto base = std::filesystem::temp_directory_path();
        auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        std::stringstream ss;
        ss << prefix << '-' << ticks << '-' << getpid();
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

bool ProcessGone(int pid) { return kill(pid, 0) != 0 && errno == ESRCH; }

// Config files live in a temp dir, so command paths must be absolute.
std::string RunTestsPath() {
    return std::filesystem::absolute("build/run_tests").string();
}

int ParsePid(const std::string &output, const char *key) {
    auto pos = output.find(key);
    if (pos == std::string::npos) {
        return 0;
    }
    return atoi(output.c_str() + pos + strlen(key));
}

struct Runtime {
    TempDir dir{"nexer-runtime"};
    EventLoop loop;
    HttpClient client;
    int admin_port = 0;
    std::string config_path;
    std::shared_ptr<Process> process;
    std::string output;
    bool exited = false;
    int64_t exit_code = -1;
    int term_signal = 0;
    int pid = 0;

    // `apps` is the JSON array body; `root` adds more root-level entries.
    Runtime(const std::string &apps, const std::string &root = "",
            const std::string &shutdown =
                "default_grace_ms: 100, force_after_ms: 300") {
        static bool curl_ok = curl_global_init(CURL_GLOBAL_DEFAULT) == 0;
        assert(curl_ok);
        setenv(kTokenEnv, kToken, 1);
        admin_port = PickFreePort();
        assert(admin_port > 0);

        std::ostringstream conf;
        conf << "{\n  admin: { enabled: true, bind: '127.0.0.1', port: "
             << admin_port << ", auth_token_env: " << kTokenEnv
             << ", shutdown: { " << shutdown << " } },\n"
             << "  logger: { level: DEBUG, file: '"
             << (dir.path / "nexer.log").string() << "' },\n"
             << "  apps: [" << apps << "]";
        if (!root.empty()) {
            conf << ",\n  " << root;
        }
        conf << "\n}\n";
        config_path = (dir.path / "nexer.conf").string();
        std::ofstream file(config_path);
        file << conf.str();
        file.close();
    }

    void Start() {
        process = Process::create(loop, "./build/nexer");
        process->push_arg("--config");
        process->push_arg(config_path.c_str());
        process->on_data(
            [this](int, const char *s, size_t len) { output.append(s, len); });
        process->on_exit([this](int64_t code, int signal) {
            exited = true;
            exit_code = code;
            term_signal = signal;
        });
        assert(process->start());
        pid = process->pid();
        assert(pid > 0);
    }

    bool RunUntil(std::function<bool()> done, uint64_t timeout_ms = 5000) {
        bool satisfied = done();
        if (satisfied) {
            return true;
        }
        auto started = Timer::now();
        auto timer = Timer::create(loop, 10);
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

    bool WaitExit(uint64_t timeout_ms = 5000) {
        return RunUntil([&] { return exited; }, timeout_ms);
    }

    bool WaitOutput(const char *needle, uint64_t timeout_ms = 5000) {
        return RunUntil(
            [&] { return output.find(needle) != std::string::npos; },
            timeout_ms);
    }

    std::string Url(const std::string &path) const {
        return "http://127.0.0.1:" + std::to_string(admin_port) + path;
    }

    std::unique_ptr<http::Response> Request(const std::string &method,
                                            const std::string &path,
                                            const std::string &body = "") {
        client.clear_headers();
        client.add_header(std::string("X-Nexer-Admin-Token: ") + kToken);
        return client.request(method, Url(path), body);
    }

    int Status(const std::string &method, const std::string &path,
               const std::string &body = "") {
        auto response = Request(method, path, body);
        return response ? response->status() : -1;
    }

    bool WaitReady(uint64_t timeout_ms = 5000) {
        return RunUntil([&] { return Status("GET", "/admin/readyz") == 200; },
                        timeout_ms);
    }

    std::string LogText() {
        std::ifstream file(dir.path / "nexer.log");
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    ~Runtime() {
        if (process && !exited) {
            process->kill(SIGKILL);
            WaitExit(2000);
        }
    }
};

std::string TreeApp(const char *name, const char *flags = "",
                    const char *extra = "auto_start: true") {
    std::string app = "{ name: ";
    app += name;
    app += ", command: { file: '" + RunTestsPath() + "', args: [helper, tree";
    if (*flags) {
        app += ", ";
        app += flags;
    }
    app += "] }, ";
    app += extra;
    app += " }";
    return app;
}

void AssertTreeGone(Runtime &runtime) {
    int leader = ParsePid(runtime.output, "leader=");
    int child = ParsePid(runtime.output, "child=");
    assert(leader > 0 && child > 0);
    assert(runtime.RunUntil([&] { return ProcessGone(child); }, 3000));
    assert(ProcessGone(leader));
}

} // namespace

static void should_exit_zero_on_sigterm_and_clean_descendants() {
    Runtime runtime(TreeApp("a"));
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());

    auto started = Timer::now();
    assert(kill(runtime.pid, SIGTERM) == 0);
    assert(kill(runtime.pid, SIGTERM) == 0); // repeated signal is harmless
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 0 && runtime.term_signal == 0);
    assert(Timer::now() - started < 3000);
    AssertTreeGone(runtime);
    assert(runtime.LogText().find("shutdown complete") != std::string::npos);
}

static void should_exit_zero_on_sigint() {
    Runtime runtime(TreeApp("a"));
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());
    assert(kill(runtime.pid, SIGINT) == 0);
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 0);
    AssertTreeGone(runtime);
}

static void should_clean_descendants_of_a_leader_that_exits_early() {
    Runtime runtime(TreeApp("a", "exit-early"));
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    int child = ParsePid(runtime.output, "child=");
    assert(child > 0);
    // The leader exits at once; nexer tears the remaining group down.
    assert(runtime.RunUntil([&] { return ProcessGone(child); }, 3000));
    assert(runtime.Status("GET", "/admin/processes/a") == 200);
    assert(kill(runtime.pid, SIGTERM) == 0);
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 0);
}

static void should_escalate_to_sigkill_for_descendants_ignoring_term() {
    Runtime runtime(TreeApp("a", "child-ignore-term"));
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());
    auto started = Timer::now();
    assert(kill(runtime.pid, SIGTERM) == 0);
    assert(runtime.WaitExit());
    auto elapsed = Timer::now() - started;
    assert(runtime.exit_code == 0);
    assert(elapsed >= 300 && elapsed < 4000);
    AssertTreeGone(runtime);
    assert(runtime.LogText().find("sending SIGKILL") != std::string::npos);
}

static void should_shutdown_over_http_and_deliver_the_response() {
    Runtime runtime(TreeApp("a"));
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());

    auto response = runtime.Request("POST", "/admin/shutdown",
                                    "{\"reason\":\"integration\"}");
    assert(response && response->status() == 202);
    std::string body(response->body().data(), response->body().size());
    jsini::Value root(body);
    assert(root.is_object());
    assert(static_cast<std::string>(root["status"]) == "shutting_down");
    assert(static_cast<std::string>(root["reason"]) == "integration");

    assert(runtime.WaitExit());
    assert(runtime.exit_code == 0);
    AssertTreeGone(runtime);
}

static void should_drain_proxy_connections_within_grace() {
    int proxy_port = PickFreePort();
    int echo_port = PickFreePort();
    std::ostringstream root;
    root << "proxies: [{ listen: " << proxy_port
         << ", upstream: { host: '127.0.0.1', port: " << echo_port << " } }],\n"
         << "  dummy: [{ type: tcp, listen: " << echo_port << ", echo: true }]";
    Runtime runtime(TreeApp("a"), root.str(),
                    "default_grace_ms: 2000, force_after_ms: 300");
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(proxy_port);
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    assert(send(fd, "ping", 4, 0) == 4);
    char buffer[16] = {0};
    assert(recv(fd, buffer, sizeof buffer, 0) == 4);
    assert(std::string(buffer, 4) == "ping");

    auto started = Timer::now();
    assert(kill(runtime.pid, SIGTERM) == 0);
    // The listener closes immediately but the open connection drains.
    runtime.RunUntil([] { return false; }, 400);
    assert(!runtime.exited);
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    assert(connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
           0);
    close(probe);
    close(fd);
    assert(runtime.WaitExit());
    auto elapsed = Timer::now() - started;
    assert(runtime.exit_code == 0);
    assert(elapsed >= 400 && elapsed < 2000);
}

static void should_exit_one_when_an_auto_start_app_fails() {
    Runtime runtime(
        "{ name: broken, command: { file: ./build/does-not-exist }, "
        "auto_start: true }, " +
        TreeApp("a"));
    runtime.Start();
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 1);
    if (runtime.output.find("child=") != std::string::npos) {
        AssertTreeGone(runtime);
    }
    auto log = runtime.LogText();
    assert(log.find("auto-start failed for broken") != std::string::npos);
    assert(log.find("startup failed") != std::string::npos);
}

static void should_fail_startup_without_waiting_for_other_roots() {
    std::string never_ready =
        "{ name: slow, command: { file: '" + RunTestsPath() +
        "', args: [helper, tree] }, checker: { file: '" + RunTestsPath() +
        "', args: [helper, sleep], env: [NAME=never] }, auto_start: true }";
    Runtime runtime(
        "{ name: broken, command: { file: ./build/does-not-exist }, "
        "auto_start: true }, " +
            never_ready,
        "startup_timeout_ms: 0");
    runtime.Start();
    auto started = Timer::now();
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 1);
    assert(Timer::now() - started < 3000);
    if (runtime.output.find("child=") != std::string::npos) {
        AssertTreeGone(runtime);
    }
    assert(runtime.LogText().find("auto-start failed for broken") !=
           std::string::npos);
}

static void should_not_launch_later_roots_after_startup_failure() {
    TempDir scratch("nexer-marker");
    auto marker = (scratch.path / "launched").string();
    Runtime runtime(
        "{ name: broken, command: { file: ./build/does-not-exist }, "
        "auto_start: true }, { name: later, type: task, command: { "
        "file: sh, args: [-c, 'touch " +
        marker + "'] }, auto_start: true }");
    runtime.Start();
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 1);
    runtime.RunUntil([] { return false; }, 200);
    assert(access(marker.c_str(), F_OK) != 0);
}

static void should_exit_one_on_startup_timeout() {
    std::string app = "{ name: a, command: { file: '" + RunTestsPath() +
                      "', args: [helper, tree] "
                      "}, "
                      "checker: { file: '" +
                      RunTestsPath() +
                      "', args: [helper, sleep], env: "
                      "[NAME=never] }, auto_start: true }";
    Runtime runtime(app, "startup_timeout_ms: 400");
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.Status("GET", "/admin/readyz") == 503);
    auto started = Timer::now();
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 1);
    assert(Timer::now() - started < 3000);
    AssertTreeGone(runtime);
    assert(runtime.LogText().find("startup timed out") != std::string::npos);
}

static void should_report_readiness_after_checker_passes() {
    SetScenario("runtime-ready");
    std::string app = "{ name: a, command: { file: '" + RunTestsPath() +
                      "', args: [helper, tree] "
                      "}, "
                      "checker: { file: '" +
                      RunTestsPath() +
                      "', args: [helper, sleep], env: "
                      "[NAME=b] }, auto_start: true }";
    Runtime runtime(app);
    runtime.Start();
    assert(runtime.WaitOutput("child="));

    auto health = runtime.Request("GET", "/admin/healthz");
    assert(health && health->status() == 200);
    auto not_ready = runtime.Request("GET", "/admin/readyz");
    assert(not_ready && not_ready->status() == 503);
    std::string body(not_ready->body().data(), not_ready->body().size());
    jsini::Value root(body);
    assert(static_cast<std::string>(root["status"]) == "not_ready");
    assert(root["blocking"].size() == 1);
    assert(static_cast<std::string>(root["blocking"][0]["name"]) == "a");
    assert(static_cast<std::string>(root["blocking"][0]["state"]) ==
           "starting");

    assert(runtime.WaitReady());
    assert(runtime.LogText().find("nexer: ready (1 apps") != std::string::npos);
    assert(kill(runtime.pid, SIGTERM) == 0);
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 0);
}

// Runs the nexer CLI synchronously and captures stdout+stderr.
struct CliResult {
    int64_t code = -1;
    std::string output;
};

static CliResult RunCli(const std::string &config_path,
                        std::vector<std::string> args,
                        uint64_t timeout_ms = 10000) {
    EventLoop loop;
    CliResult result;
    auto process = Process::create(loop, "./build/nexer");
    process->push_arg("--config");
    process->push_arg(config_path.c_str());
    for (auto &arg : args) {
        process->push_arg(arg.c_str());
    }
    process->on_data(
        [&](int, const char *s, size_t len) { result.output.append(s, len); });
    process->on_exit([&](int64_t code, int) { result.code = code; });
    process->set_timeout(timeout_ms);
    assert(process->start());
    loop.run();
    return result;
}

static void should_validate_without_starting_anything() {
    Runtime runtime(TreeApp("a"));
    auto ok = RunCli(runtime.config_path, {"validate"});
    assert(ok.code == 0);
    assert(access("./build/nexer", X_OK) == 0);
    assert(ok.output.find("configuration is valid") != std::string::npos);
    assert(ok.output.find("child=") == std::string::npos);
    assert(RunCli(runtime.config_path, {"status"}).code == 1); // not running

    auto bad_path = (runtime.dir.path / "bad.conf").string();
    std::ofstream bad(bad_path);
    bad << "{ apps: [{ name: a, type: task, checker: { file: x } }] }";
    bad.close();
    assert(RunCli(bad_path, {"validate"}).code == 1);
}

static void should_control_apps_from_the_cli() {
    Runtime runtime(TreeApp("a") + ", " +
                    TreeApp("idle", "", "auto_start: false"));
    runtime.Start();
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());

    auto status = RunCli(runtime.config_path, {"status"});
    assert(status.code == 0);
    assert(status.output.find("a ") != std::string::npos);
    assert(status.output.find("ready") != std::string::npos);
    assert(status.output.find("readiness: ready") != std::string::npos);

    auto logs = RunCli(runtime.config_path, {"logs", "a"});
    assert(logs.code == 0);
    assert(logs.output.find("leader=") != std::string::npos);

    auto stop = RunCli(runtime.config_path, {"stop", "a", "--wait"});
    assert(stop.code == 0);
    assert(stop.output.find("stop a: succeeded") != std::string::npos);
    AssertTreeGone(runtime);
    // A stopped required app makes readiness false.
    assert(runtime.Status("GET", "/admin/readyz") == 503);
    auto after = RunCli(runtime.config_path, {"status"});
    assert(after.output.find("stopped") != std::string::npos);
    assert(after.output.find("readiness: not_ready") != std::string::npos);

    auto missing = RunCli(runtime.config_path, {"start", "nope", "--wait"});
    assert(missing.code == 1);
    assert(missing.output.find("process not found") != std::string::npos);

    runtime.output.clear();
    auto start = RunCli(runtime.config_path, {"start", "a", "--wait"});
    assert(start.code == 0);
    assert(start.output.find("start a: succeeded") != std::string::npos);
    assert(runtime.WaitOutput("child="));
    assert(runtime.WaitReady());

    int before = ParsePid(runtime.output, "leader=");
    runtime.output.clear();
    auto restart = RunCli(runtime.config_path, {"restart", "a", "--wait"});
    assert(restart.code == 0);
    assert(runtime.WaitOutput("child="));
    assert(ParsePid(runtime.output, "leader=") != before);

    auto shutdown = RunCli(runtime.config_path, {"shutdown"});
    assert(shutdown.code == 0);
    assert(shutdown.output.find("shutdown:") != std::string::npos);
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 0);
    AssertTreeGone(runtime);
}

static void should_fail_startup_without_admin_token() {
    Runtime runtime(TreeApp("a"));
    unsetenv(kTokenEnv);
    runtime.Start();
    setenv(kTokenEnv, kToken, 1);
    assert(runtime.WaitExit());
    assert(runtime.exit_code == 1);
    assert(runtime.output.find("child=") == std::string::npos);
    assert(runtime.LogText().find("no admin token configured") !=
           std::string::npos);
}

static void should_refuse_control_commands_when_admin_is_disabled() {
    Runtime runtime(TreeApp("a"));
    std::ofstream file(runtime.config_path);
    file << "{ admin: { enabled: false }, apps: [" << TreeApp("a") << "] }";
    file.close();
    auto status = RunCli(runtime.config_path, {"status"});
    assert(status.code == 2);
    assert(status.output.find("admin listener is disabled") !=
           std::string::npos);
}

static void should_run_one_instance_per_config() {
    Runtime first(TreeApp("a"));
    first.Start();
    assert(first.WaitOutput("child="));

    auto duplicate = RunCli(first.config_path, {"run"});
    assert(duplicate.code == 1);
    assert(duplicate.output.find("already running") != std::string::npos);
    assert(duplicate.output.find("child=") == std::string::npos);

    // A different config runs independently.
    Runtime second(TreeApp("b"));
    second.Start();
    assert(second.WaitOutput("child="));
    assert(second.WaitReady());

    // A killed instance releases its lock.
    assert(kill(first.pid, SIGKILL) == 0);
    assert(first.WaitExit());
    Runtime again(TreeApp("a"));
    std::ofstream same(again.config_path);
    same.close();
    std::filesystem::copy_file(
        first.config_path, again.config_path,
        std::filesystem::copy_options::overwrite_existing);
    again.config_path = first.config_path;
    again.admin_port = first.admin_port;
    again.Start();
    assert(again.WaitOutput("child="));
    assert(again.WaitReady());
    assert(kill(again.pid, SIGTERM) == 0);
    assert(again.WaitExit());
    assert(again.exit_code == 0);
    assert(kill(second.pid, SIGTERM) == 0);
    assert(second.WaitExit());
    assert(second.exit_code == 0);
}

void TestRuntime() {
    // Keep instance lock files out of the user's state directory.
    TempDir locks("nexer-locks");
    setenv("XDG_RUNTIME_DIR", locks.path.c_str(), 1);

    should_validate_without_starting_anything();
    should_control_apps_from_the_cli();
    should_refuse_control_commands_when_admin_is_disabled();
    should_fail_startup_without_admin_token();
    should_run_one_instance_per_config();
    should_exit_zero_on_sigterm_and_clean_descendants();
    should_exit_zero_on_sigint();
    should_clean_descendants_of_a_leader_that_exits_early();
    should_escalate_to_sigkill_for_descendants_ignoring_term();
    should_shutdown_over_http_and_deliver_the_response();
    should_drain_proxy_connections_within_grace();
    should_exit_one_when_an_auto_start_app_fails();
    should_fail_startup_without_waiting_for_other_roots();
    should_not_launch_later_roots_after_startup_failure();
    should_exit_one_on_startup_timeout();
    should_report_readiness_after_checker_passes();
}

} // namespace test
} // namespace nexer
