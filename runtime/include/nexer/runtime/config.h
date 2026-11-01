#ifndef NEXER_CONFIG_H_
#define NEXER_CONFIG_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nexer/core/logger.h"

namespace nexer {

class Process;

namespace config {

struct HttpLimits {
    int max_header_bytes = 64 * 1024;
    int max_body_bytes = 2 * 1024 * 1024;
    int max_response_bytes = 2 * 1024 * 1024;
};

struct ForwarderLimits {
    int max_buffer_bytes = 2 * 1024 * 1024;
};

struct Command {
    std::string file;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::string cwd;

    int timeout = 0;
};

struct OutputLog {
    std::string stdout_file;
    std::string stderr_file;
    uint64_t max_bytes = 10 * 1024 * 1024;
    int max_files = 5;
    inline bool enabled() const {
        return !stdout_file.empty() || !stderr_file.empty();
    }
};

struct Restart {
    enum class Policy { Never, OnFailure, Always };
    Policy policy = Policy::Never;
    uint64_t delay_ms = 1000;
    uint64_t max_delay_ms = 30000;
    int max_retries = 5;
    uint64_t window_ms = 60000;
};

struct App {
    // service: long-running owned process (default). task: finite command
    // whose zero exit means success. external: resource verified by a
    // required checker, optionally launched and stopped by commands.
    enum class Type { Service, Task, External };
    std::string name;
    Type type = Type::Service;
    Command command;
    Command *checker = nullptr;
    Command *stop_command = nullptr;
    int max_start_time = 0;
    std::vector<const App *> preamble;
    std::vector<std::string> tags;
    bool auto_start = false;
    bool keep_alive = false;
    // Without an explicit `restart.policy` the policy is derived from
    // `keep_alive` once parsing completes.
    bool restart_policy_explicit = false;
    Restart restart;
    OutputLog logs;
};

struct Upstream {
    std::string host;
    int port = 0;
    int connect_timeout = 30000;
    const App *app = nullptr;
    std::vector<std::string> tags;
};

struct Proxy {
    int port = 0;
    Upstream upstream;
};

struct Logger {
    std::string file;
    ::Logger::Level level = ::Logger::Level::INFO;
};

struct Dummy {
    enum class Type {
        Udp,
        Tcp,
        Http,
    };
    Type type = Type::Tcp;
    int port = 0;
    bool echo = false;
};

struct Admin {
    struct Shutdown {
        uint64_t default_grace_ms = 5000;
        uint64_t force_after_ms = 30000;
        uint64_t default_delay_ms = 0;
    } shutdown;

    struct Logs {
        size_t default_tail_lines = 500;
        size_t max_tail_lines = 5000;
        bool follow_enabled = true;
    } logs;

    struct Audit {
        size_t retain_entries = 1000;
    } audit;

    struct Cors {
        bool enabled = false;
        std::vector<std::string> allow_origins;
        std::vector<std::string> allow_methods = {"GET", "POST", "DELETE",
                                                  "OPTIONS"};
        std::vector<std::string> allow_headers = {
            "Content-Type", "Authorization", "X-Nexer-Admin-Token",
            "X-Requested-With"};
        bool allow_credentials = false;
    } cors;

    bool enabled = false;
    std::string bind = "127.0.0.1";
    int port = 8301;
    std::string auth_header = "X-Nexer-Admin-Token";
    std::string auth_token_env = "NEXER_ADMIN_TOKEN";
    std::string auth_token_file;
    bool require_tls = false;
    bool health_require_auth = false;
};

} // namespace config

class Config {
  private:
    config::Logger logger_;
    config::HttpLimits http_;
    config::ForwarderLimits forwarder_;
    std::vector<config::Proxy> proxies_;
    std::vector<config::App *> apps_;
    std::map<std::string, config::App *> app_map_;

    std::vector<config::Dummy> dummies_;
    config::Admin admin_;
    // Deadline for all auto_start apps and their dependencies; zero means
    // unlimited.
    uint64_t startup_timeout_ms_ = 180000;
    // Directory of the parsed config file; relative paths resolve against
    // it. Empty when parsed from a string.
    std::string config_dir_;

    friend class ConfigParser;

  public:
    Config();
    ~Config();

    static bool parse_file(Config &, const char *file);
    static bool parse(Config &, const std::string &code);

    inline auto &apps() { return apps_; }
    inline auto &proxies() { return proxies_; }
    inline auto &dummies() { return dummies_; }
    inline auto &logger() { return logger_; }
    inline auto &http() { return http_; }
    inline auto &forwarder() { return forwarder_; }
    inline auto &admin() { return admin_; }
    inline auto &startup_timeout_ms() { return startup_timeout_ms_; }
    inline const std::string &config_dir() const { return config_dir_; }

    config::App *app(const std::string &name);
};

#define NEXER_CONFIG "nexer.conf"

} // namespace nexer

#endif // NEXER_CONFIG_H_
