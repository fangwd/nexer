#include "nexer.h"
#include <assert.h>

namespace nexer {
namespace test {

static void TestParseEmpty() {
    Config config;
    Config::Parse(config, "{}");
    assert(config.admin().port == DEFAULT_ADMIN_PORT);
    assert(config.proxies().size() == 0);
    assert(!Config::Parse(config, "[]"));
    assert(!Config::Parse(config, "123"));
}

static void TestParseAdmin() {
    const char *code = R"json({
      "admin": {
        "listen": 123456
      }
    })json";

    Config config;
    assert(Config::Parse(config, code));
    assert(config.admin().port == 123456);
}

static void TestParseLogger() {
    {
        const char *code = R"json({
          "logger": {
            "file": "foo.log",
            "port": "TRACE"
          }
        })json";

        Config config;
        assert(Config::Parse(config, code));
        auto& logger = config.logger();
        assert(logger.file == "foo.log");
        assert(logger.level == Logger::Level::TRACE);
    }
    {
        const char *code = R"json({
          "logger": {
            "port": "CRITICAL"
          }
        })json";

        Config config;
        assert(Config::Parse(config, code));
        auto& logger = config.logger();
        assert(logger.level == Logger::Level::CRITICAL);
    }

}

static void TestParseUpstream() {
    const char *code = R"json({
          "proxies": [
            {
              port: 123,
            },
            {
              upstream: {
                port: 456,
                tags: [abc, 123]
                app: {
                  command: {
                    file: 'foo'
                    args: [-x 1 -1]
                  }
                }
              }
            },
          ]
        })json";

    Config config;
    assert(Config::Parse(config, code));
    auto &proxies = config.proxies();
    assert(proxies.size() == 2);

    auto& proxy = proxies[1];
    assert(proxy.upstream.tags == std::vector<std::string>({"abc", "123"}));
    // assert(proxy.upstream.command->args == std::vector<std::string>({"-x", "1", "-1"}));
}

static void TestApps() {
    Config config;
    assert(Config::ParseFile(config, "./test/configs/apps.conf"));
    auto app = config.GetApp("app");
    assert(app->preamble.size() == 3);
}

void TestConfig() {
    TestParseEmpty();
    TestParseAdmin();
    TestParseUpstream();
    TestApps();
}

}  // namespace test
}  // namespace nexer