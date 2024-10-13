#ifndef NEXER_CONFIG_H_
#define NEXER_CONFIG_H_

#include <map>
#include <string>
#include <vector>

#include "logger.h"

#define DEFAULT_ADMIN_PORT 19500

namespace nexer {

class Process;

namespace config {

struct Command {
    std::string file;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::string cwd;

    int timeout = 0;
};

struct App {
    std::string name;
    Command command;
    Command *checker = nullptr;
    int max_start_time = 0;
    std::vector<const App *> preamble;
    std::vector<std::string> tags;
};

struct Upstream {
    std::string host;
    int port = 0;
    int connect_timeout = 30000;
    const App *app;
    std::vector<std::string> tags;
};

struct Proxy {
    int port = 0;
    Upstream upstream;
};

struct Admin {
    int port = 0;
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

}  // namespace config

class Config {
  private:
    config::Admin admin_;
    config::Logger logger_;
    std::vector<config::Proxy> proxies_;
    std::vector<config::App *> apps_;
    std::map<std::string, config::App *> app_map_;

    std::vector<config::Dummy> dummies_;

    friend class ConfigParser;

  public:
    Config();
    ~Config();

    static bool ParseFile(Config &, const char *file);
    static bool Parse(Config &, const std::string &code);

    inline auto& apps() {
        return apps_;
    }
    inline auto& proxies() {
        return proxies_;
    }
    inline auto& admin() {
        return admin_;
    }
    inline auto& dummies() {
        return dummies_;
    }
    inline auto& logger() {
        return logger_;
    }

    config::App *GetApp(const std::string &name);
};

}  // namespace nexer

#endif  // NEXER_CONFIG_H_
