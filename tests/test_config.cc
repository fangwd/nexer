#include <assert.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "nexer/runtime/nexer.h"

namespace nexer {
namespace test {

static void TestParseEmpty() {
    Config config;
    Config::parse(config, "{}");
    assert(config.proxies().size() == 0);
    assert(!Config::parse(config, "[]"));
    assert(!Config::parse(config, "123"));
}

static void TestParseLogger() {
    {
        const char *code = R"json({
          "logger": {
            "file": "foo.log",
            "level": "TRACE"
          }
        })json";

        Config config;
        assert(Config::parse(config, code));
        auto &logger = config.logger();
        assert(logger.file == "foo.log");
        assert(logger.level == Logger::Level::TRACE);
    }
    {
        const char *code = R"json({
          "logger": {
            "level": "CRITICAL"
          }
        })json";

        Config config;
        assert(Config::parse(config, code));
        auto &logger = config.logger();
        assert(logger.level == Logger::Level::CRITICAL);
    }
}

static void TestParseUpstream() {
    const char *code = R"json({
          "proxies": [
            {
              listen: 123,
              upstreams: [{ port: 1 }],
            },
            {
              bind: '0.0.0.0',
              upstreams: [
                {
                  port: 456,
                  tags: [abc, 123],
                  app: {
                    command: {
                      file: 'foo',
                      args: [-x, 1, -1]
                    }
                  }
                },
                { port: 789 },
              ]
            },
          ]
        })json";

    Config config;
    assert(Config::parse(config, code));
    auto &proxies = config.proxies();
    assert(proxies.size() == 2);
    assert(proxies[0].bind == "127.0.0.1");

    auto &proxy = proxies[1];
    assert(proxy.bind == "0.0.0.0");
    assert(proxy.upstreams.size() == 2);
    assert(proxy.upstreams[0].tags == std::vector<std::string>({"abc", "123"}));
    assert(proxy.upstreams[0].app != nullptr);
    assert(proxy.upstreams[1].port == 789);
    assert(proxy.upstreams[1].app == nullptr);

    Config empty;
    assert(!Config::parse(empty, R"json({ proxies: [{ listen: 1 }] })json"));
    Config old_key;
    assert(!Config::parse(
        old_key,
        R"json({ proxies: [{ listen: 1, upstream: { port: 2 } }] })json"));
}

static void TestParseProxyProtocol() {
    Config config;
    assert(Config::parse(config, R"json({ proxies: [
        { listen: 1, upstreams: [{ port: 2 }] },
        { listen: 3, protocol: 'http+socks', upstreams: [{ port: 4 }] },
    ] })json"));
    assert(config.proxies()[0].protocol == config::Proxy::Protocol::Tcp);
    assert(config.proxies()[1].protocol == config::Proxy::Protocol::HttpSocks);

    Config unknown;
    assert(!Config::parse(unknown, R"json({ proxies: [
        { listen: 1, protocol: 'socks', upstreams: [{ port: 2 }] },
    ] })json"));
}

static void TestApps() {
    Config config;
    assert(Config::parse_file(config, "./tests/configs/apps.conf"));
    auto app = config.app("app");
    assert(app->preamble.size() == 3);
}

static void TestCommandCwdAndDummyHttp() {
    const char *code = R"json({
      apps: [
        {
          name: app-http,
          command: {
            file: '/bin/true',
            cwd: '/tmp/app-http',
            args: [--flag],
            env: ['KEY=VALUE']
          }
        }
      ],
      dummy: [
        {
          type: http,
          listen: 8080
        }
      ]
    })json";

    Config config;
    assert(Config::parse(config, code));
    auto app = config.app("app-http");
    assert(app != nullptr);
    assert(app->command.cwd == "/tmp/app-http");
    assert(app->command.file == "/bin/true");
    assert(config.dummies().size() == 1);
    assert(config.dummies()[0].type == config::Dummy::Type::Http);
    assert(config.dummies()[0].port == 8080);
}

static void TestCyclicPreamble() {
    // Direct cycle: A -> B -> C -> A
    {
        const char *code = R"json({
          apps: [
            {
              name: app-a,
              command: { file: a },
              preamble: [app-b]
            },
            {
              name: app-b,
              command: { file: b },
              preamble: [app-c]
            },
            {
              name: app-c,
              command: { file: c },
              preamble: [app-a]
            }
          ]
        })json";

        Config config;
        assert(!Config::parse(config, code));
    }
    // Self-cycle: A -> A
    {
        const char *code = R"json({
          apps: [
            {
              name: app-a,
              command: { file: a },
              preamble: [app-a]
            }
          ]
        })json";

        Config config;
        assert(!Config::parse(config, code));
    }
    // Acyclic diamond: A -> B, A -> C, B -> D, C -> D (should pass)
    {
        const char *code = R"json({
          apps: [
            {
              name: app-d,
              command: { file: d }
            },
            {
              name: app-b,
              command: { file: b },
              preamble: [app-d]
            },
            {
              name: app-c,
              command: { file: c },
              preamble: [app-d]
            },
            {
              name: app-a,
              command: { file: a },
              preamble: [app-b, app-c]
            }
          ]
        })json";

        Config config;
        assert(Config::parse(config, code));
    }
}

static void TestHttpAndForwarderLimits() {
    const char *code = R"json({
      http: {
        max_header_bytes: 4096,
        max_body_bytes: 8192,
        max_response_bytes: 16384
      },
      forwarder: {
        max_buffer_bytes: 32768
      }
    })json";

    Config config;
    assert(Config::parse(config, code));
    assert(config.http().max_header_bytes == 4096);
    assert(config.http().max_body_bytes == 8192);
    assert(config.http().max_response_bytes == 16384);
    assert(config.forwarder().max_buffer_bytes == 32768);
}

static void TestStartupAndShutdownDurations() {
    {
        Config config;
        assert(Config::parse(config, "{}"));
        assert(config.startup_timeout_ms() == 180000);
        assert(config.admin().shutdown.default_grace_ms == 5000);
        assert(config.admin().shutdown.force_after_ms == 30000);
    }
    {
        Config config;
        const char *code = R"conf({
            startup_timeout_ms: 0,
            admin: { shutdown: { default_grace_ms: 10, force_after_ms: 20,
                                 default_delay_ms: 30 } }
        })conf";
        assert(Config::parse(config, code));
        assert(config.startup_timeout_ms() == 0);
        assert(config.admin().shutdown.default_grace_ms == 10);
        assert(config.admin().shutdown.force_after_ms == 20);
        assert(config.admin().shutdown.default_delay_ms == 30);
    }
    const char *bad[] = {
        "{ startup_timeout_ms: -1 }",
        "{ startup_timeout_ms: 99999999999 }",
        "{ startup_timeout_ms: '5s' }",
        "{ admin: { shutdown: { default_grace_ms: -5 } } }",
        "{ admin: { shutdown: { force_after_ms: 99999999999 } } }",
    };
    for (auto *code : bad) {
        Config config;
        assert(!Config::parse(config, code));
    }
}

static void TestAppTypes() {
    {
        Config config;
        const char *code = R"conf({
            apps: [
                { name: s, command: { file: x } },
                { name: t, type: task, command: { file: x } },
                { name: e, type: external, checker: { file: x },
                  command: { file: launch }, stop_command: { file: stop, args: [now] } },
                { name: e2, type: external, checker: { file: x } }
            ]
        })conf";
        assert(Config::parse(config, code));
        assert(config.app("s")->type == config::App::Type::Service);
        assert(config.app("t")->type == config::App::Type::Task);
        auto *e = config.app("e");
        assert(e->type == config::App::Type::External);
        assert(e->stop_command && e->stop_command->file == "stop");
        assert(e->stop_command->args == std::vector<std::string>({"now"}));
        assert(config.app("e2")->stop_command == nullptr);
    }
    const char *bad[] = {
        "{ apps: [{ name: a, type: daemon, command: { file: x } }] }",
        "{ apps: [{ name: a, type: task, command: { file: x }, checker: { "
        "file: c } }] }",
        "{ apps: [{ name: a, type: task, command: { file: x }, keep_alive: "
        "true }] }",
        "{ apps: [{ name: a, type: task, command: { file: x }, restart: { "
        "policy: always } }] }",
        "{ apps: [{ name: a, type: task, command: { file: x }, stop_command: { "
        "file: s } }] }",
        "{ apps: [{ name: a, type: task }] }",
        "{ apps: [{ name: a, type: external, command: { file: x } }] }",
        "{ apps: [{ name: a, type: external, checker: { file: c }, keep_alive: "
        "true }] }",
        "{ apps: [{ name: a, command: { file: x }, stop_command: { file: s } "
        "}] }",
    };
    for (auto *code : bad) {
        Config config;
        assert(!Config::parse(config, code));
    }
}

static void TestRelativePathsResolveAgainstConfigDir() {
    auto dir = std::filesystem::temp_directory_path() /
               ("nexer-config-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir / "sub");
    auto file = dir / "sub" / "nexer.conf";
    {
        std::ofstream out(file);
        out << R"conf({
            logger: { file: logs/nexer.log },
            admin: { auth_token_file: ../token },
            apps: [
                { name: a, command: { file: ./bin/start.sh, cwd: ./work } },
                { name: b, command: { file: ./bin/start.sh } },
                { name: c, command: { file: npm, args: [run, dev] },
                  checker: { file: ./check.sh, cwd: '/abs' },
                  logs: { stdout_file: logs/c.out } }
            ]
        })conf";
    }
    Config config;
    assert(Config::parse_file(config, file.c_str()));
    auto base = std::filesystem::weakly_canonical(dir / "sub");
    assert(config.config_dir() == base.string());
    assert(config.logger().file == (base / "logs/nexer.log").string());
    assert(config.admin().auth_token_file ==
           (base.parent_path() / "token").string());
    // With a cwd the command file stays relative to that (resolved) cwd.
    assert(config.app("a")->command.cwd == (base / "work").string());
    assert(config.app("a")->command.file == "./bin/start.sh");
    assert(config.app("b")->command.file == (base / "bin/start.sh").string());
    assert(config.app("c")->command.file == "npm");
    assert(config.app("c")->checker->cwd == "/abs");
    assert(config.app("c")->checker->file == "./check.sh");
    assert(config.app("c")->logs.stdout_file == (base / "logs/c.out").string());
    std::filesystem::remove_all(dir);

    // Parsing from a string keeps paths untouched.
    Config inline_config;
    assert(Config::parse(
        inline_config,
        "{ apps: [{ name: a, command: { file: ./x, cwd: w } }] }"));
    assert(inline_config.config_dir().empty());
    assert(inline_config.app("a")->command.cwd == "w");
}

static void TestDependsOnAliasAndLogs() {
    {
        Config config;
        const char *code = R"conf({
            apps: [
                { name: db, command: { file: x } },
                { name: api, command: { file: x }, depends_on: [db],
                  logs: { stdout_file: out.log, stderr_file: err.log,
                          max_bytes: 1024, max_files: 2 } },
                { name: web, command: { file: x }, preamble: [api] },
                { name: plain, command: { file: x } }
            ]
        })conf";
        assert(Config::parse(config, code));
        assert(config.app("api")->preamble.size() == 1);
        assert(config.app("api")->preamble[0] == config.app("db"));
        assert(config.app("web")->preamble[0] == config.app("api"));
        auto &logs = config.app("api")->logs;
        assert(logs.enabled());
        assert(logs.stdout_file == "out.log" && logs.stderr_file == "err.log");
        assert(logs.max_bytes == 1024 && logs.max_files == 2);
        auto &defaults = config.app("plain")->logs;
        assert(!defaults.enabled());
        assert(defaults.max_bytes == 10 * 1024 * 1024 &&
               defaults.max_files == 5);
    }
    const char *bad[] = {
        "{ apps: [{ name: a, command: { file: x }, preamble: [b], depends_on: "
        "[b] }, { name: b, command: { file: x } }] }",
        "{ apps: [{ name: a, command: { file: x }, logs: { max_bytes: 0 } }] }",
        "{ apps: [{ name: a, command: { file: x }, logs: { max_files: -1 } }] "
        "}",
        "{ apps: [{ name: a, command: { file: x }, logs: { file: out.log } }] "
        "}",
    };
    for (auto *code : bad) {
        Config config;
        assert(!Config::parse(config, code));
    }
}

void TestConfig() {
    TestDependsOnAliasAndLogs();
    TestStartupAndShutdownDurations();
    TestRelativePathsResolveAgainstConfigDir();
    TestAppTypes();
    TestParseEmpty();
    TestParseLogger();
    TestParseUpstream();
    TestParseProxyProtocol();
    TestApps();
    TestCommandCwdAndDummyHttp();
    TestHttpAndForwarderLimits();
    TestCyclicPreamble();
}

} // namespace test
} // namespace nexer
