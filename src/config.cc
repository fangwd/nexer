/* Copyright (c) Weidong Fang. All rights reserved. */

#include "config.h"
#include "jsini.hpp"

#include <fstream>
#include <sstream>
#include <assert.h>

namespace nexer {

Config::Config() {
    logger_.file = "nexer.log";
    logger_.level = Logger::Level::INFO;
    admin_.port = DEFAULT_ADMIN_PORT;
}

Config::~Config() {
    for (auto& app: apps_) {
        delete app->checker;
        delete app;
    }
}

typedef jsini::Value::Iterator::Key ConfigKey;
typedef uint8_t JsonType;

class ConfigParser {
  private:
    Config &config_;
    jsini::Value value_;

    bool Parse(jsini::Value &value, const char *name, std::function<bool(ConfigKey& key, jsini::Value &)> fn) {
        if (!value.is_object()) {
            Error(value, name, JSINI_TOBJECT);
            return false;
        }
        for (jsini::Value::Iterator it = value.begin(); it != value.end(); it++) {
            auto key = it.key();
            if (!fn(key, it.value())) {
                return false;
            }
        }
        return true;
    }

    bool Parse(jsini::Value &value, const char *name, std::function<bool(jsini::Value &)> fn) {
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

    bool Parse(jsini::Value &src, const char *name, std::vector<std::string> &dst) {
        return Parse(src, name, [&](jsini::Value& value) {
            dst.emplace_back();
            return Parse(value, dst.back(), true);
        });
    }

    void Error(const jsini::Value &value, const char *name, JsonType expected_type) {
        if (expected_type != JSINI_UNDEFINED && expected_type != value.type()) {
            auto expected = jsini_type_name(expected_type);
            auto got = jsini_type_name(value.type());
            log_error("Bad %s (line %u: expected %s, got %s)", name, value.lineno(), expected, got);
            return;
        }
        log_error("Bad %s (line %u)", name, value.lineno());
    }

    void Error(const ConfigKey& key, const char *name) {
        log_error("Unknown %s config: %s (line %u)", name, (const char *) key, key.lineno());
    }

  public:
    ConfigParser(Config &config, const std::string &code) : config_(config), value_(code) {}
    ConfigParser(Config &config, const char *file) : config_(config), value_(file) {}

    bool Parse() {
        if (value_.type() == JSINI_UNDEFINED) {
            log_error("Error pasing config");
            return false;
        }
        return Parse(value_, "config", [&](ConfigKey &key, jsini::Value& value) {
            bool ok = false;
            if (key == "admin") {
                ok = Parse(value, config_.admin_);
            } else if (key == "logger") {
                ok = Parse(value, config_.logger_);
            } else if (key == "proxies") {
                ok = Parse(value, config_.proxies_);
            } else if (key == "apps") {
                ok = Parse(value, config_.apps_);
            } else {
                log_error("Unknown config entry: %s (line %u)", (const char *) key, key.lineno());
            }
            return ok;
        });
    }

  private:
    bool Parse(jsini::Value &value, config::Admin &config) {
        return Parse(value, "admin", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "listen") {
                if (!(ok = Parse(value, config.port))) {
                    Error(value, "admin port", JSINI_TINTEGER);
                }
            } else {
                Error(key, "admin");
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
                } else if (!Logger::ParseLevel(config.level, level.data())) {
                    Error(value, "log level", JSINI_UNDEFINED);
                }
            } else {
                Error(key, "logger");
            }
            return ok;
        });
    }

    bool Parse(jsini::Value& value, std::vector<config::Proxy> &proxies) {
        return Parse(value, "proxies", [&](jsini::Value& value) {
            proxies.emplace_back();
            return Parse(value, proxies.back());
        });
    }

    bool Parse(jsini::Value &value, config::Proxy &proxy) {
        return Parse(value, "proxy", [&](ConfigKey &key, jsini::Value& value) {
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
        return Parse(value, "upstream", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "host") {
                if (!(ok = Parse(value, upstream.host))) {
                    Error(value, "upstream host", JSINI_TSTRING);
                }
            } else if (key == "port") {
                if (!(ok = Parse(value, upstream.port))) {
                    Error(value, "upstream port", JSINI_TINTEGER);
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
            return name.size() > 0 ? config_.GetApp(name) : nullptr;
        }

        config::App tmp;
        if (Parse(value, tmp)) {
            if (tmp.name.size() > 0) {
                auto app = config_.GetApp(tmp.name);
                *app = std::move(tmp);
                return app;
            }
            auto app = new config::App();
            *app = std::move(tmp);
            config_.apps_.push_back(app);
            return app;
        }

        return nullptr;
    }

    bool Parse(jsini::Value &value, config::Command &cmd) {
        return Parse(value, "command", [&](ConfigKey &key, jsini::Value &value) {
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
                ok = Parse(value, "command env", cmd.env);
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
        return Parse(value, "app", [&](ConfigKey &key, jsini::Value &value) {
            bool ok = false;
            if (key == "name") {
                if (!(ok = Parse(value, app.name, true))) {
                    Error(value, "app file", JSINI_TSTRING);
                }
            } else if (key == "command") {
                ok = Parse(value, app.command);
            } else if (key == "checker") {
                app.checker = new config::Command();
                app.checker->timeout = 10000;
                ok = Parse(value, *app.checker);
            } else if (key == "max_start_time") {
                if (!(ok = Parse(value, app.max_start_time))) {
                    Error(value, "app max_start_time", JSINI_TINTEGER);
                }
            } else if (key == "preamble") {
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
            } else {
                Error(key, "app");
            }
            return ok;
        });
    }
};

bool Config::ParseFile(Config &config, const char *file) {
    ConfigParser parser(config, file);
    return parser.Parse();
}

bool Config::Parse(Config &config, const std::string &code) {
    ConfigParser parser(config, code);
    return parser.Parse();
}

config::App* Config::GetApp(const std::string &name) {
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

}  // namespace nexer
