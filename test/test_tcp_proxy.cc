#include <assert.h>

#include <thread>

#include "http_client.h"
#include "http_server.h"
#include "runner.h"
#include "helper.h"
#include "logger.h"
#include "string_buffer.h"
#include "tcp_proxy.h"

using namespace nexer;

#define HTTP_PORT 19001
#define PROXY_PORT 19002

namespace nexer {
namespace test {

TcpProxy *proxy = nullptr;

static void start_http_server() {
    EventLoop loop;
    http::Server& server = http::Server::Create(loop);

    server.OnRequest([&server](http::incoming::Request& req, http::outgoing::Response& res) {
        res.body() << "Hello";
        res.End(200);

        if (req.url().path == "/shutdown") {
            server.Close();
        }
    });

    server.Listen(HTTP_PORT);

    loop.Run();
}

static void start_proxy_server() {
    EventLoop loop;
    config::Upstream upstream{.host = "localhost", .port = HTTP_PORT};
    proxy = &TcpProxy::Create(loop, upstream, nullptr);
    proxy->Listen(PROXY_PORT);
    loop.Run();
}

static void test_request() {
    HttpClient client;
    StringBuffer sb;
    auto url_of = [&sb](const char* path) {
        sb.Clear();
        sb.Printf("http://localhost:%d%s", PROXY_PORT, path);
        return sb.data();
    };

    auto res = client.Get(url_of("/hello"));
    assert(res->body() == "Hello");
}

static void shutdown_servers() {
    HttpClient client;
    StringBuffer sb;

    auto url_of = [&sb](int port) {
        sb.Clear();
        sb.Printf("http://localhost:%d/shutdown", port);
        return sb.data();
    };

    auto res = client.Get(url_of(PROXY_PORT));
    assert(res->status() == 200);
    assert(res->body() == "Hello");

    proxy->Close();
}

std::string sample_config = R"conf({
    proxies: [
      {
        listen: 19500,
        upstream: {
            host: '127.0.0.1',
            port: 19501,
            connect_timeout: 1000,
            app: {
                command: {
                    file: ./build/run_test,
                    args: [ helper, sleep ],
                    env: [ NAME=a ]
                }
                checker: {
                    file: ./build/run_test,
                    args: [ helper, sleep ],
                    env: [ NAME=b ]
                }
                max_start_time: 50
            }
        }
      }
    ]
})conf";

struct Context {
    Config config;
    EventLoop loop;
    ProcessManager manager;
    TcpProxy* proxy;
    TcpServer* server;
    struct {
        std::string client_data;
    } server_data;

    Context() : manager(loop) {
        assert(Config::Parse(config, sample_config));
        auto& conf = config.proxies()[0];
        proxy = &nexer::TcpProxy::Create(loop, conf.upstream, &manager);
        proxy->Listen(conf.port);
    }

    void StartServer() {
        auto& conf = config.proxies()[0];
        server = &nexer::TcpServer::Create(loop);
        server->Listen(conf.upstream.port);
        server->OnConnection([&](TcpClient& client) {
            client.OnData([&](const char* s, size_t len) {
                server_data.client_data.append(s, len);
                client.Write(server_data.client_data.data(), server_data.client_data.size());
            });
        });
    }
};


// upstream check failed, incoming waiting
static void TestUpstreamCheckFailure() {
    Context context;

    // exits 1 immediately
    SetScenario("simple-fail");
    delete ((config::App*)(context.config.proxies()[0].upstream.app))->checker;
    ((config::App*)(context.config.proxies()[0].upstream.app))->checker = nullptr;

    auto& client = nexer::TcpClient::Create(context.loop);
    client.Connect(19500);

    bool closed = false;
    client.OnClose([&] {
        closed = true;
    });

    auto& timer = nexer::Timer::Create(context.loop, 2000);
    timer.OnTick([&] {
        timer.Close();
        context.proxy->Close();
    });

    timer.Start();
    context.loop.Run();
    assert(closed);
}

// upstream check failed, incoming gone
static void TestUpstreamCheckFailure2() {
    Context context;

    // exits 1 after 500ms
    SetScenario("simple-with-sleep-fail");

    auto& client = nexer::TcpClient::Create(context.loop);

    client.Connect(19500);
    {
        auto& timer = nexer::Timer::Create(context.loop, 200);
        timer.OnTick([&] {
            timer.Close();
            client.Close();
        });
        timer.Start();
    }

    bool closed = false;
    client.OnClose([&] {
        closed = true;
    });

    auto& timer = nexer::Timer::Create(context.loop, 2000);
    timer.OnTick([&] {
        timer.Close();
        context.proxy->Close();
    });
    timer.Start();

    context.loop.Run();

    assert(closed);
}

// upstream connection failed, incoming waiting
static void TestUpstreamConnectFailure() {
    Context context;

    SetScenario("simple-check");

    auto& client = nexer::TcpClient::Create(context.loop);

    client.Connect(19500);

    bool closed = false;
    client.OnClose([&] {
        closed = true;
    });

    auto& timer = nexer::Timer::Create(context.loop, 2000);
    timer.OnTick([&] {
        timer.Close();
        context.proxy->Close();
    });
    timer.Start();

    context.loop.Run();

    assert(closed);
}

// upstream connection failed, incoming gone
static void TestUpstreamConnectFailure2() {
    Context context;

    SetScenario("simple-check");

    auto& client = nexer::TcpClient::Create(context.loop);

    client.Connect(19500);
    {
        auto& timer = nexer::Timer::Create(context.loop, 200);
        timer.OnTick([&] {
            timer.Close();
            client.Close();
        });
        timer.Start();
    }

    bool closed = false;
    client.OnClose([&] {
        closed = true;
    });

    auto& timer = nexer::Timer::Create(context.loop, 2000);
    timer.OnTick([&] {
        timer.Close();
        context.proxy->Close();
    });
    timer.Start();

    context.loop.Run();

    assert(closed);
}

// upstream connection succeeded, incoming waiting
static void TestUpstreamConnectSuccess() {
    Context context;

    SetScenario("simple-check");

    auto& client = nexer::TcpClient::Create(context.loop);

    client.Connect(19500);

    context.StartServer();

    bool closed = false;
    std::string data;

    client.OnClose([&] {
        closed = true;
    });

    client.OnConnect([&] {
        client.Write("hello", 5);
    });

    client.OnData([&](const char *s, size_t len) {
        data.append(s, len);
        if (data.size() == 5) {
            client.Close();
        }
    });

    auto& timer = nexer::Timer::Create(context.loop, 2000);
    timer.OnTick([&] {
        timer.Close();
        context.proxy->Close();
        context.server->Close();
    });
    timer.Start();

    context.loop.Run();

    assert(closed);
    assert(data == "hello");
}

// upstream connection succeeded, incoming gone
static void TestUpstreamConnectSuccess2() {
    Context context;

    SetScenario("simple-check");

    auto& client = nexer::TcpClient::Create(context.loop);

    client.Connect(19500);
    {
        auto& timer = nexer::Timer::Create(context.loop, 200);
        timer.OnTick([&] {
            timer.Close();
            client.Close();
        });
        timer.Start();
    }

    context.StartServer();

    bool closed = false;
    std::string data;

    client.OnClose([&] {
        closed = true;
    });

    client.OnConnect([&] {
        client.Write("hello", 5);
    });

    client.OnData([&](const char *s, size_t len) {
        data.append(s, len);
        if (data.size() == 5) {
            client.Close();
        }
    });

    auto& timer = nexer::Timer::Create(context.loop, 2000);
    timer.OnTick([&] {
        timer.Close();
        context.proxy->Close();
        context.server->Close();
    });
    timer.Start();

    context.loop.Run();

    assert(closed);
}

void TestTcpProxy() {
    // std::thread t1(start_http_server);
    // std::thread t2(start_proxy_server);
    // test_request();
    // shutdown_servers();
    // t1.join();
    // t2.join();

    TestUpstreamCheckFailure();
    TestUpstreamCheckFailure2();
    TestUpstreamConnectFailure();
    TestUpstreamConnectFailure2();
    TestUpstreamConnectSuccess();
    TestUpstreamConnectSuccess2();
}

}  // namespace test
}  // namespace nexer
