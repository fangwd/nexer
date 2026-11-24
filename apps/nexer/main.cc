#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "jsini.hpp"
#include "nexer/core/logger.h"
#include "nexer/http/http_client.h"
#include "nexer/runtime/instance_lock.h"
#include "nexer/runtime/nexer.h"

static bool resolve_config_file(char *path, size_t len);

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] [command] [args]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr,
            "  -c, --config <file>  Path to config file (default: %s)\n",
            NEXER_CONFIG);
    fprintf(stderr, "  -d, --daemon         With run: continue in the "
                    "background once startup succeeds\n");
    fprintf(stderr, "  -h, --help           Show this help message\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  run                  Start the environment in the "
                    "foreground (default)\n");
    fprintf(stderr, "  validate             Parse and validate the config "
                    "without starting anything\n");
    fprintf(stderr, "  status               Show process states of the running "
                    "instance\n");
    fprintf(stderr, "  running              Exit 0 if an instance is running "
                    "for the config, 1 if not\n");
    fprintf(stderr, "  logs <app> [--follow]  Print captured output; --follow "
                    "streams new lines\n");
    fprintf(stderr, "  start <app> [--wait]   Start an app\n");
    fprintf(stderr, "  stop <app> [--wait]    Stop an app\n");
    fprintf(stderr, "  restart <app> [--wait] Restart an app\n");
    fprintf(stderr, "  shutdown [--wait]      Stop the running instance\n");
}

namespace {

// Blocking client for the admin API of a running instance.
class Control {
  private:
    nexer::Config &config_;
    std::string config_path_;
    std::string base_url_;
    std::string token_;
    nexer::HttpClient client_;

    static size_t stream_callback(char *data, size_t size, size_t count,
                                  void *user) {
        auto *pending = static_cast<std::string *>(user);
        pending->append(data, size * count);
        size_t start = 0;
        while (true) {
            auto end = pending->find("\n\n", start);
            if (end == std::string::npos) {
                break;
            }
            std::string frame = pending->substr(start, end - start);
            start = end + 2;
            auto data_pos = frame.find("data: ");
            if (data_pos == std::string::npos) {
                continue;
            }
            jsini::Value event(frame.substr(data_pos + 6));
            if (event.is_object() && event["text"].is_string()) {
                printf("[%s] %s\n", static_cast<const char *>(event["pipe"]),
                       static_cast<const char *>(event["text"]));
                fflush(stdout);
            }
        }
        pending->erase(0, start);
        return size * count;
    }

  public:
    Control(nexer::Config &config, std::string config_path)
        : config_(config), config_path_(std::move(config_path)) {
        // The admin listener is local; a proxy from the environment (often
        // this instance's own) cannot reach it.
        client_.bypass_proxy();
        auto &admin = config_.admin();
        base_url_ = "http://" + admin.bind + ":" + std::to_string(admin.port);
        if (!admin.auth_token_env.empty()) {
            if (const char *value = std::getenv(admin.auth_token_env.c_str())) {
                token_ = value;
            }
        }
        if (token_.empty() && !admin.auth_token_file.empty()) {
            std::ifstream stream(admin.auth_token_file);
            if (stream.good()) {
                std::getline(stream, token_);
            }
        }
    }

    bool available() const {
        if (!config_.admin().enabled) {
            fprintf(stderr,
                    "nexer: the admin listener is disabled in %s; enable "
                    "`admin` to use control commands\n",
                    config_path_.c_str());
            return false;
        }
        return true;
    }

    // Returns the HTTP status, or -1 after printing a connection error. The
    // response body is returned in `payload`.
    int request(const std::string &method, const std::string &path,
                std::string &payload, const std::string &body = "") {
        client_.clear_headers();
        client_.add_header("Content-Type: application/json");
        if (!token_.empty()) {
            client_.add_header(config_.admin().auth_header + ": " + token_);
        }
        auto response = client_.request(method, base_url_ + path, body);
        if (!response || response->status() < 100) {
            fprintf(stderr,
                    "nexer: cannot reach the admin listener at %s (is the "
                    "instance running?)\n",
                    base_url_.c_str());
            return -1;
        }
        payload.assign(response->body().data(), response->body().size());
        if (response->status() >= 400) {
            jsini::Value json(payload);
            const char *message =
                json.is_object() && json["message"].is_string()
                    ? static_cast<const char *>(json["message"])
                    : "request failed";
            fprintf(stderr, "nexer: %s (%d)\n", message, response->status());
        }
        return response->status();
    }

    // Streams the SSE log endpoint until the connection closes.
    int follow(const std::string &path) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            return 1;
        }
        std::string pending;
        struct curl_slist *headers = nullptr;
        if (!token_.empty()) {
            headers = curl_slist_append(
                headers, (config_.admin().auth_header + ": " + token_).c_str());
        }
        auto url = base_url_ + path;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pending);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        auto result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK) {
            fprintf(stderr,
                    "nexer: cannot reach the admin listener at %s: %s\n",
                    base_url_.c_str(), curl_easy_strerror(result));
            return 1;
        }
        return status >= 400 ? 1 : 0;
    }
};

int command_validate(nexer::Config &config, const std::string &config_path) {
    (void)config;
    printf("%s: configuration is valid (%zu apps, %zu proxies)\n",
           config_path.c_str(), config.apps().size(), config.proxies().size());
    return 0;
}

int command_status(Control &control) {
    std::string payload;
    int status = control.request("GET", "/admin/processes", payload);
    if (status != 200) {
        return 1;
    }
    jsini::Value json(payload);
    printf("%-20s %-9s %-10s %-8s %-7s %s\n", "NAME", "TYPE", "STATE",
           "DESIRED", "PID", "LAST ERROR");
    auto &processes = json["processes"];
    for (size_t i = 0; i < processes.size(); i++) {
        auto &entry = processes[i];
        int64_t pid = entry["pid"].is_integer() ? (int64_t)entry["pid"] : -1;
        const char *last_error =
            entry["last_error"].is_object()
                ? (const char *)entry["last_error"]["message"]
                : "";
        printf("%-20s %-9s %-10s %-8s %-7s %s\n", (const char *)entry["name"],
               (const char *)entry["type"], (const char *)entry["state"],
               (const char *)entry["desired_state"],
               pid > 0 ? std::to_string(pid).c_str() : "-", last_error);
    }
    std::string ready_payload;
    int ready_status = control.request("GET", "/admin/readyz", ready_payload);
    if (ready_status == 200 || ready_status == 503) {
        jsini::Value ready(ready_payload);
        printf("readiness: %s\n", (const char *)ready["status"]);
    }
    return 0;
}

int command_logs(Control &control, const std::string &app, bool follow) {
    if (follow) {
        return control.follow("/admin/processes/" + app + "/logs/stream");
    }
    std::string payload;
    int status =
        control.request("GET", "/admin/processes/" + app + "/logs", payload);
    if (status != 200) {
        return 1;
    }
    jsini::Value json(payload);
    auto &entries = json["entries"];
    for (size_t i = 0; i < entries.size(); i++) {
        auto &entry = entries[i];
        printf("%s [%s] %s\n", (const char *)entry["ts"],
               (const char *)entry["pipe"], (const char *)entry["text"]);
    }
    return 0;
}

int command_control(Control &control, const std::string &verb,
                    const std::string &app, bool wait) {
    std::string payload;
    int status = control.request("POST", "/admin/processes/" + app + "/" + verb,
                                 payload);
    if (status != 202) {
        return 1;
    }
    jsini::Value json(payload);
    auto &operation = json["operation"];
    std::string id = (const char *)operation["id"];
    printf("%s %s: accepted (operation %s)\n", verb.c_str(), app.c_str(),
           id.c_str());
    if (!wait) {
        return 0;
    }
    while (true) {
        std::string state_payload;
        if (control.request("GET", "/admin/operations/" + id, state_payload) !=
            200) {
            return 1;
        }
        jsini::Value state(state_payload);
        std::string value = (const char *)state["state"];
        if (value == "succeeded") {
            printf("%s %s: succeeded\n", verb.c_str(), app.c_str());
            return 0;
        }
        if (value == "failed") {
            fprintf(stderr, "nexer: %s %s failed: %s\n", verb.c_str(),
                    app.c_str(), (const char *)state["error"]["message"]);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int command_shutdown(Control &control, const std::string &config_path,
                     bool wait) {
    std::string payload;
    int status = control.request("POST", "/admin/shutdown", payload,
                                 "{\"reason\":\"nexer shutdown command\"}");
    if (status != 202) {
        return 1;
    }
    jsini::Value json(payload);
    printf("shutdown: %s\n", (const char *)json["status"]);
    if (!wait) {
        return 0;
    }
    // The instance holds its lock until it exits.
    while (nexer::InstanceLock::running(config_path)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("shutdown: stopped\n");
    return 0;
}

const int kLaunchSignals[] = {SIGINT, SIGTERM, SIGHUP};
pid_t daemon_pid = 0;
volatile sig_atomic_t daemon_interrupted = 0;

void stop_daemon(int) {
    daemon_interrupted = 1;
    kill(daemon_pid, SIGTERM);
}

// --daemon parent: returns 0 once the child reports readiness, otherwise the
// child's exit code. Interrupting it stops the child and returns 1.
int wait_for_daemon(pid_t pid, int ready_fd, const std::string &log_file,
                    const sigset_t &unblocked) {
    daemon_pid = pid;
    struct sigaction action = {};
    action.sa_handler = stop_daemon;
    sigemptyset(&action.sa_mask);
    for (int signum : kLaunchSignals) {
        sigaction(signum, &action, nullptr);
    }
    sigprocmask(SIG_SETMASK, &unblocked, nullptr);

    char byte;
    ssize_t n;
    while ((n = read(ready_fd, &byte, 1)) < 0 && errno == EINTR) {
    }
    close(ready_fd);
    if (n == 1 && !daemon_interrupted) {
        printf("nexer: running in the background (pid %d)%s\n",
               static_cast<int>(pid),
               log_file.empty() ? "; output is discarded without logger.file"
                                : "");
        return 0;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (daemon_interrupted) {
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// --daemon child: drops the terminal, kept until now for startup errors, and
// releases the parent.
void detach(int ready_fd, const std::string &log_file) {
    int null_fd = open("/dev/null", O_RDWR);
    int out = log_file.empty()
                  ? null_fd
                  : open(log_file.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (out < 0) {
        out = null_fd;
    }
    fflush(stdout);
    fflush(stderr);
    dup2(null_fd, 0);
    dup2(out, 1);
    dup2(out, 2);
    if (out > 2 && out != null_fd) {
        close(out);
    }
    if (null_fd > 2) {
        close(null_fd);
    }
    char byte = 1;
    if (write(ready_fd, &byte, 1) < 0) {
        // The parent is gone; nothing waits for this.
    }
    close(ready_fd);
}

int command_run(nexer::Config &config, const std::string &config_path,
                bool daemon) {
    int ready_fd = -1;
    if (daemon) {
        // Forks before any event loop exists; the child takes the lock so it
        // records its own pid.
        int fds[2];
        if (pipe(fds) != 0) {
            fprintf(stderr, "nexer: pipe: %s\n", strerror(errno));
            return 1;
        }
        // Held back until the parent can pass them on to the child.
        sigset_t signals, unblocked;
        sigemptyset(&signals);
        for (int signum : kLaunchSignals) {
            sigaddset(&signals, signum);
        }
        sigprocmask(SIG_BLOCK, &signals, &unblocked);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "nexer: fork: %s\n", strerror(errno));
            return 1;
        }
        if (pid > 0) {
            close(fds[1]);
            return wait_for_daemon(pid, fds[0], config.logger().file,
                                   unblocked);
        }
        sigprocmask(SIG_SETMASK, &unblocked, nullptr);
        close(fds[0]);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        setsid();
        ready_fd = fds[1];
    }

    nexer::InstanceLock lock;
    std::string error;
    if (!lock.acquire(config_path, error)) {
        fprintf(stderr, "nexer: %s\n", error.c_str());
        return 1;
    }
    default_logger.open(config.logger().file.c_str(), config.logger().level);
    nexer::Nexer nexer(config);
    if (daemon) {
        nexer.on_ready(
            [ready_fd, &config] { detach(ready_fd, config.logger().file); });
    }
    return nexer.run();
}

} // namespace

int main(int argc, char **argv) {
    std::string config_path;

    static struct option long_options[] = {
        {"config", required_argument, nullptr, 'c'},
        {"daemon", no_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    bool daemon = false;
    int opt;
    while ((opt = getopt_long(argc, argv, "+c:dh", long_options, nullptr)) !=
           -1) {
        switch (opt) {
        case 'c':
            config_path.assign(optarg);
            break;
        case 'd':
            daemon = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    std::string command = optind < argc ? argv[optind] : "run";
    if (daemon && command != "run") {
        fprintf(stderr, "nexer: --daemon only applies to run\n");
        return 1;
    }
    std::vector<std::string> args(argv + std::min(optind + 1, argc),
                                  argv + argc);
    bool wait = false;
    bool follow = false;
    std::string target;
    for (auto &arg : args) {
        if (arg == "--wait") {
            wait = true;
        } else if (arg == "--follow" || arg == "-f") {
            follow = true;
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "nexer: unknown option %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        } else if (target.empty()) {
            target = arg;
        } else {
            fprintf(stderr, "nexer: unexpected argument %s\n", arg.c_str());
            return 1;
        }
    }

    if (config_path.empty()) {
        char path[1024] = {0};
        if (!resolve_config_file(path, sizeof path)) {
            log_fatal("Cannot find %s", NEXER_CONFIG);
            return 1;
        }
        config_path.assign(path);
    }

    nexer::Config config;
    if (!nexer::Config::parse_file(config, config_path.c_str())) {
        log_fatal("Failed to parse %s", config_path.c_str());
        return 1;
    }

    if (command == "run") {
        return command_run(config, config_path, daemon);
    }
    if (command == "validate") {
        return command_validate(config, config_path);
    }
    if (command == "running") {
        bool running = nexer::InstanceLock::running(config_path);
        printf("nexer: %s\n", running ? "running" : "not running");
        return running ? 0 : 1;
    }

    bool needs_target = command == "logs" || command == "start" ||
                        command == "stop" || command == "restart";
    if (needs_target && target.empty()) {
        fprintf(stderr, "nexer: %s requires an app name\n", command.c_str());
        return 1;
    }
    if (!needs_target && command != "status" && command != "shutdown") {
        fprintf(stderr, "nexer: unknown command %s\n", command.c_str());
        print_usage(argv[0]);
        return 1;
    }

    Control control(config, config_path);
    if (!control.available()) {
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int status = 1;
    if (command == "status") {
        status = command_status(control);
    } else if (command == "logs") {
        status = command_logs(control, target, follow);
    } else if (command == "shutdown") {
        status = command_shutdown(control, config_path, wait);
    } else {
        status = command_control(control, command, target, wait);
    }
    curl_global_cleanup();
    return status;
}

static bool resolve_config_file(char *path, size_t len) {
    if (!path || len == 0)
        return false;

    auto exists_noexcept = [](const std::filesystem::path &p) -> bool {
        std::error_code ec;
        return std::filesystem::exists(p, ec) && !ec;
    };

    auto set_path = [&](const std::filesystem::path &p) -> bool {
        const std::string s =
            p.string(); // narrow string (OK for typical config paths)
        if (s.size() + 1 > len)
            return false;
        std::memcpy(path, s.c_str(), s.size() + 1);
        return true;
    };

    // 1) Path was provided by caller (argument)
    if (path[0] != '\0') {
        return exists_noexcept(std::filesystem::path(path));
    }

    // 2) Try "./NEXER_CONFIG"
    if (!set_path(NEXER_CONFIG))
        return false;
    if (exists_noexcept(std::filesystem::path(path)))
        return true;

    // 3) Try user config dir
    // Prefer XDG_CONFIG_HOME on Unix; fall back to HOME/.config
#ifndef _WIN32
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        std::filesystem::path cand =
            std::filesystem::path(xdg) / "nexer" / NEXER_CONFIG;
        if (set_path(cand) && exists_noexcept(cand))
            return true;
    }

    if (const char *home = std::getenv("HOME"); home && *home) {
        std::filesystem::path cand =
            std::filesystem::path(home) / ".config" / "nexer" / NEXER_CONFIG;
        if (set_path(cand) && exists_noexcept(cand))
            return true;
    }
#else
    // Windows: prefer APPDATA (Roaming)
    if (const char *appdata = std::getenv("APPDATA"); appdata && *appdata) {
        std::filesystem::path cand =
            std::filesystem::path(appdata) / "nexer" / NEXER_CONFIG;
        if (set_path(cand) && exists_noexcept(cand))
            return true;
    }
    // Optional fallback if APPDATA isn't set
    if (const char *home = std::getenv("USERPROFILE"); home && *home) {
        std::filesystem::path cand = std::filesystem::path(home) / "AppData" /
                                     "Roaming" / "nexer" / NEXER_CONFIG;
        if (set_path(cand) && exists_noexcept(cand))
            return true;
    }
#endif

    return false;
}
