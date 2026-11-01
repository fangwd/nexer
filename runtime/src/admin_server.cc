#include "nexer/runtime/admin_server.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

#include "jsini.hpp"
#include "nexer/core/logger.h"
#include "nexer/core/string_buffer.h"
#include "nexer/core/timer.h"
#include "nexer/core/url.h"
#include "nexer/http/http_server.h"
#include "nexer/runtime/process.h"
#include "uv.h"

#include "llhttp.h"

namespace nexer {

namespace {

const char kHeaderOrigin[] = "Origin";
const char kHeaderRequestPrivateNetwork[] =
    "Access-Control-Request-Private-Network";

std::string join_strings(const std::vector<std::string> &values,
                         const char *delim = ", ") {
    std::string out;
    bool first = true;
    for (const auto &value : values) {
        if (value.empty()) {
            continue;
        }
        if (!first) {
            out.append(delim);
        }
        out.append(value);
        first = false;
    }
    return out;
}

bool parse_int64(const jsini::Value &value, uint64_t &out) {
    if (value.is_integer()) {
        long long v = static_cast<long long>(value);
        if (v < 0) {
            return false;
        }
        out = static_cast<uint64_t>(v);
        return true;
    }
    return false;
}

bool parse_int(const jsini::Value &value, int &out) {
    if (value.is_integer()) {
        long long v = static_cast<long long>(value);
        if (v < std::numeric_limits<int>::min() ||
            v > std::numeric_limits<int>::max()) {
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }
    return false;
}

std::string build_process_label(const config::App &app) {
    if (!app.name.empty()) {
        return app.name;
    }
    std::ostringstream oss;
    bool first = true;
    for (const auto &env : app.command.env) {
        if (!first)
            oss << ' ';
        oss << env.substr(0, env.find('='));
        first = false;
    }
    if (!app.command.file.empty()) {
        if (!first)
            oss << ' ';
        oss << app.command.file;
        first = false;
    }
    for (const auto &arg : app.command.args) {
        if (!first)
            oss << ' ';
        oss << arg;
        first = false;
    }
    return oss.str();
}

const char *app_type_name(config::App::Type type) {
    switch (type) {
    case config::App::Type::Task:
        return "task";
    case config::App::Type::External:
        return "external";
    case config::App::Type::Service:
        break;
    }
    return "service";
}

std::string to_lower(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (auto ch : value) {
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::string to_upper(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (auto ch : value) {
        out.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool equals_ignore_case(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string format_iso8601(uint64_t millis_since_epoch) {
    std::time_t seconds =
        static_cast<std::time_t>(millis_since_epoch / 1000ULL);
    int millis = static_cast<int>(millis_since_epoch % 1000ULL);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, millis);
    return std::string(buffer);
}

std::optional<uint64_t> parse_iso8601(const std::string &value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::tm tm{};
    char tail = '\0';
    int millis = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d.%d%c", &tm.tm_year,
                    &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec, &millis, &tail) >= 7) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
#if defined(_WIN32)
        auto seconds = _mkgmtime(&tm);
#else
        auto seconds = timegm(&tm);
#endif
        if (seconds < 0) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(seconds) * 1000ULL +
               static_cast<uint64_t>(millis);
    }
    return std::nullopt;
}

std::string http_method_name(uint8_t method_id) {
    auto method = static_cast<llhttp_method>(method_id);
    const char *name = llhttp_method_name(method);
    return name ? std::string(name) : std::string("UNKNOWN");
}

std::string to_string(int signal) {
    if (signal == 0)
        return "NONE";
#ifdef _WIN32
    return std::to_string(signal);
#else
    switch (signal) {
    case SIGTERM:
        return "SIGTERM";
    case SIGINT:
        return "SIGINT";
#ifdef SIGHUP
    case SIGHUP:
        return "SIGHUP";
#endif
#ifdef SIGUSR1
    case SIGUSR1:
        return "SIGUSR1";
#endif
#ifdef SIGUSR2
    case SIGUSR2:
        return "SIGUSR2";
#endif
#ifdef SIGKILL
    case SIGKILL:
        return "SIGKILL";
#endif
    default:
        return std::to_string(signal);
    }
#endif
}

int to_signal(const std::string &value, bool &ok) {
    ok = true;
    if (value.empty()) {
        return SIGTERM;
    }

    std::string upper = to_upper(value);

    auto parse_int = [&](const std::string &s) -> int {
        char *end = nullptr;
        long number = std::strtol(s.c_str(), &end, 10);
        if (!end || *end != '\0') {
            ok = false;
            return SIGTERM;
        }
        return static_cast<int>(number);
    };

#ifdef SIGTERM
    if (upper == "SIGTERM" || upper == "TERM")
        return SIGTERM;
#endif
#ifdef SIGINT
    if (upper == "SIGINT" || upper == "INT")
        return SIGINT;
#endif
#ifdef SIGHUP
    if (upper == "SIGHUP" || upper == "HUP")
        return SIGHUP;
#endif
#ifdef SIGUSR1
    if (upper == "SIGUSR1" || upper == "USR1")
        return SIGUSR1;
#endif
#ifdef SIGUSR2
    if (upper == "SIGUSR2" || upper == "USR2")
        return SIGUSR2;
#endif
#ifdef SIGKILL
    if (upper == "SIGKILL" || upper == "KILL")
        return SIGKILL;
#endif

    if (std::all_of(upper.begin(), upper.end(),
                    [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return parse_int(upper);
    }

    ok = false;
    return SIGTERM;
}

uint64_t clamp(uint64_t value, uint64_t default_value, uint64_t max_value) {
    if (value == 0) {
        value = default_value;
    }
    if (value > max_value) {
        value = max_value;
    }
    return value;
}

} // namespace

class AdminServer::Impl {
  public:
    Impl(EventLoop &loop, Config &config, ProcessManager &manager)
        : loop_(loop), config_(config), process_manager_(manager),
          settings_(config.admin()), boot_time_ms_(Timer::now()),
          default_tail_lines_(
              std::max<size_t>(1, settings_.logs.default_tail_lines)),
          max_tail_lines_(std::max<size_t>(default_tail_lines_,
                                           settings_.logs.max_tail_lines)),
          audit_retention_(
              std::max<size_t>(16, settings_.audit.retain_entries)) {
        cors_allow_methods_value_ = join_strings(settings_.cors.allow_methods);
        auto cors_headers = settings_.cors.allow_headers;
        if (!settings_.auth_header.empty()) {
            auto has_auth_header = std::any_of(
                cors_headers.begin(), cors_headers.end(),
                [&](const std::string &value) {
                    return equals_ignore_case(value, settings_.auth_header);
                });
            if (!has_auth_header) {
                cors_headers.push_back(settings_.auth_header);
            }
        }
        cors_allow_headers_value_ = join_strings(cors_headers);
        build_app_index();
        load_auth_token();
    }

    bool start();
    void stop();
    inline void set_runtime(AdminRuntime *runtime) { runtime_ = runtime; }

  private:
    struct IoStats {
        uint64_t stdout_bytes = 0;
        uint64_t stderr_bytes = 0;
        uint64_t stdout_lines = 0;
        uint64_t stderr_lines = 0;
        uint64_t last_stdout_ms = 0;
        uint64_t last_stderr_ms = 0;
    };

    struct ProcessInfo {
        const config::App *config = nullptr;
        std::string label;
        IoStats io;
    };

    struct LogEntry {
        uint64_t seq = 0;
        uint64_t ts_ms = 0;
        int pipe = 1;
        std::string text;
    };

    struct ProcessLog {
        std::deque<LogEntry> entries;
        std::string partial_stdout;
        std::string partial_stderr;
    };

    struct Operation {
        std::string id;
        std::string process;
        std::string kind;
        std::string state = "pending";
        int error = 0;
        uint64_t created_at_ms = 0;
        uint64_t finished_at_ms = 0;
    };

    struct AuditEntry {
        uint64_t ts_ms = 0;
        std::string actor;
        std::string remote;
        std::string method;
        std::string path;
        int status = 0;
        std::string request_id;
    };

    struct LogStream : public std::enable_shared_from_this<LogStream> {
        std::string process_name;
        int pipe_mask = 0;
        uint64_t last_seq = 0;
        std::shared_ptr<http::incoming::Request> request;
        std::shared_ptr<Timer> heartbeat_timer;
    };

    EventLoop &loop_;
    Config &config_;
    ProcessManager &process_manager_;
    config::Admin settings_;

    std::shared_ptr<http::Server> server_;
    std::vector<FunctionList<void>::remove_fn> subscriptions_;

    std::unordered_map<std::string, const config::App *> label_to_app_;
    std::unordered_map<const config::App *, std::string> app_to_label_;
    std::unordered_map<std::string, ProcessInfo> processes_;
    std::unordered_map<std::string, ProcessLog> logs_;

    std::deque<AuditEntry> audit_log_;
    std::unordered_map<std::string, Operation> operations_;
    std::deque<std::string> operation_order_;
    uint64_t next_operation_id_ = 1;

    uint64_t boot_time_ms_ = 0;
    size_t default_tail_lines_ = 500;
    size_t max_tail_lines_ = 5000;
    size_t audit_retention_ = 1000;
    uint64_t next_log_seq_ = 1;
    uint64_t next_request_id_ = 1;

    std::string auth_token_;
    std::string cors_allow_methods_value_;
    std::string cors_allow_headers_value_;

    std::vector<std::shared_ptr<LogStream>> streams_;
    AdminRuntime *runtime_ = nullptr;

    void build_app_index();
    void load_auth_token();

    void register_process_hooks();

    void handle_request(const std::shared_ptr<http::incoming::Request> &request,
                        http::outgoing::Response &response);

    bool authenticate(const http::incoming::Request &request,
                      http::outgoing::Response &response, int &status_out);

    void write_json(http::outgoing::Response &response,
                    const std::string &payload, int status = 200);
    void write_json(http::outgoing::Response &response,
                    const jsini::Value &payload, int status = 200);
    void write_error(http::outgoing::Response &response, int status,
                     const std::string &message,
                     const std::string &request_id = {});

    std::string generate_request_id();

    void handle_health(const std::shared_ptr<http::incoming::Request> &request,
                       http::outgoing::Response &response,
                       const std::string &request_id);
    void handle_ready(const std::shared_ptr<http::incoming::Request> &request,
                      http::outgoing::Response &response,
                      const std::string &request_id);
    bool reject_mutation(http::outgoing::Response &response,
                         const std::string &request_id);
    void handle_info(const std::shared_ptr<http::incoming::Request> &request,
                     http::outgoing::Response &response,
                     const std::string &request_id);
    void handle_list_processes(
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response, const std::string &request_id);
    void handle_process_detail(
        const std::string &process_name,
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response, const std::string &request_id);
    void
    handle_process_logs(const std::string &process_name,
                        const std::shared_ptr<http::incoming::Request> &request,
                        http::outgoing::Response &response,
                        const std::string &request_id);
    void handle_process_logs_stream(
        const std::string &process_name,
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response, const std::string &request_id);
    void
    handle_shutdown(const std::shared_ptr<http::incoming::Request> &request,
                    http::outgoing::Response &response,
                    const std::string &request_id);
    void handle_signal_process(
        const std::string &process_name,
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response, const std::string &request_id);
    void handle_restart_process(
        const std::string &process_name,
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response, const std::string &request_id);
    void handle_start_process(
        const std::string &process_name,
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response, const std::string &request_id);
    void
    handle_stop_process(const std::string &process_name,
                        const std::shared_ptr<http::incoming::Request> &request,
                        http::outgoing::Response &response,
                        const std::string &request_id);
    void
    handle_stop_all(const std::shared_ptr<http::incoming::Request> &request,
                    http::outgoing::Response &response,
                    const std::string &request_id);
    bool parse_stop_query(const std::shared_ptr<http::incoming::Request> &,
                          http::outgoing::Response &,
                          const std::string &request_id, bool &dry_run);
    void
    handle_operation(const std::string &operation_id,
                     const std::shared_ptr<http::incoming::Request> &request,
                     http::outgoing::Response &response,
                     const std::string &request_id);
    Operation &begin_operation(const std::string &process,
                               const std::string &kind);
    ProcessManager::Callback
    operation_callback(const std::string &operation_id);
    void write_operation(jsini::Value &node, const Operation &operation) const;
    void
    handle_audit_log(const std::shared_ptr<http::incoming::Request> &request,
                     http::outgoing::Response &response,
                     const std::string &request_id);

    void append_audit(const std::string &actor, const std::string &remote,
                      const std::string &method, const std::string &path,
                      int status, const std::string &request_id);

    std::string get_header_value(const http::incoming::Request &request,
                                 const std::string &name) const;
    std::optional<std::string>
    resolve_cors_origin(const std::string &origin) const;
    void apply_cors_headers(http::outgoing::Response &response,
                            const std::string &allow_origin_value);
    void handle_cors_preflight(
        const std::shared_ptr<http::incoming::Request> &request,
        http::outgoing::Response &response,
        const std::string &allow_origin_value);

    void append_log_line(const std::string &process_name, int pipe,
                         const std::string &line);
    void flush_pending_log(const std::string &process_name, int pipe,
                           std::string &buffer);
    void on_process_data(std::shared_ptr<Process> process, int pipe,
                         const char *data, size_t len);

    static int pipe_mask_from_string(const std::string &value);
    static std::string pipe_name(int pipe);
    void write_io_stats(jsini::Value &node, const IoStats &io) const;
    void write_process_summary(jsini::Value &node, const std::string &name,
                               const ProcessInfo &info);
    ProcessInfo *find_process(const std::string &name);
};

bool AdminServer::Impl::start() {
    if (!settings_.enabled) {
        log_info("admin: HTTP admin interface disabled");
        return true;
    }
    if (settings_.require_tls) {
        log_error("admin: require_tls is set but native TLS is not "
                  "supported; terminate TLS in front of the loopback "
                  "listener or use an SSH tunnel");
        return false;
    }
    if (auth_token_.empty()) {
        log_error("admin: no admin token configured; set %s or "
                  "auth_token_file to a nonempty secret",
                  settings_.auth_token_env.empty()
                      ? "auth_token_env"
                      : settings_.auth_token_env.c_str());
        return false;
    }

    server_ = http::Server::create(loop_);
    if (!server_) {
        log_error("admin: failed to create HTTP server");
        return false;
    }

    server_->on_request([this](std::shared_ptr<http::incoming::Request> request,
                               http::outgoing::Response &response) {
        handle_request(request, response);
    });

    if (!server_->Listen(settings_.bind, settings_.port)) {
        log_error("admin: failed to listen on %s:%d", settings_.bind.c_str(),
                  settings_.port);
        server_->close();
        server_.reset();
        return false;
    }

    register_process_hooks();
    log_info("admin: listening on %s:%d", settings_.bind.c_str(),
             settings_.port);
    return true;
}

void AdminServer::Impl::stop() {
    for (auto &remove : subscriptions_) {
        if (remove) {
            remove();
        }
    }
    subscriptions_.clear();

    for (auto &stream : streams_) {
        if (!stream)
            continue;
        if (stream->heartbeat_timer) {
            stream->heartbeat_timer->stop();
            stream->heartbeat_timer->close();
        }
        if (stream->request) {
            stream->request->response().end(200);
        }
    }
    streams_.clear();

    if (server_) {
        server_->close();
        server_.reset();
    }
}

void AdminServer::Impl::build_app_index() {
    for (auto *app : config_.apps()) {
        if (!app) {
            continue;
        }
        auto label = build_process_label(*app);
        if (!label.empty()) {
            label_to_app_[label] = app;
            app_to_label_[app] = label;
        }
        ProcessInfo info;
        info.config = app;
        info.label = label;
        processes_[label] = std::move(info);
    }
}

void AdminServer::Impl::load_auth_token() {
    if (!settings_.auth_token_env.empty()) {
        if (const char *value = std::getenv(settings_.auth_token_env.c_str())) {
            auth_token_ = value;
        }
    }

    if (auth_token_.empty() && !settings_.auth_token_file.empty()) {
        std::ifstream stream(settings_.auth_token_file);
        if (stream.good()) {
            std::getline(stream, auth_token_);
        }
    }
}

AdminServer::Impl::ProcessInfo *
AdminServer::Impl::find_process(const std::string &name) {
    auto it = processes_.find(name);
    return it == processes_.end() ? nullptr : &it->second;
}

void AdminServer::Impl::register_process_hooks() {
    subscriptions_.push_back(process_manager_.OnProcessStart(
        [this](std::shared_ptr<Process> process) {
            if (!process)
                return;
            auto name = process->name();
            auto *info = find_process(name);
            if (!info)
                return;
            info->io = {};
            auto &log_buffer = logs_[name];
            log_buffer.entries.clear();
            log_buffer.partial_stdout.clear();
            log_buffer.partial_stderr.clear();
        }));

    subscriptions_.push_back(process_manager_.OnProcessExit(
        [this](std::shared_ptr<Process> process, int64_t, int) {
            if (!process)
                return;
            auto name = process->name();
            if (!find_process(name))
                return;

            // Flush any remaining partial data as final log entries.
            auto &log_buffer = logs_[name];
            if (!log_buffer.partial_stdout.empty()) {
                append_log_line(name, 1, log_buffer.partial_stdout);
                log_buffer.partial_stdout.clear();
            }
            if (!log_buffer.partial_stderr.empty()) {
                append_log_line(name, 2, log_buffer.partial_stderr);
                log_buffer.partial_stderr.clear();
            }
        }));

    subscriptions_.push_back(process_manager_.OnProcessData(
        [this](std::shared_ptr<Process> process, int pipe, const char *data,
               size_t len) {
            on_process_data(std::move(process), pipe, data, len);
        }));
}

std::string AdminServer::Impl::generate_request_id() {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << next_request_id_++;
    return oss.str();
}

void AdminServer::Impl::write_json(http::outgoing::Response &response,
                                   const std::string &payload, int status) {
    response.set_header("Content-Type", "application/json");
    response.body() << payload;
    response.end(status);
}

void AdminServer::Impl::write_json(http::outgoing::Response &response,
                                   const jsini::Value &payload, int status) {
    std::ostringstream buffer;
    payload.dump(buffer);
    write_json(response, buffer.str(), status);
}

void AdminServer::Impl::write_error(http::outgoing::Response &response,
                                    int status, const std::string &message,
                                    const std::string &request_id) {
    jsini::Value root;
    root["timestamp"] = format_iso8601(Timer::now()).c_str();
    root["status"] = static_cast<int64_t>(status);
    root["error"] = status >= 500 ? "server_error" : "request_error";
    root["message"] = message.c_str();
    if (!request_id.empty()) {
        root["request_id"] = request_id.c_str();
    }
    write_json(response, root, status);
}

bool AdminServer::Impl::authenticate(const http::incoming::Request &request,
                                     http::outgoing::Response &response,
                                     int &status_out) {
    status_out = 200;

    auto header_value = get_header_value(request, settings_.auth_header);

    if (header_value.empty()) {
        status_out = 401;
        write_error(response, status_out, "missing admin token");
        return false;
    }

    if (header_value != auth_token_) {
        status_out = 403;
        write_error(response, status_out, "admin token rejected");
        return false;
    }

    return true;
}

void AdminServer::Impl::handle_request(
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response) {
    auto request_id = generate_request_id();
    response.set_header("X-Request-Id", request_id);

    const auto method = http_method_name(request->method());
    const auto path = request->url().path;

    std::optional<std::string> cors_allow_origin;
    if (settings_.cors.enabled) {
        auto origin = get_header_value(*request, kHeaderOrigin);
        if (!origin.empty()) {
            cors_allow_origin = resolve_cors_origin(origin);
            if (cors_allow_origin) {
                apply_cors_headers(response, *cors_allow_origin);
            }
        }
        if (method == "OPTIONS") {
            if (cors_allow_origin) {
                handle_cors_preflight(request, response, *cors_allow_origin);
            } else {
                response.end(204);
            }
            return;
        }
    }

    bool requires_auth = true;
    if (path == "/admin/healthz" && !settings_.health_require_auth) {
        requires_auth = false;
    }

    int auth_status = 200;
    if (requires_auth && !authenticate(*request, response, auth_status)) {
        append_audit("anonymous", "", method, path, auth_status, request_id);
        return;
    }

    if (path == "/admin/healthz") {
        handle_health(request, response, request_id);
        append_audit("token", "", method, path, 200, request_id);
        return;
    }
    if (path == "/admin/readyz" && method == "GET") {
        handle_ready(request, response, request_id);
        append_audit("token", "", method, path, response.status(), request_id);
        return;
    }
    // The shutdown route reports a duplicate request as 409 itself.
    bool mutation =
        (method == "POST" || method == "DELETE") && path != "/admin/shutdown";
    if (mutation && reject_mutation(response, request_id)) {
        append_audit("token", "", method, path, response.status(), request_id);
        return;
    }
    if (path == "/admin/info") {
        handle_info(request, response, request_id);
        append_audit("token", "", method, path, 200, request_id);
        return;
    }
    if (path == "/admin/processes" && method == "GET") {
        handle_list_processes(request, response, request_id);
        append_audit("token", "", method, path, 200, request_id);
        return;
    }
    if (path.rfind("/admin/processes/", 0) == 0) {
        auto tail = std::string(path.begin() + std::strlen("/admin/processes/"),
                                path.end());
        auto slash = tail.find('/');
        std::string process_name =
            slash == std::string::npos ? tail : tail.substr(0, slash);
        std::string remainder =
            slash == std::string::npos ? std::string() : tail.substr(slash + 1);

        if (process_name.empty()) {
            write_error(response, 404, "process not specified", request_id);
            append_audit("token", "", method, path, 404, request_id);
            return;
        }

        if (method == "GET" && remainder.empty()) {
            handle_process_detail(process_name, request, response, request_id);
            append_audit("token", "", method, path, 200, request_id);
            return;
        }
        if (method == "GET" && remainder == "logs") {
            handle_process_logs(process_name, request, response, request_id);
            append_audit("token", "", method, path, 200, request_id);
            return;
        }
        if (method == "GET" && remainder == "logs/stream") {
            handle_process_logs_stream(process_name, request, response,
                                       request_id);
            append_audit("token", "", method, path, 200, request_id);
            return;
        }
        if (method == "POST" && remainder == "signal") {
            handle_signal_process(process_name, request, response, request_id);
            append_audit("token", "", method, path, response.status(),
                         request_id);
            return;
        }
        if (method == "POST" && remainder == "restart") {
            handle_restart_process(process_name, request, response, request_id);
            append_audit("token", "", method, path, response.status(),
                         request_id);
            return;
        }
        if (method == "POST" && remainder == "start") {
            handle_start_process(process_name, request, response, request_id);
            append_audit("token", "", method, path, response.status(),
                         request_id);
            return;
        }
        if ((method == "POST" && remainder == "stop") ||
            (method == "DELETE" && remainder.empty())) {
            handle_stop_process(process_name, request, response, request_id);
            append_audit("token", "", method, path, response.status(),
                         request_id);
            return;
        }
    }
    if (path == "/admin/processes" && method == "DELETE") {
        handle_stop_all(request, response, request_id);
        append_audit("token", "", method, path, response.status(), request_id);
        return;
    }
    if (path.rfind("/admin/operations/", 0) == 0 && method == "GET") {
        auto operation_id = std::string(
            path.begin() + std::strlen("/admin/operations/"), path.end());
        handle_operation(operation_id, request, response, request_id);
        append_audit("token", "", method, path, response.status(), request_id);
        return;
    }
    if (path == "/admin/shutdown" && (method == "POST" || method == "GET")) {
        handle_shutdown(request, response, request_id);
        append_audit("token", "", method, path, response.status(), request_id);
        return;
    }
    if (path == "/admin/audit-log" && method == "GET") {
        handle_audit_log(request, response, request_id);
        append_audit("token", "", method, path, 200, request_id);
        return;
    }

    write_error(response, 404, "unknown admin endpoint", request_id);
    append_audit("token", "", method, path, 404, request_id);
}

void AdminServer::Impl::handle_health(
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &) {
    jsini::Value root;
    root["status"] = "ok";
    write_json(response, root, 200);
}

void AdminServer::Impl::handle_ready(
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &request_id) {
    if (!runtime_) {
        write_error(response, 503, "readiness unavailable", request_id);
        return;
    }
    auto readiness = runtime_->readiness();
    jsini::Value root;
    root["status"] = readiness.ready ? "ready" : "not_ready";
    root["startup_complete"] = readiness.startup_complete;
    root["shutting_down"] = readiness.shutting_down;
    auto &blocking = root["blocking"];
    for (auto &snapshot : readiness.blocking) {
        auto &entry = blocking[blocking.size()];
        auto it = app_to_label_.find(snapshot.app);
        entry["name"] = it == app_to_label_.end() ? "" : it->second.c_str();
        entry["state"] = to_string(snapshot.observed);
        entry["desired_state"] = to_string(snapshot.desired);
    }
    write_json(response, root, readiness.ready ? 200 : 503);
}

bool AdminServer::Impl::reject_mutation(http::outgoing::Response &response,
                                        const std::string &request_id) {
    if (runtime_ && runtime_->shutting_down()) {
        write_error(response, 503, "runtime is shutting down", request_id);
        return true;
    }
    return false;
}

void AdminServer::Impl::handle_info(
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &) {
    jsini::Value root;
    root["timestamp"] = format_iso8601(Timer::now()).c_str();
    root["started_at"] = format_iso8601(boot_time_ms_).c_str();
    root["uptime_ms"] = static_cast<uint64_t>(Timer::now() - boot_time_ms_);
    root["version"] = "unknown";
    root["commit"] = "unknown";

    auto &admin = root["admin"];
    admin["bind"] = settings_.bind.c_str();
    admin["port"] = static_cast<int64_t>(settings_.port);
    admin["tls"] = settings_.require_tls;

    auto &logs = admin["logs"];
    logs["default_tail_lines"] = static_cast<uint64_t>(default_tail_lines_);
    logs["max_tail_lines"] = static_cast<uint64_t>(max_tail_lines_);

    write_json(response, root, 200);
}

void AdminServer::Impl::write_io_stats(jsini::Value &node,
                                       const IoStats &io) const {
    auto &io_node = node["io"];
    io_node["stdout_bytes"] = static_cast<uint64_t>(io.stdout_bytes);
    io_node["stderr_bytes"] = static_cast<uint64_t>(io.stderr_bytes);
    io_node["stdout_lines"] = static_cast<uint64_t>(io.stdout_lines);
    io_node["stderr_lines"] = static_cast<uint64_t>(io.stderr_lines);
    if (io.last_stdout_ms) {
        io_node["last_stdout_at"] = format_iso8601(io.last_stdout_ms).c_str();
    } else {
        io_node["last_stdout_at"] = (const char *)nullptr;
    }
    if (io.last_stderr_ms) {
        io_node["last_stderr_at"] = format_iso8601(io.last_stderr_ms).c_str();
    } else {
        io_node["last_stderr_at"] = (const char *)nullptr;
    }
}

void AdminServer::Impl::write_process_summary(jsini::Value &node,
                                              const std::string &name,
                                              const ProcessInfo &info) {
    auto snapshot = process_manager_.snapshot(*info.config);
    node["name"] = name.c_str();
    node["type"] = app_type_name(info.config->type);
    node["pid"] = static_cast<int64_t>(snapshot.pid);
    node["state"] = to_string(snapshot.observed);
    node["desired_state"] = to_string(snapshot.desired);
    node["ready"] = snapshot.ready;
    node["acquired"] = snapshot.acquired;
    bool running = snapshot.pid > 0;
    if (running && snapshot.started_at_ms) {
        node["started_at"] = format_iso8601(snapshot.started_at_ms).c_str();
        node["uptime_ms"] =
            static_cast<uint64_t>(Timer::now() - snapshot.started_at_ms);
    } else {
        node["started_at"] = (const char *)nullptr;
        node["uptime_ms"] = static_cast<uint64_t>(0);
    }
    node["restarts"] = static_cast<uint64_t>(
        snapshot.launches > 0 ? snapshot.launches - 1 : 0);
    node["retries"] = static_cast<int64_t>(snapshot.retries);
    if (snapshot.next_retry_at_ms) {
        node["next_retry_at"] =
            format_iso8601(snapshot.next_retry_at_ms).c_str();
    } else {
        node["next_retry_at"] = (const char *)nullptr;
    }
    if (snapshot.last_error != 0) {
        auto &last_error = node["last_error"];
        last_error["code"] = static_cast<int64_t>(snapshot.last_error);
        last_error["message"] = error_string(snapshot.last_error).c_str();
    } else {
        node["last_error"] = (const char *)nullptr;
    }
    if (snapshot.last_exit.present) {
        auto &last_exit = node["last_exit"];
        last_exit["code"] = static_cast<int64_t>(snapshot.last_exit.code);
        last_exit["signal"] = to_string(snapshot.last_exit.signal);
        last_exit["at"] = format_iso8601(snapshot.last_exit.at_ms).c_str();
    } else {
        node["last_exit"] = (const char *)nullptr;
    }
    write_io_stats(node, info.io);
}

void AdminServer::Impl::handle_list_processes(
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &) {
    jsini::Value root;
    root["timestamp"] = format_iso8601(Timer::now()).c_str();
    auto &processes = root["processes"];
    for (auto &[name, info] : processes_) {
        auto index = processes.size();
        write_process_summary(processes[index], name, info);
    }
    write_json(response, root, 200);
}

void AdminServer::Impl::handle_process_detail(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &request_id) {
    auto *info_ptr = find_process(process_name);
    if (!info_ptr) {
        write_error(response, 404, "process not found", request_id);
        return;
    }
    auto &info = *info_ptr;
    jsini::Value root;
    write_process_summary(root, process_name, info);
    if (info.config) {
        auto &command = root["command"];
        command["file"] = info.config->command.file.c_str();

        auto &args = command["args"];
        for (const auto &arg : info.config->command.args) {
            auto idx = args.size();
            args[idx] = arg.c_str();
        }

        // Keys only: values may hold credentials.
        auto &env = command["env"];
        for (const auto &e : info.config->command.env) {
            auto idx = env.size();
            env[idx] = (e.substr(0, e.find('=')) + "=<redacted>").c_str();
        }

        auto &logs = root["logs"];
        auto &settings = info.config->logs;
        logs["stdout_file"] = settings.stdout_file.empty()
                                  ? (const char *)nullptr
                                  : settings.stdout_file.c_str();
        logs["stderr_file"] = settings.stderr_file.empty()
                                  ? (const char *)nullptr
                                  : settings.stderr_file.c_str();
        auto snapshot = process_manager_.snapshot(*info.config);
        logs["error"] = snapshot.output_error.empty()
                            ? (const char *)nullptr
                            : snapshot.output_error.c_str();
    }
    write_json(response, root, 200);
}

void AdminServer::Impl::append_log_line(const std::string &process_name,
                                        int pipe, const std::string &line) {
    auto &buffer = logs_[process_name];
    LogEntry entry;
    entry.seq = next_log_seq_++;
    entry.pipe = pipe;
    entry.ts_ms = Timer::now();
    entry.text = line;
    buffer.entries.push_back(std::move(entry));
    while (buffer.entries.size() > max_tail_lines_) {
        buffer.entries.pop_front();
    }

    for (auto it = streams_.begin(); it != streams_.end();) {
        auto &stream = *it;
        if (!stream || !stream->request) {
            it = streams_.erase(it);
            continue;
        }
        if (stream->process_name != process_name) {
            ++it;
            continue;
        }
        if (pipe == 1 && !(stream->pipe_mask & 0x1)) {
            ++it;
            continue;
        }
        if (pipe == 2 && !(stream->pipe_mask & 0x2)) {
            ++it;
            continue;
        }
        if (!stream->request->client()) {
            ++it;
            continue;
        }
        auto &resp = stream->request->response();
        resp.begin_chunked_encoding();
        const auto &latest = buffer.entries.back();
        jsini::Value event;
        event["ts"] = format_iso8601(latest.ts_ms).c_str();
        event["pipe"] = pipe_name(latest.pipe).c_str();
        event["text"] = latest.text.c_str();
        std::ostringstream json_payload;
        event.dump(json_payload);
        auto json = json_payload.str();

        std::ostringstream sse;
        sse << "id: " << latest.seq << "\n";
        sse << "event: log\n";
        sse << "data: " << json << "\n\n";
        auto payload = sse.str();
        resp.write_chunk(payload.c_str(), payload.size());
        stream->last_seq = latest.seq;
        ++it;
    }
}

void AdminServer::Impl::flush_pending_log(const std::string &process_name,
                                          int pipe, std::string &buffer) {
    size_t pos = 0;
    while (true) {
        auto newline = buffer.find('\n', pos);
        if (newline == std::string::npos) {
            break;
        }
        std::string line = buffer.substr(pos, newline - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        append_log_line(process_name, pipe, line);
        pos = newline + 1;
    }
    buffer.erase(0, pos);
}

void AdminServer::Impl::on_process_data(std::shared_ptr<Process> process,
                                        int pipe, const char *data,
                                        size_t len) {
    if (!process || !data || len == 0) {
        return;
    }
    auto name = process->name();
    auto *info_ptr = find_process(name);
    if (!info_ptr) {
        return;
    }
    auto &info = *info_ptr;
    auto &log_buffer = logs_[name];
    auto now = Timer::now();

    auto newline_count =
        static_cast<uint64_t>(std::count(data, data + len, '\n'));

    if (pipe == 1) {
        info.io.stdout_bytes += len;
        info.io.stdout_lines += newline_count;
        info.io.last_stdout_ms = now;
        auto &scratch = log_buffer.partial_stdout;
        scratch.append(data, len);
        flush_pending_log(name, pipe, scratch);
    } else {
        info.io.stderr_bytes += len;
        info.io.stderr_lines += newline_count;
        info.io.last_stderr_ms = now;
        auto &scratch = log_buffer.partial_stderr;
        scratch.append(data, len);
        flush_pending_log(name, pipe, scratch);
    }
}

int AdminServer::Impl::pipe_mask_from_string(const std::string &value) {
    if (value.empty() || equals_ignore_case(value, "both")) {
        return 0x3;
    }
    if (equals_ignore_case(value, "stdout")) {
        return 0x1;
    }
    if (equals_ignore_case(value, "stderr")) {
        return 0x2;
    }
    return 0;
}

std::string AdminServer::Impl::pipe_name(int pipe) {
    return pipe == 2 ? "stderr" : "stdout";
}

void AdminServer::Impl::handle_process_logs(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id) {
    auto &log_buffer = logs_[process_name];

    struct QueryState {
        uint64_t lines = 0;
        int pipe_mask = 0x3;
        uint64_t from_seq = 0;
    } state;

    parse_query_string(request->url().query, [&](const std::string &key,
                                                 const std::string &value) {
        if (equals_ignore_case(key, "lines")) {
            state.lines =
                value.empty() ? 0 : std::strtoull(value.c_str(), nullptr, 10);
        } else if (equals_ignore_case(key, "pipe")) {
            state.pipe_mask = pipe_mask_from_string(value);
        } else if (equals_ignore_case(key, "token")) {
            state.from_seq =
                value.empty() ? 0 : std::strtoull(value.c_str(), nullptr, 10);
        }
        return 0;
    });

    state.lines = clamp(state.lines, default_tail_lines_, max_tail_lines_);

    if (state.pipe_mask == 0) {
        write_error(response, 400, "invalid pipe filter", request_id);
        return;
    }

    const auto &buffer = log_buffer.entries;
    std::vector<LogEntry> filtered;
    filtered.reserve(buffer.size());
    for (const auto &entry : buffer) {
        if (state.from_seq && entry.seq <= state.from_seq) {
            continue;
        }
        if (entry.pipe == 1 && !(state.pipe_mask & 0x1)) {
            continue;
        }
        if (entry.pipe == 2 && !(state.pipe_mask & 0x2)) {
            continue;
        }
        filtered.push_back(entry);
    }

    if (filtered.size() > state.lines) {
        filtered.erase(filtered.begin(),
                       filtered.end() - static_cast<long>(state.lines));
    }

    jsini::Value root;
    root["process"] = process_name.c_str();
    if (state.pipe_mask == 0x1) {
        root["pipe"] = "stdout";
    } else if (state.pipe_mask == 0x2) {
        root["pipe"] = "stderr";
    } else {
        root["pipe"] = "both";
    }

    auto &entries = root["entries"];
    for (const auto &entry : filtered) {
        auto index = entries.size();
        auto &item = entries[index];
        item["id"] = static_cast<uint64_t>(entry.seq);
        item["ts"] = format_iso8601(entry.ts_ms).c_str();
        item["pipe"] = pipe_name(entry.pipe).c_str();
        item["text"] = entry.text.c_str();
    }

    if (!filtered.empty()) {
        root["next_token"] = static_cast<uint64_t>(filtered.back().seq);
    }
    write_json(response, root, 200);
}

void AdminServer::Impl::handle_process_logs_stream(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id) {
    if (!settings_.logs.follow_enabled) {
        write_error(response, 503, "log streaming disabled", request_id);
        return;
    }

    auto &log_buffer = logs_[process_name];

    int pipe_mask = 0x3;
    uint64_t last_seq = 0;

    parse_query_string(request->url().query, [&](const std::string &key,
                                                 const std::string &value) {
        if (equals_ignore_case(key, "pipe")) {
            pipe_mask = pipe_mask_from_string(value);
        } else if (equals_ignore_case(key, "token")) {
            last_seq =
                value.empty() ? 0 : std::strtoull(value.c_str(), nullptr, 10);
        }
        return 0;
    });

    if (pipe_mask == 0) {
        write_error(response, 400, "invalid pipe filter", request_id);
        return;
    }

    std::string last_event_id;
    for (const auto &header : request->headers()) {
        if (equals_ignore_case(header.key, "Last-Event-Id")) {
            last_event_id = header.value;
            break;
        }
    }
    if (!last_event_id.empty()) {
        last_seq = std::strtoull(last_event_id.c_str(), nullptr, 10);
    }

    auto stream = std::make_shared<LogStream>();
    stream->process_name = process_name;
    stream->pipe_mask = pipe_mask;
    stream->last_seq = last_seq;
    stream->request = request;

    auto client = request->client();
    if (!client) {
        write_error(response, 499, "client disconnected", request_id);
        return;
    }

    response.set_status(200);
    response.set_header("Content-Type", "text/event-stream");
    response.set_header("Cache-Control", "no-cache");
    response.set_header("Connection", "keep-alive");
    response.begin_chunked_encoding();
    response.write_chunk("retry: 5000\n\n");

    streams_.push_back(stream);

    auto heartbeat = Timer::create(loop_, 15000);
    stream->heartbeat_timer = heartbeat;
    auto weak_stream = std::weak_ptr<LogStream>(stream);
    heartbeat->on_tick([weak_stream] {
        if (auto stream = weak_stream.lock()) {
            if (!stream->request) {
                return;
            }
            auto &resp = stream->request->response();
            resp.begin_chunked_encoding();
            resp.write_chunk(": keep-alive\n\n");
        }
    });
    heartbeat->start();

    client->on_close([weak_stream] {
        if (auto stream = weak_stream.lock()) {
            if (stream->heartbeat_timer) {
                stream->heartbeat_timer->stop();
                stream->heartbeat_timer->close();
                stream->heartbeat_timer.reset();
            }
            stream->request.reset();
        }
    });

    auto send_entry = [&](const LogEntry &entry) {
        jsini::Value event;
        event["ts"] = format_iso8601(entry.ts_ms).c_str();
        event["pipe"] = pipe_name(entry.pipe).c_str();
        event["text"] = entry.text.c_str();
        std::ostringstream json_payload;
        event.dump(json_payload);
        auto json = json_payload.str();

        std::ostringstream sse;
        sse << "id: " << entry.seq << "\n";
        sse << "event: log\n";
        sse << "data: " << json << "\n\n";
        auto payload = sse.str();
        response.write_chunk(payload.c_str(), payload.size());
        stream->last_seq = entry.seq;
    };

    auto &buffer = log_buffer.entries;

    if (last_seq) {
        for (const auto &entry : buffer) {
            if (entry.seq <= last_seq)
                continue;
            if (entry.pipe == 1 && !(pipe_mask & 0x1))
                continue;
            if (entry.pipe == 2 && !(pipe_mask & 0x2))
                continue;
            send_entry(entry);
        }
    } else {
        size_t count = buffer.size();
        size_t start =
            count > default_tail_lines_ ? count - default_tail_lines_ : 0;
        for (size_t i = start; i < count; ++i) {
            const auto &entry = buffer[i];
            if (entry.pipe == 1 && !(pipe_mask & 0x1))
                continue;
            if (entry.pipe == 2 && !(pipe_mask & 0x2))
                continue;
            send_entry(entry);
        }
    }
}

void AdminServer::Impl::handle_shutdown(
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id) {
    if (!runtime_) {
        write_error(response, 503, "shutdown unavailable", request_id);
        return;
    }

    uint64_t delay_ms = settings_.shutdown.default_delay_ms;
    uint64_t grace_ms = settings_.shutdown.default_grace_ms;
    uint64_t force_after_ms = settings_.shutdown.force_after_ms;
    std::string reason;

    const auto &body_buffer = request->body();
    if (body_buffer.size() > 0) {
        std::string payload(body_buffer.data(), body_buffer.size());
        jsini::Value root(payload);
        bool ok = root.is_object();
        if (ok) {
            for (auto it = root.begin(); ok && it != root.end(); it++) {
                std::string key = static_cast<const char *>(it.key());
                const jsini::Value &val = it.value();
                if (key == "delay_ms") {
                    uint64_t parsed = 0;
                    ok = parse_int64(val, parsed);
                    if (ok) {
                        delay_ms = parsed;
                    }
                } else if (key == "grace_ms") {
                    uint64_t parsed = 0;
                    ok = parse_int64(val, parsed);
                    if (ok) {
                        grace_ms = parsed;
                    }
                } else if (key == "force_after_ms") {
                    uint64_t parsed = 0;
                    ok = parse_int64(val, parsed);
                    if (ok) {
                        force_after_ms = parsed;
                    }
                } else if (key == "reason") {
                    if (!val.is_string()) {
                        ok = false;
                    } else {
                        reason = static_cast<std::string>(val);
                    }
                } else {
                    ok = false;
                }
            }
        }

        static constexpr uint64_t kMaxDurationMs = 24ULL * 60 * 60 * 1000;
        if (!ok || delay_ms > kMaxDurationMs || grace_ms > kMaxDurationMs ||
            force_after_ms > kMaxDurationMs) {
            write_error(response, 400, "invalid shutdown payload", request_id);
            return;
        }
    }

    auto now_ms = Timer::now();

    ShutdownOptions options;
    options.delay_ms = delay_ms;
    options.grace_ms = grace_ms;
    options.force_after_ms = force_after_ms;
    options.reason = reason.empty() ? "admin api" : reason;
    if (!runtime_->request_shutdown(std::move(options))) {
        write_error(response, 409, "shutdown already scheduled", request_id);
        return;
    }

    jsini::Value root;
    root["status"] = delay_ms > 0 ? "scheduled" : "shutting_down";
    root["delay_ms"] = delay_ms;
    root["grace_ms"] = grace_ms;
    root["force_after_ms"] = force_after_ms;
    root["shutdown_at"] = format_iso8601(now_ms + delay_ms).c_str();
    if (!reason.empty()) {
        root["reason"] = reason.c_str();
    }
    write_json(response, root, 202);
}

void AdminServer::Impl::handle_signal_process(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id) {
    auto *info = find_process(process_name);
    if (!info) {
        write_error(response, 404, "process not found", request_id);
        return;
    }
    auto snapshot = process_manager_.snapshot(*info->config);
    if (snapshot.pid <= 0) {
        write_error(response, 409, "process not running", request_id);
        return;
    }

    int signal = SIGTERM;
    bool signal_ok = true;
    const auto &body = request->body();
    if (body.size() > 0) {
        std::string payload(body.data(), body.size());
        jsini::Value root(payload);
        bool ok = root.is_object();
        if (ok) {
            for (auto it = root.begin(); ok && it != root.end(); it++) {
                std::string key = static_cast<const char *>(it.key());
                const jsini::Value &val = it.value();
                if (key == "signal") {
                    if (val.is_string()) {
                        auto text = static_cast<std::string>(val);
                        signal = to_signal(text, signal_ok);
                    } else {
                        int parsed = 0;
                        ok = parse_int(val, parsed);
                        if (ok) {
                            signal = parsed;
                        }
                    }
                } else {
                    ok = false;
                }
            }
        }

        if (!ok) {
            write_error(response, 400, "invalid signal payload", request_id);
            return;
        }
    }

    if (!signal_ok) {
        write_error(response, 400, "invalid signal", request_id);
        return;
    }

    int status = uv_kill(snapshot.pid, signal);
    if (status != 0) {
        write_error(response, 500, uv_strerror(status), request_id);
        return;
    }
    jsini::Value root;
    root["process"] = process_name.c_str();
    root["signal"] = to_string(signal);
    write_json(response, root, 202);
}

void AdminServer::Impl::handle_restart_process(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &request_id) {
    auto *info = find_process(process_name);
    if (!info) {
        write_error(response, 404, "process not found", request_id);
        return;
    }

    auto &operation = begin_operation(process_name, "restart");
    auto operation_id = operation.id;
    process_manager_.Restart(*info->config, operation_callback(operation_id));
    auto &result = operations_[operation_id];
    if (result.error == kErrorNotAcquired ||
        result.error == kErrorNoStopCommand) {
        write_error(response, 409, error_string(result.error), request_id);
        return;
    }

    jsini::Value root;
    root["process"] = process_name.c_str();
    root["state"] = "restarting";
    write_operation(root["operation"], result);
    write_json(response, root, 202);
}

void AdminServer::Impl::handle_start_process(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &request_id) {
    auto *info = find_process(process_name);
    if (!info) {
        write_error(response, 404, "process not found", request_id);
        return;
    }
    auto operation_id = begin_operation(process_name, "start").id;
    process_manager_.Start(*info->config, operation_callback(operation_id));

    jsini::Value root;
    root["process"] = process_name.c_str();
    root["state"] = "starting";
    write_operation(root["operation"], operations_[operation_id]);
    write_json(response, root, 202);
}

// The DELETE aliases keep `dry_run`; the former `signal` query is rejected
// because a stop is not a raw signal.
bool AdminServer::Impl::parse_stop_query(
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id,
    bool &dry_run) {
    dry_run = false;
    bool has_signal = false;
    parse_query_string(request->url().query, [&](const std::string &key,
                                                 const std::string &value) {
        if (equals_ignore_case(key, "dry_run")) {
            dry_run = equals_ignore_case(value, "true") || value == "1";
        } else if (equals_ignore_case(key, "signal")) {
            has_signal = true;
        }
        return 0;
    });
    if (has_signal) {
        write_error(response, 400,
                    "signal is not supported on the stop alias; use the "
                    "signal route",
                    request_id);
        return false;
    }
    return true;
}

void AdminServer::Impl::handle_stop_process(
    const std::string &process_name,
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id) {
    auto *info = find_process(process_name);
    if (!info) {
        write_error(response, 404, "process not found", request_id);
        return;
    }
    bool dry_run = false;
    if (!parse_stop_query(request, response, request_id, dry_run)) {
        return;
    }
    if (dry_run) {
        jsini::Value root;
        root["process"] = process_name.c_str();
        root["state"] = "planned";
        root["operation"] = (const char *)nullptr;
        write_json(response, root, 202);
        return;
    }
    auto operation_id = begin_operation(process_name, "stop").id;
    process_manager_.Stop(*info->config, operation_callback(operation_id));

    jsini::Value root;
    root["process"] = process_name.c_str();
    root["state"] = "stopping";
    write_operation(root["operation"], operations_[operation_id]);
    write_json(response, root, 202);
}

// DELETE /admin/processes stops every running app; kept as a stop alias.
void AdminServer::Impl::handle_stop_all(
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &request_id) {
    bool dry_run = false;
    if (!parse_stop_query(request, response, request_id, dry_run)) {
        return;
    }
    std::vector<std::string> targets;
    for (const auto &[name, info] : processes_) {
        auto snapshot = process_manager_.snapshot(*info.config);
        if (snapshot.pid > 0 || snapshot.observed == ObservedState::Starting ||
            snapshot.observed == ObservedState::Backoff ||
            snapshot.observed == ObservedState::Ready) {
            targets.push_back(name);
        }
    }
    jsini::Value root;
    auto &results = root["results"];
    for (const auto &name : targets) {
        auto &item = results[results.size()];
        item["process"] = name.c_str();
        if (dry_run) {
            item["state"] = "planned";
            item["operation"] = (const char *)nullptr;
            continue;
        }
        auto operation_id = begin_operation(name, "stop").id;
        process_manager_.Stop(*processes_[name].config,
                              operation_callback(operation_id));
        item["state"] = "stopping";
        write_operation(item["operation"], operations_[operation_id]);
    }
    write_json(response, root, 202);
}

AdminServer::Impl::Operation &
AdminServer::Impl::begin_operation(const std::string &process,
                                   const std::string &kind) {
    static constexpr size_t kRetainOperations = 256;
    std::ostringstream id;
    id << "op-" << std::hex << next_operation_id_++;
    Operation operation;
    operation.id = id.str();
    operation.process = process;
    operation.kind = kind;
    operation.created_at_ms = Timer::now();
    operation_order_.push_back(operation.id);
    while (operation_order_.size() > kRetainOperations) {
        operations_.erase(operation_order_.front());
        operation_order_.pop_front();
    }
    return operations_[operation.id] = std::move(operation);
}

ProcessManager::Callback
AdminServer::Impl::operation_callback(const std::string &operation_id) {
    return [this, operation_id](std::shared_ptr<Process>, int error) {
        auto it = operations_.find(operation_id);
        if (it == operations_.end()) {
            return;
        }
        auto &operation = it->second;
        operation.error = error;
        operation.state = error == 0 ? "succeeded" : "failed";
        operation.finished_at_ms = Timer::now();
        if (error != 0) {
            log_error("admin: %s of %s failed (%s)", operation.kind.c_str(),
                      operation.process.c_str(), error_string(error).c_str());
        }
    };
}

void AdminServer::Impl::write_operation(jsini::Value &node,
                                        const Operation &operation) const {
    node["id"] = operation.id.c_str();
    node["process"] = operation.process.c_str();
    node["kind"] = operation.kind.c_str();
    node["state"] = operation.state.c_str();
    node["created_at"] = format_iso8601(operation.created_at_ms).c_str();
    if (operation.finished_at_ms) {
        node["finished_at"] = format_iso8601(operation.finished_at_ms).c_str();
    } else {
        node["finished_at"] = (const char *)nullptr;
    }
    if (operation.error != 0) {
        auto &error = node["error"];
        error["code"] = static_cast<int64_t>(operation.error);
        error["message"] = error_string(operation.error).c_str();
    } else {
        node["error"] = (const char *)nullptr;
    }
}

void AdminServer::Impl::handle_operation(
    const std::string &operation_id,
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &request_id) {
    auto it = operations_.find(operation_id);
    if (it == operations_.end()) {
        write_error(response, 404, "operation not found", request_id);
        return;
    }
    jsini::Value root;
    write_operation(root, it->second);
    write_json(response, root, 200);
}

void AdminServer::Impl::handle_audit_log(
    const std::shared_ptr<http::incoming::Request> &,
    http::outgoing::Response &response, const std::string &) {
    jsini::Value root;
    auto &entries = root["entries"];
    for (const auto &entry : audit_log_) {
        auto index = entries.size();
        auto &item = entries[index];
        item["ts"] = format_iso8601(entry.ts_ms).c_str();
        item["actor"] = entry.actor.c_str();
        item["method"] = entry.method.c_str();
        item["path"] = entry.path.c_str();
        item["status"] = static_cast<int64_t>(entry.status);
        item["request_id"] = entry.request_id.c_str();
    }
    write_json(response, root, 200);
}

void AdminServer::Impl::append_audit(const std::string &actor,
                                     const std::string &remote,
                                     const std::string &method,
                                     const std::string &path, int status,
                                     const std::string &request_id) {
    AuditEntry entry;
    entry.ts_ms = Timer::now();
    entry.actor = actor;
    entry.remote = remote;
    entry.method = method;
    entry.path = path;
    entry.status = status;
    entry.request_id = request_id;
    audit_log_.push_back(std::move(entry));
    while (audit_log_.size() > audit_retention_) {
        audit_log_.pop_front();
    }
}

std::string
AdminServer::Impl::get_header_value(const http::incoming::Request &request,
                                    const std::string &name) const {
    for (const auto &header : request.headers()) {
        if (equals_ignore_case(header.key, name)) {
            return header.value;
        }
    }
    return {};
}

std::optional<std::string>
AdminServer::Impl::resolve_cors_origin(const std::string &origin) const {
    if (!settings_.cors.enabled || origin.empty() ||
        settings_.cors.allow_origins.empty()) {
        return std::nullopt;
    }
    for (const auto &allowed : settings_.cors.allow_origins) {
        if (allowed == "*") {
            if (settings_.cors.allow_credentials) {
                return origin;
            }
            return std::string("*");
        }
        if (equals_ignore_case(allowed, origin)) {
            return origin;
        }
    }
    return std::nullopt;
}

void AdminServer::Impl::apply_cors_headers(
    http::outgoing::Response &response, const std::string &allow_origin_value) {
    response.set_header("Access-Control-Allow-Origin", allow_origin_value);
    response.set_header("Vary", "Origin");
    if (!cors_allow_methods_value_.empty()) {
        response.set_header("Access-Control-Allow-Methods",
                            cors_allow_methods_value_);
    }
    if (!cors_allow_headers_value_.empty()) {
        response.set_header("Access-Control-Allow-Headers",
                            cors_allow_headers_value_);
    }
    if (settings_.cors.allow_credentials) {
        response.set_header("Access-Control-Allow-Credentials", "true");
    }
    response.set_header("Access-Control-Expose-Headers", "X-Request-Id");
}

void AdminServer::Impl::handle_cors_preflight(
    const std::shared_ptr<http::incoming::Request> &request,
    http::outgoing::Response &response, const std::string &) {
    auto private_network =
        get_header_value(*request, kHeaderRequestPrivateNetwork);
    if (!private_network.empty() &&
        equals_ignore_case(private_network, "true")) {
        response.set_header("Access-Control-Allow-Private-Network", "true");
    }
    response.end(204);
}

AdminServer::AdminServer(EventLoop &loop, Config &config,
                         ProcessManager &manager)
    : impl_(std::make_unique<Impl>(loop, config, manager)) {}

AdminServer::~AdminServer() = default;

void AdminServer::set_runtime(AdminRuntime *runtime) {
    impl_->set_runtime(runtime);
}

bool AdminServer::start() { return impl_->start(); }

void AdminServer::stop() { impl_->stop(); }

} // namespace nexer
