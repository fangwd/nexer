#include <assert.h>
#include <stdlib.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "helper.h"
#include "jsini.hpp"
#include "nexer/http/http_client.h"
#include "nexer/runtime/admin_server.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"

namespace nexer {
namespace test {

namespace {

constexpr const char *kAdminTokenEnv = "NEXER_TEST_ADMIN_TOKEN";
constexpr const char *kAdminTokenValue = "secret-token";
constexpr const char *kAdminHeader = "X-Test-Token";

static std::string BufferToString(const StringBuffer &buffer) {
    return std::string(buffer.data(), buffer.size());
}

static std::unique_ptr<http::Response>
GetWithRetry(HttpClient &client, const std::string &url, int attempts = 5,
             std::chrono::milliseconds delay = std::chrono::milliseconds(50)) {
    std::unique_ptr<http::Response> response;
    for (int i = 0; i < attempts; ++i) {
        response = client.get(url);
        if (response && response->status() >= 100) {
            break;
        }
        std::this_thread::sleep_for(delay);
    }
    return response;
}

class FakeRuntime : public AdminRuntime {
  public:
    std::vector<ShutdownOptions> requests;
    Readiness state;
    bool stopping = false;

    bool request_shutdown(ShutdownOptions options) override {
        if (stopping) {
            return false;
        }
        stopping = true;
        requests.push_back(std::move(options));
        return true;
    }
    Readiness readiness() override { return state; }
    bool shutting_down() const override { return stopping; }
};

class AdminFixture {
  public:
    AdminFixture() : manager(loop) {
        static bool curl_init_ok = []() {
            return curl_global_init(CURL_GLOBAL_DEFAULT) == 0;
        }();
        if (!curl_init_ok) {
            fprintf(stderr, "curl_global_init failed\n");
            abort();
        }

        setenv(kAdminTokenEnv, kAdminTokenValue, 1);

        port = PickFreePort();
        assert(port > 0);

        config.admin().enabled = true;
        config.admin().bind = "127.0.0.1";
        config.admin().port = port;
        config.admin().auth_header = kAdminHeader;
        config.admin().auth_token_env = kAdminTokenEnv;
        config.admin().health_require_auth = false;
        config.admin().logs.follow_enabled = true;
        config.admin().logs.default_tail_lines = 4;
        config.admin().logs.max_tail_lines = 16;
        config.admin().audit.retain_entries = 32;
        config.admin().shutdown.default_delay_ms = 0;
        config.admin().shutdown.default_grace_ms = 10;
        config.admin().shutdown.force_after_ms = 25;

        auto *app = new config::App();
        app->name = "app-one";
        app->command.file = "./build/run_tests";
        app->command.args = {"helper", "sleep"};
        // Scenario data in helper.cc is keyed by NAME, so reuse "a" to get
        // a deterministic message/output for the admin log assertions.
        app->command.env = {"NAME=a"};
        app->max_start_time = 500;
        config.apps().push_back(app);

        admin = std::make_unique<AdminServer>(loop, config, manager);
        admin->set_runtime(&runtime);
        bool started = admin->start();
        assert(started);
        loop_thread = std::thread([this] {
            loop_running_.store(true, std::memory_order_release);
            loop.run();
            loop_running_.store(false, std::memory_order_release);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~AdminFixture() {
        // Post cleanup to the loop thread so handles are closed on the
        // correct thread.  Stop() forces the loop to exit; any leftover
        // handles are cleaned up safely by EventLoop::Close().
        if (loop_running_.load(std::memory_order_acquire)) {
            loop.post([this]() {
                admin->stop();
                manager.Shutdown();
                loop.stop();
            });
        }
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
        // Safety: ensure Stop is called even if the loop had already
        // exited before the Post callback ran (double-call is harmless).
        admin->stop();
    }

    std::string Url(const std::string &path) const {
        char buf[256];
        snprintf(buf, sizeof buf, "http://127.0.0.1:%d%s", port, path.c_str());
        return std::string(buf);
    }

    std::unique_ptr<http::Response>
    Request(const std::string &method, const std::string &path,
            const std::string &body = std::string(), bool include_auth = true) {
        client.clear_headers();
        if (include_auth) {
            client.add_header(std::string(kAdminHeader) + ": " +
                              kAdminTokenValue);
        }
        return client.request(method, Url(path), body);
    }

    void RequireApp(const char *scenario, bool reset_scenario = true) {
        if (reset_scenario) {
            SetScenario(scenario);
        }
        assert(!config.apps().empty());
        auto *app = config.apps()[0];
        assert(app);
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        // Dispatch the Require call to the event loop thread so that
        // uv_spawn, uv_read_start, and timer setup all happen on the
        // correct thread.  Without this, pipe read callbacks never fire
        // because the loop is unaware of handles created from another
        // thread.
        loop.post([this, app, done]() {
            manager.Require(
                *app, [done](std::shared_ptr<Process> process, int error) {
                    assert(error == 0);
                    (void)process;
                    if (done) {
                        done->set_value();
                    }
                });
        });
        auto status = future.wait_for(std::chrono::seconds(2));
        assert(status == std::future_status::ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    EventLoop loop;
    Config config;
    ProcessManager manager;
    FakeRuntime runtime;
    std::unique_ptr<AdminServer> admin;
    std::thread loop_thread;
    std::atomic<bool> loop_running_{false};
    int port = 0;
    HttpClient client;
};

static bool JsonInfoHasPort(const std::string &body, int expected_port) {
    jsini::Value root(body);
    if (!root.is_object())
        return false;
    return (int)root["admin"]["port"] == expected_port;
}

static std::unique_ptr<jsini::Value>
ParseJsonOrAbort(const std::unique_ptr<http::Response> &response,
                 const char *context = "response") {
    if (!response) {
        fprintf(stderr, "%s missing response\n", context);
        abort();
    }
    std::string payload = BufferToString(response->body());
    auto root = std::make_unique<jsini::Value>(payload);
    if (!root->is_object()) {
        fprintf(stderr, "%s returned invalid JSON: %s\n", context,
                payload.c_str());
        abort();
    }
    return root;
}

static std::unique_ptr<jsini::Value> WaitForProcessDetail(
    AdminFixture &fixture, const std::string &expected_state,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto detail = fixture.Request("GET", "/admin/processes/app-one");
        if (!detail || detail->status() != 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        auto payload = BufferToString(detail->body());
        auto root = std::make_unique<jsini::Value>(payload);
        if (!root->is_object()) {
            fprintf(stderr, "process detail invalid JSON: %s\n",
                    payload.c_str());
            abort();
        }
        if (expected_state.empty() || !(*root)["state"].is_string()) {
            return root;
        }
        auto state = static_cast<std::string>((*root)["state"]);
        if (state == expected_state) {
            return root;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    fprintf(stderr, "process did not reach state %s\n", expected_state.c_str());
    abort();
}

static std::string ReadSse(AdminFixture &fixture, const std::string &path,
                           size_t max_bytes = 4096) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(fixture.port);
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: 127.0.0.1\r\n";
    request << kAdminHeader << ": " << kAdminTokenValue << "\r\n";
    request << "Accept: text/event-stream\r\n";
    request << "Connection: keep-alive\r\n\r\n";
    auto payload = request.str();
    auto sent = send(fd, payload.data(), payload.size(), 0);
    assert(sent == static_cast<ssize_t>(payload.size()));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string data;
    data.reserve(max_bytes);
    char buffer[1024];
    size_t events = 0;
    while (data.size() < max_bytes) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        data.append(buffer, n);
        size_t count = 0;
        size_t pos = 0;
        while ((pos = data.find("event: log", pos)) != std::string::npos) {
            ++count;
            pos += 9;
        }
        events = count;
        if (events >= 2 &&
            data.find("\n\n", data.rfind("event: log")) != std::string::npos) {
            break;
        }
    }
    close(fd);
    return data;
}

static void should_require_authentication() {
    AdminFixture fixture;
    fixture.client.clear_headers();

    auto health = GetWithRetry(fixture.client, fixture.Url("/admin/healthz"));
    if (!health || health->status() != 200) {
        fprintf(stderr, "admin health status: %d\n",
                health ? health->status() : -1);
        abort();
    }

    auto info_unauth = GetWithRetry(fixture.client, fixture.Url("/admin/info"));
    if (!info_unauth || info_unauth->status() != 401) {
        fprintf(stderr, "admin info missing auth status: %d\n",
                info_unauth ? info_unauth->status() : -1);
        abort();
    }

    fixture.client.clear_headers();
    fixture.client.add_header(std::string(kAdminHeader) + ": wrong-token");
    auto info_forbidden =
        GetWithRetry(fixture.client, fixture.Url("/admin/info"));
    if (!info_forbidden || info_forbidden->status() != 403) {
        fprintf(stderr, "admin info wrong token status: %d\n",
                info_forbidden ? info_forbidden->status() : -1);
        abort();
    }

    fixture.client.clear_headers();
    fixture.client.add_header(std::string(kAdminHeader) + ": " +
                              kAdminTokenValue);
    auto info = GetWithRetry(fixture.client, fixture.Url("/admin/info"));
    if (!info || info->status() != 200) {
        fprintf(stderr, "admin info status: %d\n", info ? info->status() : -1);
        abort();
    }
    std::string payload = BufferToString(info->body());
    if (!JsonInfoHasPort(payload, fixture.port)) {
        fprintf(stderr, "admin info port mismatch (body: %s)\n",
                payload.c_str());
        return;
    }
}

static void should_list_and_describe_processes() {
    AdminFixture fixture;
    fixture.RequireApp("admin-long-run");
    auto detail = WaitForProcessDetail(fixture, "ready");
    assert(static_cast<std::string>((*detail)["state"]) == "ready");

    auto list = fixture.Request("GET", "/admin/processes");
    if (!list || list->status() != 200) {
        fprintf(stderr, "admin processes list status: %d\n",
                list ? list->status() : -1);
        abort();
    }
    auto list_json = ParseJsonOrAbort(list, "process list");
    auto &processes = (*list_json)["processes"];
    assert(processes.size() >= 1);
    auto &entry = processes[0];
    assert(static_cast<std::string>(entry["state"]) == "ready");
    assert(static_cast<int64_t>(entry["pid"]) > 0);

    auto &command = (*detail)["command"];
    assert(static_cast<std::string>(command["file"]) == "./build/run_tests");
    assert(command["args"].size() == 2);
    assert(static_cast<std::string>(command["args"][0]) == "helper");
    assert(static_cast<std::string>(command["args"][1]) == "sleep");
    auto &io = (*detail)["io"];
    assert(io["stdout_bytes"].is_integer());
    assert(io["stderr_bytes"].is_integer());

    auto missing = fixture.Request("GET", "/admin/processes/unknown");
    assert(missing && missing->status() == 404);
}

static void should_manage_process_states_and_io() {
    AdminFixture fixture;
    fixture.RequireApp("admin-stateflow");
    auto running = WaitForProcessDetail(fixture, "ready");
    assert(static_cast<std::string>((*running)["state"]) == "ready");

    auto stopped = WaitForProcessDetail(fixture, "completed");
    auto &io = (*stopped)["io"];
    assert(static_cast<uint64_t>(io["stdout_lines"]) >= 1);
    auto &last_exit = (*stopped)["last_exit"];
    assert(static_cast<int64_t>(last_exit["code"]) == 0);

    fixture.RequireApp("admin-stateflow", false);
    WaitForProcessDetail(fixture, "ready");
    WaitForProcessDetail(fixture, "completed");

    fixture.RequireApp("admin-stateflow", false);
    WaitForProcessDetail(fixture, "ready");
    auto failed = WaitForProcessDetail(fixture, "failed");
    auto &failed_exit = (*failed)["last_exit"];
    assert(static_cast<int64_t>(failed_exit["code"]) == 5);
}

static void should_collect_process_logs() {
    AdminFixture fixture;
    fixture.RequireApp("admin-log-burst");

    auto defaults = fixture.Request(
        "GET", "/admin/processes/app-one/logs?pipe=stdout&lines=0");
    assert(defaults && defaults->status() == 200);
    auto default_json = ParseJsonOrAbort(defaults, "default logs");
    assert((*default_json)["entries"].size() ==
           fixture.config.admin().logs.default_tail_lines);

    auto expanded = fixture.Request(
        "GET", "/admin/processes/app-one/logs?pipe=stdout&lines=32");
    assert(expanded && expanded->status() == 200);
    auto expanded_json = ParseJsonOrAbort(expanded, "expanded logs");
    auto &expanded_entries = (*expanded_json)["entries"];
    assert(expanded_entries.size() ==
           fixture.config.admin().logs.max_tail_lines);
    auto last_id = static_cast<uint64_t>(
        expanded_entries[expanded_entries.size() - 1]["id"]);
    assert(static_cast<uint64_t>((*expanded_json)["next_token"]) == last_id);

    auto mid_id = static_cast<uint64_t>(expanded_entries[10]["id"]);
    auto page = fixture.Request(
        "GET", "/admin/processes/app-one/logs?pipe=stdout&lines=32&token=" +
                   std::to_string(mid_id));
    assert(page && page->status() == 200);
    auto page_json = ParseJsonOrAbort(page, "paged logs");
    auto &page_entries = (*page_json)["entries"];
    assert(page_entries.size() == expanded_entries.size() - 11);
    assert(static_cast<uint64_t>(page_entries[0]["id"]) > mid_id);

    AdminFixture pipes;
    auto *app = pipes.config.apps()[0];
    app->command.args = {"helper", "hello"};
    app->command.env.clear();
    pipes.RequireApp("simple-with-sleep");

    auto stderr_logs = pipes.Request(
        "GET", "/admin/processes/app-one/logs?pipe=stderr&lines=5");
    assert(stderr_logs && stderr_logs->status() == 200);
    auto stderr_json = ParseJsonOrAbort(stderr_logs, "stderr logs");
    assert((*stderr_json)["entries"].size() == 1);
    assert(static_cast<std::string>((*stderr_json)["entries"][0]["pipe"]) ==
           "stderr");

    auto both_logs =
        pipes.Request("GET", "/admin/processes/app-one/logs?pipe=both&lines=5");
    assert(both_logs && both_logs->status() == 200);
    auto both_json = ParseJsonOrAbort(both_logs, "both logs");
    assert((*both_json)["entries"].size() == 2);
}

static void should_stream_process_logs() {
    AdminFixture fixture;
    fixture.RequireApp("admin-log-burst");
    auto stream =
        ReadSse(fixture, "/admin/processes/app-one/logs/stream?pipe=stdout");
    if (stream.find("HTTP/1.1 200") == std::string::npos ||
        stream.find("event: log") == std::string::npos) {
        fprintf(stderr, "unexpected SSE payload: %s\n", stream.c_str());
        abort();
    }
    if (stream.find("id:") == std::string::npos ||
        stream.find("data:") == std::string::npos) {
        fprintf(stderr, "SSE chunk missing id/data: %s\n", stream.c_str());
        abort();
    }
}

static void should_handle_signals_and_deletes() {
    AdminFixture fixture;

    fixture.RequireApp("admin-long-run");
    WaitForProcessDetail(fixture, "ready");
    auto signal = fixture.Request("POST", "/admin/processes/app-one/signal",
                                  "{\"signal\":\"HUP\"}");
    assert(signal && signal->status() == 202);
    auto signal_json = ParseJsonOrAbort(signal, "signal response");
    assert(static_cast<std::string>((*signal_json)["signal"]) == "SIGHUP");
    // Raw signals leave the desired state alone; the exit is observed as
    // failed.
    auto hup_exit = WaitForProcessDetail(fixture, "failed");
    assert(static_cast<std::string>((*hup_exit)["last_exit"]["signal"]) ==
           "SIGHUP");
    assert(static_cast<std::string>((*hup_exit)["desired_state"]) == "running");

    fixture.RequireApp("admin-long-run");
    WaitForProcessDetail(fixture, "ready");
    auto numeric_signal = fixture.Request(
        "POST", "/admin/processes/app-one/signal", "{\"signal\":2}");
    assert(numeric_signal && numeric_signal->status() == 202);
    auto numeric_json = ParseJsonOrAbort(numeric_signal, "numeric signal");
    assert((*numeric_json)["signal"].is_string());
    WaitForProcessDetail(fixture, "failed");

    fixture.RequireApp("admin-long-run");
    WaitForProcessDetail(fixture, "ready");
    // DELETE is an alias for a deliberate stop.
    auto kill_one = fixture.Request("DELETE", "/admin/processes/app-one");
    assert(kill_one && kill_one->status() == 202);
    auto kill_json = ParseJsonOrAbort(kill_one, "kill one");
    assert(static_cast<std::string>((*kill_json)["state"]) == "stopping");
    assert((*kill_json)["operation"]["id"].is_string());
    auto stopped = WaitForProcessDetail(fixture, "stopped");
    assert(static_cast<std::string>((*stopped)["desired_state"]) == "stopped");
    assert(static_cast<std::string>((*stopped)["last_exit"]["signal"]) ==
           "SIGTERM");

    auto start = fixture.Request("POST", "/admin/processes/app-one/start");
    assert(start && start->status() == 202);
    WaitForProcessDetail(fixture, "ready");

    // dry_run only reports the plan; the former signal query is rejected.
    auto planned =
        fixture.Request("DELETE", "/admin/processes/app-one?dry_run=true");
    assert(planned && planned->status() == 202);
    auto planned_json = ParseJsonOrAbort(planned, "dry run");
    assert(static_cast<std::string>((*planned_json)["state"]) == "planned");
    assert((*planned_json)["operation"].is_null());
    auto planned_all = fixture.Request("DELETE", "/admin/processes?dry_run=1");
    assert(planned_all && planned_all->status() == 202);
    auto planned_all_json = ParseJsonOrAbort(planned_all, "dry run all");
    assert(static_cast<std::string>(
               (*planned_all_json)["results"][0]["state"]) == "planned");
    auto with_signal =
        fixture.Request("DELETE", "/admin/processes/app-one?signal=KILL");
    assert(with_signal && with_signal->status() == 400);
    auto still = WaitForProcessDetail(fixture, "ready");
    assert(static_cast<std::string>((*still)["desired_state"]) == "running");

    auto kill_all = fixture.Request("DELETE", "/admin/processes");
    assert(kill_all && kill_all->status() == 202);
    auto kill_all_json = ParseJsonOrAbort(kill_all, "kill all");
    assert((*kill_all_json)["results"].size() == 1);
    assert(static_cast<std::string>((*kill_all_json)["results"][0]["state"]) ==
           "stopping");
    WaitForProcessDetail(fixture, "stopped");
}

static std::unique_ptr<jsini::Value> WaitForOperation(
    AdminFixture &fixture, const std::string &id,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto response = fixture.Request("GET", "/admin/operations/" + id);
        assert(response && response->status() == 200);
        auto root = ParseJsonOrAbort(response, "operation");
        auto state = static_cast<std::string>((*root)["state"]);
        if (state != "pending") {
            return root;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    fprintf(stderr, "operation %s did not finish\n", id.c_str());
    abort();
}

static void should_start_and_stop_with_operations() {
    AdminFixture fixture;
    SetScenario("admin-long-run");

    auto unknown = fixture.Request("POST", "/admin/processes/nope/start");
    assert(unknown && unknown->status() == 404);
    auto missing = fixture.Request("GET", "/admin/operations/op-ff");
    assert(missing && missing->status() == 404);

    auto start = fixture.Request("POST", "/admin/processes/app-one/start");
    assert(start && start->status() == 202);
    auto start_json = ParseJsonOrAbort(start, "start");
    assert(static_cast<std::string>((*start_json)["state"]) == "starting");
    auto &operation = (*start_json)["operation"];
    assert(static_cast<std::string>(operation["kind"]) == "start");
    assert(static_cast<std::string>(operation["process"]) == "app-one");
    auto start_id = static_cast<std::string>(operation["id"]);
    auto started = WaitForOperation(fixture, start_id);
    assert(static_cast<std::string>((*started)["state"]) == "succeeded");
    assert((*started)["error"].is_null());
    assert((*started)["finished_at"].is_string());
    WaitForProcessDetail(fixture, "ready");

    auto stop = fixture.Request("POST", "/admin/processes/app-one/stop");
    assert(stop && stop->status() == 202);
    auto stop_json = ParseJsonOrAbort(stop, "stop");
    auto stop_id = static_cast<std::string>((*stop_json)["operation"]["id"]);
    assert(stop_id != start_id);
    auto stopped = WaitForOperation(fixture, stop_id);
    assert(static_cast<std::string>((*stopped)["state"]) == "succeeded");
    auto detail = WaitForProcessDetail(fixture, "stopped");
    assert(static_cast<std::string>((*detail)["desired_state"]) == "stopped");
    assert(static_cast<bool>((*detail)["ready"]) == false);

    // A restart carries an operation too; stopping it again is a no-op.
    auto restart = fixture.Request("POST", "/admin/processes/app-one/restart");
    assert(restart && restart->status() == 202);
    auto restart_json = ParseJsonOrAbort(restart, "restart");
    auto restart_id =
        static_cast<std::string>((*restart_json)["operation"]["id"]);
    auto restarted = WaitForOperation(fixture, restart_id);
    assert(static_cast<std::string>((*restarted)["state"]) == "succeeded");
    WaitForProcessDetail(fixture, "ready");

    // A failed start is reported through the operation.
    auto *app = fixture.config.apps()[0];
    fixture.Request("POST", "/admin/processes/app-one/stop");
    WaitForProcessDetail(fixture, "stopped");
    app->command.file = "./build/does-not-exist";
    auto failing = fixture.Request("POST", "/admin/processes/app-one/start");
    assert(failing && failing->status() == 202);
    auto failing_json = ParseJsonOrAbort(failing, "failing start");
    auto failing_id =
        static_cast<std::string>((*failing_json)["operation"]["id"]);
    auto failed = WaitForOperation(fixture, failing_id);
    assert(static_cast<std::string>((*failed)["state"]) == "failed");
    assert((*failed)["error"]["message"].is_string());
}

static void should_restart_processes() {
    AdminFixture fixture;
    fixture.RequireApp("admin-restart");
    WaitForProcessDetail(fixture, "ready");

    auto restart = fixture.Request("POST", "/admin/processes/app-one/restart");
    assert(restart && restart->status() == 202);
    auto restart_json = ParseJsonOrAbort(restart, "restart response");
    assert(static_cast<std::string>((*restart_json)["state"]) == "restarting");

    auto running = WaitForProcessDetail(fixture, "ready");
    assert(static_cast<uint64_t>((*running)["restarts"]) >= 1);
}

static void should_record_audit_log() {
    AdminFixture fixture;
    auto info = fixture.Request("GET", "/admin/info");
    assert(info && info->status() == 200);

    auto unauth = fixture.Request("GET", "/admin/info", std::string(), false);
    assert(unauth && unauth->status() == 401);

    fixture.client.clear_headers();
    fixture.client.add_header(std::string(kAdminHeader) + ": rejected");
    auto forbidden = fixture.client.request("GET", fixture.Url("/admin/info"));
    assert(forbidden && forbidden->status() == 403);

    auto missing = fixture.Request("GET", "/admin/nope");
    assert(missing && missing->status() == 404);

    auto audit = fixture.Request("GET", "/admin/audit-log");
    assert(audit && audit->status() == 200);
    auto audit_json = ParseJsonOrAbort(audit, "audit log");
    auto &entries = (*audit_json)["entries"];
    assert(entries.size() >= 4);
    size_t base = entries.size() - 4;
    int expected[] = {200, 401, 403, 404};
    for (size_t i = 0; i < 4; ++i) {
        assert(static_cast<int64_t>(entries[base + i]["status"]) ==
               expected[i]);
    }
}

static void should_shutdown_with_delays() {
    AdminFixture fixture;
    auto invalid =
        fixture.Request("POST", "/admin/shutdown", "{\"grace_ms\":-1}");
    assert(invalid && invalid->status() == 400);
    assert(fixture.runtime.requests.empty());

    auto response =
        fixture.Request("POST", "/admin/shutdown", "{\"delay_ms\":50}");
    if (!response || response->status() != 202) {
        fprintf(stderr, "admin shutdown schedule status: %d\n",
                response ? response->status() : -1);
        abort();
    }
    auto root = ParseJsonOrAbort(response, "shutdown response");
    assert(static_cast<std::string>((*root)["status"]) == "scheduled");
    assert(static_cast<uint64_t>((*root)["delay_ms"]) == 50);
    assert(fixture.runtime.requests.size() == 1);
    auto &options = fixture.runtime.requests[0];
    assert(options.delay_ms == 50);
    assert(options.grace_ms ==
           fixture.config.admin().shutdown.default_grace_ms);
    assert(options.force_after_ms ==
           fixture.config.admin().shutdown.force_after_ms);

    auto duplicate = fixture.Request("GET", "/admin/shutdown");
    assert(duplicate && duplicate->status() == 409);

    // Mutations are rejected once shutdown was requested; status remains.
    auto restart = fixture.Request("POST", "/admin/processes/app-one/restart");
    assert(restart && restart->status() == 503);
    auto kill = fixture.Request("DELETE", "/admin/processes/app-one");
    assert(kill && kill->status() == 503);
    auto list = fixture.Request("GET", "/admin/processes");
    assert(list && list->status() == 200);
    auto health = fixture.Request("GET", "/admin/healthz");
    assert(health && health->status() == 200);
}

static void should_require_token_and_reject_tls() {
    EventLoop loop;
    Config config;
    ProcessManager manager(loop);
    config.admin().enabled = true;
    config.admin().bind = "127.0.0.1";
    config.admin().port = PickFreePort();
    config.admin().auth_token_env = "NEXER_TEST_MISSING_TOKEN";
    unsetenv("NEXER_TEST_MISSING_TOKEN");
    {
        AdminServer admin(loop, config, manager);
        assert(!admin.start());
        admin.stop();
    }
    setenv("NEXER_TEST_MISSING_TOKEN", "present", 1);
    config.admin().require_tls = true;
    {
        AdminServer admin(loop, config, manager);
        assert(!admin.start());
        admin.stop();
    }
    config.admin().require_tls = false;
    {
        AdminServer admin(loop, config, manager);
        assert(admin.start());
        admin.stop();
    }
    unsetenv("NEXER_TEST_MISSING_TOKEN");
    loop.run();
}

static void should_redact_environment_values() {
    AdminFixture fixture;
    auto *app = fixture.config.apps()[0];
    app->command.env = {"NAME=a", "SECRET_TOKEN=hunter2"};
    app->logs.stdout_file = "/tmp/nexer-test-out.log";
    auto detail = fixture.Request("GET", "/admin/processes/app-one");
    assert(detail && detail->status() == 200);
    std::string body = BufferToString(detail->body());
    assert(body.find("hunter2") == std::string::npos);
    auto root = ParseJsonOrAbort(detail, "detail");
    auto &env = (*root)["command"]["env"];
    assert(env.size() == 2);
    assert(static_cast<std::string>(env[0]) == "NAME=<redacted>");
    assert(static_cast<std::string>(env[1]) == "SECRET_TOKEN=<redacted>");
    assert(static_cast<std::string>((*root)["logs"]["stdout_file"]) ==
           "/tmp/nexer-test-out.log");
    assert((*root)["logs"]["stderr_file"].is_null());
    assert((*root)["logs"]["error"].is_null());

    auto audit = fixture.Request("GET", "/admin/audit-log");
    assert(audit &&
           BufferToString(audit->body()).find("hunter2") == std::string::npos);
}

static void should_report_readiness() {
    AdminFixture fixture;
    auto unauth = fixture.Request("GET", "/admin/readyz", std::string(), false);
    assert(unauth && unauth->status() == 401);

    ProcessSnapshot blocking;
    blocking.app = fixture.config.apps()[0];
    blocking.observed = ObservedState::Starting;
    blocking.desired = DesiredState::Running;
    fixture.runtime.state.blocking.push_back(blocking);
    auto not_ready = fixture.Request("GET", "/admin/readyz");
    assert(not_ready && not_ready->status() == 503);
    auto not_ready_json = ParseJsonOrAbort(not_ready, "readyz");
    assert(static_cast<std::string>((*not_ready_json)["status"]) ==
           "not_ready");
    assert(static_cast<bool>((*not_ready_json)["startup_complete"]) == false);
    auto &entries = (*not_ready_json)["blocking"];
    assert(entries.size() == 1);
    assert(static_cast<std::string>(entries[0]["name"]) == "app-one");
    assert(static_cast<std::string>(entries[0]["state"]) == "starting");

    fixture.runtime.state.blocking.clear();
    fixture.runtime.state.startup_complete = true;
    fixture.runtime.state.ready = true;
    auto ready = fixture.Request("GET", "/admin/readyz");
    assert(ready && ready->status() == 200);
    auto ready_json = ParseJsonOrAbort(ready, "readyz");
    assert(static_cast<std::string>((*ready_json)["status"]) == "ready");
}

} // namespace

void TestAdminServer() {
    should_require_authentication();
    should_list_and_describe_processes();
    should_manage_process_states_and_io();
    should_collect_process_logs();
    should_stream_process_logs();
    should_handle_signals_and_deletes();
    should_restart_processes();
    should_record_audit_log();
    should_shutdown_with_delays();
    should_report_readiness();
    should_start_and_stop_with_operations();
    should_require_token_and_reject_tls();
    should_redact_environment_values();
}

} // namespace test
} // namespace nexer
