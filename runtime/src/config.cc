#include "nexer/runtime/config.h"
#include "jsini.hpp"

#include <assert.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace nexer {

Config::Config() { logger_.level = Logger::Level::INFO; }

Config::~Config() {
    for (auto &app : apps_) {
        delete app->checker;
        delete app->stop_command;
        delete app;
    }
}

typedef jsini::Value::Iterator::Key ConfigKey;
typedef uint8_t JsonType;

class ConfigParser {
  private:
    Config &config_;
    jsini::Value value_;

    bool Parse(jsini::Value &value, const char *name,
               std::function<bool(ConfigKey &key, jsini::Value &)> fn) {
        if (!value.is_object()) {
            Error(value, name, JSINI_TOBJECT);
            return false;
        }
        for (jsini::Value::Iterator it = value.begin(); it != value.end();
             it++) {
            auto key = it.key();
            if (!fn(key, it.value())) {
                return false;
            }
        }
        return true;
    }

    bool Parse(jsini::Value &value, const char *name,
               std::function<bool(jsini::Value &)> fn) {
        if (!value.is_array()) {
            Error(value, name, JSINI_TARRAY);
            return false;
        }
        for (size_t i = 0; i < value.size(); i++) {
            if (!fn(value[i])) {
                return false;
            }
        }
        return true;
    }

    inline bool Parse(jsini::Value &src, int &dst) {
        if (src.type() == JSINI_TINTEGER) {
            dst = (int)src;
            return true;
        }
        return false;
    }

    inline bool Parse(jsini::Value &src, uint64_t &dst) {
        if (src.type() == JSINI_TINTEGER || src.type() == JSINI_TNUMBER) {
            dst = static_cast<uint64_t>(
                static_cast<long long>(static_cast<int>(src)));
            return true;
        }
        return false;
    }

    inline bool Parse(jsini::Value &src, bool &dst) {
        if (src.type() == JSINI_TBOOL) {
            dst = (bool)src;
            return true;
        }
        return false;
    }

    inline bool Parse(jsini::Value &src, std::string &dst, bool force = false) {
        if (src.type() == JSINI_TSTRING) {
            dst = (const char *)src;
            return true;
        }
        if (force) {
            std::stringstream ss;
            src.dump(ss, 0, 0);
            dst = ss.str();
            return true;
        }
        return false;
    }

    bool Parse(jsini::Value &src, const char *name,
               std::vector<std::string> &dst) {
        return Parse(src, name, [&](jsini::Value &value) {
            dst.emplace_back();
            return Parse(value, dst.back(), true);
        });
    }

    void Error(const jsini::Value &value, const char *name,
               JsonType expected_type) {
        if (expected_type != JSINI_UNDEFINED && expected_type != value.type()) {
            auto expected = jsini_type_name(expected_type);
            auto got = jsini_type_name(value.type());
            log_error("Bad %s (line %u: expected %s, got %s)", name,
                      value.lineno(), expected, got);
            return;
        }
        log_error("Bad %s (line %u)", name, value.lineno());
    }

    void Error(const ConfigKey &key, const char *name) {
        log_error("Unknown %s config: %s (line %u)", name, (const char *)key,
                  key.lineno());
    }

  public:
    ConfigParser(Config &config, const std::string &code)
        : config_(config), value_(code) {}
    ConfigParser(Config &config, const char *file)
        : config_(config), value_(file) {}

    bool Parse() {
        if (value_.type() == JSINI_UNDEFINED) {
            log_error("Error pasing config");
            return false;
        }
        bool ok =
            Parse(value_, "config", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "logger") {
                    ok = Parse(value, config_.logger_);
                } else if (key == "http") {
                    ok = Parse(value, config_.http_);
                } else if (key == "forwarder") {
                    ok = Parse(value, config_.forwarder_);
                } else if (key == "proxies") {
                    ok = Parse(value, config_.proxies_);
                } else if (key == "apps") {
                    ok = Parse(value, config_.apps_);
                } else if (key == "dummy") {
                    ok = Parse(value, config_.dummies_);
                } else if (key == "admin") {
                    ok = Parse(value, config_.admin_);
                } else if (key == "startup_timeout_ms") {
                    ok = ParseDuration(value, "startup_timeout_ms",
                                       config_.startup_timeout_ms_);
                } else {
                    log_error("Unknown config entry: %s (line %u)",
                              (const char *)key, key.lineno());
                }
                return ok;
            });
        if (!ok) {
            return false;
        }
        for (auto *app : config_.apps_) {
            if (!app->restart_policy_explicit) {
                app->restart.policy = app->keep_alive
                                          ? config::Restart::Policy::OnFailure
                                          : config::Restart::Policy::Never;
            }
            if (!ValidateAppType(*app)) {
                return false;
            }
        }
        if (!config_.config_dir_.empty()) {
            ResolvePaths();
        }
        return ValidatePreambleGraph();
    }

    // Relative paths are rebased onto the config directory. A relative
    // command file containing a slash is rebased only when no cwd is set,
    // because with a cwd it is relative to that directory at exec time.
    void ResolvePaths() {
        Resolve(config_.logger_.file);
        Resolve(config_.admin_.auth_token_file);
        for (auto *app : config_.apps_) {
            ResolveCommand(app->command);
            if (app->checker) {
                ResolveCommand(*app->checker);
            }
            if (app->stop_command) {
                ResolveCommand(*app->stop_command);
            }
            Resolve(app->logs.stdout_file);
            Resolve(app->logs.stderr_file);
        }
    }

    void ResolveCommand(config::Command &command) {
        bool had_cwd = !command.cwd.empty();
        Resolve(command.cwd);
        if (!had_cwd && command.file.find('/') != std::string::npos) {
            Resolve(command.file);
        }
    }

    void Resolve(std::string &path) {
        if (path.empty() || std::filesystem::path(path).is_absolute()) {
            return;
        }
        path = (std::filesystem::path(config_.config_dir_) / path)
                   .lexically_normal()
                   .string();
    }

    bool ValidateAppType(const config::App &app) {
        const char *name = app.name.empty() ? "<unnamed>" : app.name.c_str();
        bool automatic_restart =
            app.restart.policy != config::Restart::Policy::Never;
        switch (app.type) {
        case config::App::Type::Service:
            if (app.stop_command) {
                log_error("Bad app %s: stop_command requires type external",
                          name);
                return false;
            }
            return true;
        case config::App::Type::Task:
            if (app.checker) {
                log_error("Bad app %s: a task cannot have a checker", name);
                return false;
            }
            if (automatic_restart) {
                log_error("Bad app %s: a task cannot restart automatically",
                          name);
                return false;
            }
            if (app.stop_command) {
                log_error("Bad app %s: stop_command requires type external",
                          name);
                return false;
            }
            if (app.command.file.empty()) {
                log_error("Bad app %s: a task requires a command", name);
                return false;
            }
            return true;
        case config::App::Type::External:
            if (!app.checker) {
                log_error("Bad app %s: an external resource requires a checker",
                          name);
                return false;
            }
            if (automatic_restart) {
                log_error("Bad app %s: an external resource cannot restart "
                          "automatically",
                          name);
                return false;
            }
            return true;
        }
        return true;
    }

  private:
    bool Parse(jsini::Value &value, config::HttpLimits &config) {
        return Parse(value, "http", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "max_header_bytes") {
                if (!(ok = Parse(value, config.max_header_bytes))) {
                    Error(value, "http max_header_bytes", JSINI_TINTEGER);
                }
            } else if (key == "max_body_bytes") {
                if (!(ok = Parse(value, config.max_body_bytes))) {
                    Error(value, "http max_body_bytes", JSINI_TINTEGER);
                }
            } else if (key == "max_response_bytes") {
                if (!(ok = Parse(value, config.max_response_bytes))) {
                    Error(value, "http max_response_bytes", JSINI_TINTEGER);
                }
            } else {
                Error(key, "http");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value &value, config::ForwarderLimits &config) {
        return Parse(
            value, "forwarder", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "max_buffer_bytes") {
                    if (!(ok = Parse(value, config.max_buffer_bytes))) {
                        Error(value, "forwarder max_buffer_bytes",
                              JSINI_TINTEGER);
                    }
                } else {
                    Error(key, "forwarder");
                }
                return ok;
            });
    }

    bool Parse(jsini::Value &value, config::Logger &config) {
        return Parse(value, "logger", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "file") {
                if (!(ok = Parse(value, config.file))) {
                    Error(value, "log file", JSINI_TSTRING);
                }
            } else if (key == "level") {
                std::string level;
                if (!(ok = Parse(value, level))) {
                    Error(value, "log level", JSINI_TSTRING);
                } else if (!Logger::parse_level(config.level, level.data())) {
                    Error(value, "log level", JSINI_UNDEFINED);
                }
            } else {
                Error(key, "logger");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value &value, std::vector<config::Proxy> &proxies) {
        return Parse(value, "proxies", [&](jsini::Value &value) {
            proxies.emplace_back();
            return Parse(value, proxies.back());
        });
    }

    bool Parse(jsini::Value &value, config::Proxy &proxy) {
        return Parse(value, "proxy", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "upstream") {
                ok = Parse(value, proxy.upstream);
            } else if (key == "listen") {
                if (!(ok = Parse(value, proxy.port))) {
                    Error(value, "forward listening port", JSINI_TINTEGER);
                }
            } else {
                Error(key, "proxy");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value &value, config::Upstream &upstream) {
        return Parse(
            value, "upstream", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "host") {
                    if (!(ok = Parse(value, upstream.host))) {
                        Error(value, "upstream host", JSINI_TSTRING);
                    }
                } else if (key == "port") {
                    if (!(ok = Parse(value, upstream.port))) {
                        Error(value, "upstream port", JSINI_TINTEGER);
                    }
                } else if (key == "connect_timeout") {
                    if (!(ok = Parse(value, upstream.connect_timeout))) {
                        Error(value, "upstream connect timeout",
                              JSINI_TINTEGER);
                    }
                } else if (key == "app") {
                    if (!(ok = ((upstream.app = ParseApp(value)) != nullptr))) {
                        Error(value, "upstream app", JSINI_UNDEFINED);
                    }
                } else if (key == "tags") {
                    ok = Parse(value, "upstream tags", upstream.tags);
                } else {
                    Error(key, "upstream");
                }
                return ok;
            });
    }

    config::App *ParseApp(jsini::Value &value) {
        if (value.is_string()) {
            std::string name = value;
            return name.size() > 0 ? config_.app(name) : nullptr;
        }

        config::App tmp;
        if (Parse(value, tmp)) {
            if (tmp.name.size() > 0) {
                auto app = config_.app(tmp.name);
                delete app->checker;
                delete app->stop_command;
                app->checker = nullptr;
                app->stop_command = nullptr;
                *app = std::move(tmp);
                return app;
            }
            auto app = new config::App();
            *app = std::move(tmp);
            config_.apps_.push_back(app);
            return app;
        }

        delete tmp.checker;
        delete tmp.stop_command;
        return nullptr;
    }

    bool Parse(jsini::Value &value, config::Command &cmd) {
        return Parse(
            value, "command", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "file") {
                    if (!(ok = Parse(value, cmd.file))) {
                        Error(value, "command file", JSINI_TSTRING);
                    }
                } else if (key == "args") {
                    ok = Parse(value, "command args", cmd.args);
                } else if (key == "env") {
                    ok = Parse(value, "command env", cmd.env);
                } else if (key == "cwd") {
                    if (!(ok = Parse(value, cmd.cwd))) {
                        Error(value, "command cwd", JSINI_TSTRING);
                    }
                } else if (key == "timeout") {
                    if (!(ok = Parse(value, cmd.timeout))) {
                        Error(value, "command timeout", JSINI_TINTEGER);
                    }
                } else {
                    Error(key, "command");
                }
                return ok;
            });
    }

    bool Parse(jsini::Value &value, std::vector<config::App *> &apps) {
        return Parse(value, "apps", [&](jsini::Value &value) {
            auto app = ParseApp(value);
            if (app == nullptr) {
                Error(value, "app", JSINI_UNDEFINED);
                return false;
            }
            return true;
        });
    }

    bool Parse(jsini::Value &value, config::App &app) {
        std::string dependency_key;
        return Parse(value, "app", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "name") {
                if (!(ok = Parse(value, app.name, true))) {
                    Error(value, "app file", JSINI_TSTRING);
                }
            } else if (key == "command") {
                ok = Parse(value, app.command);
            } else if (key == "type") {
                std::string type;
                if (!(ok = Parse(value, type))) {
                    Error(value, "app type", JSINI_TSTRING);
                } else if (type == "service") {
                    app.type = config::App::Type::Service;
                } else if (type == "task") {
                    app.type = config::App::Type::Task;
                } else if (type == "external") {
                    app.type = config::App::Type::External;
                } else {
                    log_error("Unknown app type: %s (line %u)", type.c_str(),
                              value.lineno());
                    ok = false;
                }
            } else if (key == "checker") {
                if (app.checker) {
                    delete app.checker;
                }
                app.checker = new config::Command();
                app.checker->timeout = 10000;
                ok = Parse(value, *app.checker);
            } else if (key == "stop_command") {
                if (app.stop_command) {
                    delete app.stop_command;
                }
                app.stop_command = new config::Command();
                ok = Parse(value, *app.stop_command);
            } else if (key == "max_start_time") {
                if (!(ok = Parse(value, app.max_start_time))) {
                    Error(value, "app max_start_time", JSINI_TINTEGER);
                }
            } else if (key == "preamble" || key == "depends_on") {
                std::string key_name = static_cast<const char *>(key);
                if (!dependency_key.empty() && dependency_key != key_name) {
                    log_error("Bad app: preamble and depends_on cannot both "
                              "be set (line %u)",
                              key.lineno());
                    return false;
                }
                dependency_key = key_name;
                return Parse(value, "app preamble", [&](jsini::Value &value) {
                    auto preamble = ParseApp(value);
                    if (preamble == nullptr) {
                        Error(value, "app preamble", JSINI_UNDEFINED);
                        return false;
                    }
                    app.preamble.push_back(preamble);
                    return true;
                });
            } else if (key == "tags") {
                ok = Parse(value, "app tags", app.tags);
            } else if (key == "auto_start") {
                if (!(ok = Parse(value, app.auto_start))) {
                    Error(value, "app auto_start", JSINI_TBOOL);
                }
            } else if (key == "keep_alive") {
                if (!(ok = Parse(value, app.keep_alive))) {
                    Error(value, "app keep_alive", JSINI_TBOOL);
                }
            } else if (key == "restart") {
                ok = Parse(value, app.restart, app.restart_policy_explicit);
            } else if (key == "logs") {
                ok = Parse(value, app.logs);
            } else {
                Error(key, "app");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value &value, config::Restart &restart,
               bool &policy_explicit) {
        bool ok = Parse(
            value, "app restart", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "policy") {
                    policy_explicit = true;
                    std::string policy;
                    if (!(ok = Parse(value, policy))) {
                        Error(value, "app restart policy", JSINI_TSTRING);
                    } else if (policy == "never") {
                        restart.policy = config::Restart::Policy::Never;
                    } else if (policy == "on_failure") {
                        restart.policy = config::Restart::Policy::OnFailure;
                    } else if (policy == "always") {
                        restart.policy = config::Restart::Policy::Always;
                    } else {
                        log_error("Unknown app restart policy: %s (line %u)",
                                  policy.c_str(), value.lineno());
                        ok = false;
                    }
                } else if (key == "delay_ms") {
                    ok = ParseDuration(value, "app restart delay_ms",
                                       restart.delay_ms);
                } else if (key == "max_delay_ms") {
                    ok = ParseDuration(value, "app restart max_delay_ms",
                                       restart.max_delay_ms);
                } else if (key == "max_retries") {
                    if (!(ok = Parse(value, restart.max_retries)) ||
                        restart.max_retries < 0) {
                        Error(value, "app restart max_retries", JSINI_TINTEGER);
                        ok = false;
                    }
                } else if (key == "window_ms") {
                    ok = ParseDuration(value, "app restart window_ms",
                                       restart.window_ms);
                    if (ok && restart.window_ms == 0) {
                        Error(value, "app restart window_ms", JSINI_UNDEFINED);
                        ok = false;
                    }
                } else {
                    Error(key, "app restart");
                }
                return ok;
            });
        if (ok && restart.max_delay_ms < restart.delay_ms) {
            log_error("Bad app restart: max_delay_ms is smaller than "
                      "delay_ms (line %u)",
                      value.lineno());
            return false;
        }
        return ok;
    }

    bool Parse(jsini::Value &value, config::OutputLog &logs) {
        return Parse(
            value, "app logs", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "stdout_file") {
                    if (!(ok = Parse(value, logs.stdout_file))) {
                        Error(value, "app logs stdout_file", JSINI_TSTRING);
                    }
                } else if (key == "stderr_file") {
                    if (!(ok = Parse(value, logs.stderr_file))) {
                        Error(value, "app logs stderr_file", JSINI_TSTRING);
                    }
                } else if (key == "max_bytes") {
                    long long raw = 0;
                    if (value.type() == JSINI_TINTEGER) {
                        raw = static_cast<long long>(value);
                    }
                    if (value.type() != JSINI_TINTEGER || raw <= 0) {
                        Error(value, "app logs max_bytes", JSINI_TINTEGER);
                    } else {
                        logs.max_bytes = static_cast<uint64_t>(raw);
                        ok = true;
                    }
                } else if (key == "max_files") {
                    if (!(ok = Parse(value, logs.max_files)) ||
                        logs.max_files < 0) {
                        Error(value, "app logs max_files", JSINI_TINTEGER);
                        ok = false;
                    }
                } else {
                    Error(key, "app logs");
                }
                return ok;
            });
    }

    // Nonnegative, bounded (at most one day) millisecond duration.
    bool ParseDuration(jsini::Value &value, const char *name, uint64_t &dst) {
        static constexpr long long kMaxDurationMs = 24LL * 60 * 60 * 1000;
        if (value.type() != JSINI_TINTEGER) {
            Error(value, name, JSINI_TINTEGER);
            return false;
        }
        long long raw = static_cast<long long>(value);
        if (raw < 0 || raw > kMaxDurationMs) {
            log_error("Bad %s (line %u: expected 0..%lld)", name,
                      value.lineno(), kMaxDurationMs);
            return false;
        }
        dst = static_cast<uint64_t>(raw);
        return true;
    }

    bool Parse(jsini::Value &value, std::vector<config::Dummy> &dummies) {
        return Parse(value, "dummies", [&](jsini::Value &value) {
            dummies.emplace_back();
            return Parse(value, dummies.back());
        });
    }

    bool Parse(jsini::Value &value, config::Dummy &dummy) {
        return Parse(value, "dummy", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "type") {
                ok = Parse(value, dummy.type);
            } else if (key == "listen") {
                if (!(ok = Parse(value, dummy.port))) {
                    Error(value, "dummy server port", JSINI_TINTEGER);
                }
            } else if (key == "echo") {
                if (!(ok = Parse(value, dummy.echo))) {
                    Error(value, "dummy echo", JSINI_TBOOL);
                }
            } else {
                Error(key, "dummy");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value &value, config::Dummy::Type &protocol) {
        if (!value.is_string()) {
            Error(value, "dummy server protocol", JSINI_TSTRING);
            return false;
        }
        if (value == "tcp") {
            protocol = config::Dummy::Type::Tcp;
            return true;
        }
        if (value == "udp") {
            protocol = config::Dummy::Type::Udp;
            return true;
        }
        if (value == "http") {
            protocol = config::Dummy::Type::Http;
            return true;
        }
        log_error("Unknown dummy server protocol: %s (line %u)",
                  (const char *)value, value.lineno());
        return false;
    }

    bool Parse(jsini::Value &value, config::Admin &admin) {
        return Parse(value, "admin", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "enabled") {
                if (!(ok = Parse(value, admin.enabled))) {
                    Error(value, "admin enabled", JSINI_TBOOL);
                }
            } else if (key == "bind") {
                if (!(ok = Parse(value, admin.bind))) {
                    Error(value, "admin bind", JSINI_TSTRING);
                }
            } else if (key == "port") {
                int port;
                if (!(ok = Parse(value, port))) {
                    Error(value, "admin port", JSINI_TINTEGER);
                } else {
                    admin.port = port;
                }
            } else if (key == "auth_header") {
                if (!(ok = Parse(value, admin.auth_header))) {
                    Error(value, "admin auth_header", JSINI_TSTRING);
                }
            } else if (key == "auth_token_env") {
                if (!(ok = Parse(value, admin.auth_token_env))) {
                    Error(value, "admin auth_token_env", JSINI_TSTRING);
                }
            } else if (key == "auth_token_file") {
                if (!(ok = Parse(value, admin.auth_token_file))) {
                    Error(value, "admin auth_token_file", JSINI_TSTRING);
                }
            } else if (key == "require_tls") {
                if (!(ok = Parse(value, admin.require_tls))) {
                    Error(value, "admin require_tls", JSINI_TBOOL);
                }
            } else if (key == "health_require_auth") {
                if (!(ok = Parse(value, admin.health_require_auth))) {
                    Error(value, "admin health_require_auth", JSINI_TBOOL);
                }
            } else if (key == "shutdown") {
                ok = Parse(value, admin.shutdown);
            } else if (key == "logs") {
                ok = Parse(value, admin.logs);
            } else if (key == "audit") {
                ok = Parse(value, admin.audit);
            } else if (key == "cors") {
                ok = Parse(value, admin.cors);
            } else {
                Error(key, "admin");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value &value, config::Admin::Shutdown &shutdown) {
        return Parse(
            value, "admin shutdown", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "default_grace_ms") {
                    ok = ParseDuration(value, "admin shutdown default_grace_ms",
                                       shutdown.default_grace_ms);
                } else if (key == "force_after_ms") {
                    ok = ParseDuration(value, "admin shutdown force_after_ms",
                                       shutdown.force_after_ms);
                } else if (key == "default_delay_ms") {
                    ok = ParseDuration(value, "admin shutdown default_delay_ms",
                                       shutdown.default_delay_ms);
                } else {
                    Error(key, "admin shutdown");
                }
                return ok;
            });
    }

    bool Parse(jsini::Value &value, config::Admin::Logs &logs) {
        return Parse(
            value, "admin logs", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "default_tail_lines") {
                    uint64_t tmp;
                    if (!(ok = Parse(value, tmp))) {
                        Error(value, "admin logs default_tail_lines",
                              JSINI_TINTEGER);
                    } else {
                        logs.default_tail_lines = static_cast<size_t>(tmp);
                    }
                } else if (key == "max_tail_lines") {
                    uint64_t tmp;
                    if (!(ok = Parse(value, tmp))) {
                        Error(value, "admin logs max_tail_lines",
                              JSINI_TINTEGER);
                    } else {
                        logs.max_tail_lines = static_cast<size_t>(tmp);
                    }
                } else if (key == "follow_enabled") {
                    if (!(ok = Parse(value, logs.follow_enabled))) {
                        Error(value, "admin logs follow_enabled", JSINI_TBOOL);
                    }
                } else {
                    Error(key, "admin logs");
                }
                return ok;
            });
    }

    bool Parse(jsini::Value &value, config::Admin::Audit &audit) {
        return Parse(
            value, "admin audit", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "retain_entries") {
                    uint64_t tmp;
                    if (!(ok = Parse(value, tmp))) {
                        Error(value, "admin audit retain_entries",
                              JSINI_TINTEGER);
                    } else {
                        audit.retain_entries = static_cast<size_t>(tmp);
                    }
                } else {
                    Error(key, "admin audit");
                }
                return ok;
            });
    }

    bool Parse(jsini::Value &value, config::Admin::Cors &cors) {
        return Parse(
            value, "admin cors", [&](ConfigKey &key, jsini::Value &value) {
                bool ok = false;
                if (key == "enabled") {
                    if (!(ok = Parse(value, cors.enabled))) {
                        Error(value, "admin cors enabled", JSINI_TBOOL);
                    }
                } else if (key == "allow_origins") {
                    cors.allow_origins.clear();
                    ok = Parse(value, "admin cors allow_origins",
                               cors.allow_origins);
                } else if (key == "allow_methods") {
                    cors.allow_methods.clear();
                    ok = Parse(value, "admin cors allow_methods",
                               cors.allow_methods);
                } else if (key == "allow_headers") {
                    cors.allow_headers.clear();
                    ok = Parse(value, "admin cors allow_headers",
                               cors.allow_headers);
                } else if (key == "allow_credentials") {
                    if (!(ok = Parse(value, cors.allow_credentials))) {
                        Error(value, "admin cors allow_credentials",
                              JSINI_TBOOL);
                    }
                } else {
                    Error(key, "admin cors");
                }
                return ok;
            });
    }

    bool ValidatePreambleGraph() {
        std::set<const config::App *> visited;
        std::set<const config::App *> visiting;
        for (auto *app : config_.apps_) {
            if (!ValidatePreambleDFS(app, visited, visiting)) {
                return false;
            }
        }
        return true;
    }

    bool ValidatePreambleDFS(const config::App *app,
                             std::set<const config::App *> &visited,
                             std::set<const config::App *> &visiting) {
        if (visited.count(app)) {
            return true;
        }
        if (visiting.count(app)) {
            log_error("Cyclic preamble dependency detected: %s",
                      app->name.c_str());
            return false;
        }
        visiting.insert(app);
        for (auto *dep : app->preamble) {
            if (!ValidatePreambleDFS(dep, visited, visiting)) {
                return false;
            }
        }
        visiting.erase(app);
        visited.insert(app);
        return true;
    }
};

bool Config::parse_file(Config &config, const char *file) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(file, ec);
    config.config_dir_ =
        ec ? std::filesystem::absolute(file).parent_path().string()
           : canonical.parent_path().string();
    ConfigParser parser(config, file);
    return parser.Parse();
}

bool Config::parse(Config &config, const std::string &code) {
    ConfigParser parser(config, code);
    return parser.Parse();
}

config::App *Config::app(const std::string &name) {
    if (name.size() > 0) {
        auto it = app_map_.find(name);
        if (it != app_map_.end()) {
            return it->second;
        }
    }

    auto app = new config::App();
    app->name = name;
    apps_.push_back(app);

    if (name.size() > 0) {
        app_map_[name] = app;
    }

    return app;
}

} // namespace nexer
