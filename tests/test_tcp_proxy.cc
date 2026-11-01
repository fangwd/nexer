#include <assert.h>

#include <list>

#include "helper.h"
#include "nexer/core/string_buffer.h"
#include "nexer/http/http_client.h"
#include "nexer/http/http_server.h"
#include "nexer/runtime/tcp_proxy.h"

using namespace nexer;

#define HTTP_PORT 19001
#define PROXY_PORT 19002

namespace nexer {
namespace test {

std::shared_ptr<TcpProxy> proxy = nullptr;

static void start_http_server() {
    EventLoop loop;
    auto server = http::Server::create(loop);

    server->on_request([server](std::shared_ptr<http::incoming::Request> req,
                                http::outgoing::Response &res) {
        res.body() << "Hello";
        res.end(200);

        if (req->url().path == "/shutdown") {
            server->close();
        }
    });

    server->Listen(HTTP_PORT);

    loop.run();
}

static void start_proxy_server() {
    EventLoop loop;
    config::Upstream upstream{.host = "localhost", .port = HTTP_PORT};
    proxy = TcpProxy::create(loop, upstream, nullptr);
    proxy->Listen(PROXY_PORT);
    loop.run();
}

static void test_request() {
    HttpClient client;
    StringBuffer sb;
    auto url_of = [&sb](const char *path) {
        sb.Clear();
        sb.Printf("http://localhost:%d%s", PROXY_PORT, path);
        return sb.data();
    };

    auto res = client.get(url_of("/hello"));
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

    auto res = client.get(url_of(PROXY_PORT));
    assert(res->status() == 200);
    assert(res->body() == "Hello");

    proxy->close();
}

std::string sample_config = R"conf({
    proxies: [
      {
        listen: 19500,
        upstream: {
            host: '127.0.0.1',
            port: 19501,
            connect_timeout: 200,
            app: {
                command: {
                    file: ./build/run_tests,
                    args: [ helper, sleep ],
                    env: [ NAME=a ]
                }
                checker: {
                    file: ./build/run_tests,
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
    std::shared_ptr<TcpProxy> proxy;
    std::shared_ptr<TcpServer> server;
    std::list<std::shared_ptr<TcpClient>> accepted_clients;
    struct {
        std::string client_data;
    } server_data;

    Context(size_t max_buffer_bytes = 2 * 1024 * 1024, bool disable_app = false)
        : manager(loop) {
        assert(Config::parse(config, sample_config));
        auto &conf = config.proxies()[0];
        int listen_port = PickFreePort();
        int upstream_port = PickFreePort();
        if (listen_port > 0) {
            conf.port = listen_port;
        }
        if (upstream_port > 0) {
            conf.upstream.port = upstream_port;
        }
        if (disable_app) {
            conf.upstream.app = nullptr;
        }
        proxy = nexer::TcpProxy::create(loop, conf.upstream, &manager,
                                        max_buffer_bytes);
        proxy->Listen(conf.port);
    }

    ~Context() {
        for (auto &client : accepted_clients) {
            if (client && !client->is_closing()) {
                client->close();
            }
        }
        accepted_clients.clear();

        if (proxy) {
            proxy->shutdown();
            if (!proxy->is_closing()) {
                proxy->close();
            }
            proxy.reset();
        }

        if (server) {
            if (!server->is_closing()) {
                server->close();
            }
            server.reset();
        }

        manager.Shutdown();

        // Drain any pending close callbacks so libuv releases resources
        // cleanly.
        loop.stop();
        loop.run();
    }

    void StartServer() {
        auto &conf = config.proxies()[0];
        server = TcpServer::Create(loop);
        server->Listen(conf.upstream.port);
        server->OnConnection([this](std::shared_ptr<TcpClient> client) {
            accepted_clients.push_back(client);
            std::weak_ptr<TcpClient> weak_client = client;
            client->on_data([this, weak_client](const char *s, size_t len) {
                if (auto client = weak_client.lock()) {
                    server_data.client_data.append(s, len);
                    client->write(server_data.client_data.data(),
                                  server_data.client_data.size());
                }
            });
            client->on_close([this, weak_client]() {
                if (auto client = weak_client.lock()) {
                    accepted_clients.remove(client);
                }
            });
        });
    }
};

// upstream check failed, incoming waiting
static void TestUpstreamCheckFailure() {
    Context context;

    // exits 1 immediately
    SetScenario("simple-fail");
    delete ((config::App *)(context.config.proxies()[0].upstream.app))->checker;
    ((config::App *)(context.config.proxies()[0].upstream.app))->checker =
        nullptr;

    auto client = nexer::TcpClient::create(context.loop);
    client->connect(context.config.proxies()[0].port);

    bool closed = false;
    client->on_close([&] {
        closed = true;
        context.loop.stop();
    });

    auto timer = nexer::Timer::create(context.loop, 1000);
    std::weak_ptr<TcpProxy> weak_p = context.proxy;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_p, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (auto p = weak_p.lock()) {
            p->close();
        }
    });

    timer->start();
    context.loop.run();
    if (timer && !timer->is_closing()) {
        timer->stop();
        timer->close();
    }
    assert(closed);
}

// upstream check failed, incoming gone
static void TestUpstreamCheckFailure2() {
    Context context;

    // exits 1 after 500ms
    auto client = nexer::TcpClient::create(context.loop);

    client->connect(context.config.proxies()[0].port);
    auto timer200 = nexer::Timer::create(context.loop, 60);
    std::weak_ptr<TcpClient> weak_client = client;
    std::weak_ptr<Timer> weak_timer200 = timer200;
    timer200->on_tick([weak_client, weak_timer200] {
        if (auto timer = weak_timer200.lock()) {
            timer->close();
        }
        if (auto client = weak_client.lock()) {
            client->close();
        }
    });
    timer200->start();

    bool closed = false;
    client->on_close([&] { closed = true; });

    auto timer = nexer::Timer::create(context.loop, 300);
    auto p = context.proxy;
    std::weak_ptr<TcpProxy> weak_p = p;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_p, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (auto p = weak_p.lock()) {
            p->close();
        }
    });
    timer->start();

    context.loop.run();
    if (timer200 && !timer200->is_closing()) {
        timer200->stop();
        timer200->close();
    }
    if (timer && !timer->is_closing()) {
        timer->stop();
        timer->close();
    }

    assert(closed);
}

static void TestForwarderBufferLimitCloses() {
    Context context(64, true);

    auto client = nexer::TcpClient::create(context.loop);
    client->connect(context.config.proxies()[0].port);

    bool closed = false;
    client->on_close([&] { closed = true; });

    client->on_connect([client]() {
        std::string payload(256, 'x');
        client->write(payload.data(), payload.size());
    });

    auto timer = nexer::Timer::create(context.loop, 1000);
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_timer, &context] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        context.loop.stop();
    });
    timer->start(false);

    context.loop.run();
    assert(closed);

    context.proxy->close();
    if (context.server) {
        context.server->close();
    }
    context.loop.run();
}

// upstream connection failed, incoming waiting
static void TestUpstreamConnectFailure() {
    Context context;

    SetScenario("simple-check");

    auto client = nexer::TcpClient::create(context.loop);

    client->connect(context.config.proxies()[0].port);

    bool closed = false;
    client->on_close([&] { closed = true; });

    auto timer = nexer::Timer::create(context.loop, 300);
    auto p = context.proxy;
    std::weak_ptr<TcpProxy> weak_p = p;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_p, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (auto p = weak_p.lock()) {
            p->close();
        }
    });
    timer->start();

    context.loop.run();
    if (timer && !timer->is_closing()) {
        timer->stop();
        timer->close();
    }

    assert(closed);
}

// upstream connection failed, incoming gone
static void TestUpstreamConnectFailure2() {
    Context context;

    SetScenario("simple-check");

    auto client = nexer::TcpClient::create(context.loop);

    client->connect(context.config.proxies()[0].port);
    auto timer200 = nexer::Timer::create(context.loop, 60);
    std::weak_ptr<TcpClient> weak_client = client;
    std::weak_ptr<Timer> weak_timer200 = timer200;
    timer200->on_tick([weak_client, weak_timer200] {
        if (auto timer = weak_timer200.lock()) {
            timer->close();
        }
        if (auto client = weak_client.lock()) {
            client->close();
        }
    });
    timer200->start();

    bool closed = false;
    client->on_close([&] { closed = true; });

    auto timer = nexer::Timer::create(context.loop, 300);
    auto p = context.proxy;
    std::weak_ptr<TcpProxy> weak_p = p;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_p, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (auto p = weak_p.lock()) {
            p->close();
        }
    });
    timer->start();

    context.loop.run();
    if (timer200 && !timer200->is_closing()) {
        timer200->stop();
        timer200->close();
    }
    if (timer && !timer->is_closing()) {
        timer->stop();
        timer->close();
    }

    assert(closed);
}

// upstream connection succeeded, incoming waiting
static void TestUpstreamConnectSuccess() {
    Context context;

    SetScenario("simple-check");

    auto client = nexer::TcpClient::create(context.loop);

    client->connect(context.config.proxies()[0].port);

    context.StartServer();

    bool closed = false;
    std::string data;

    client->on_close([&] { closed = true; });

    client->on_connect([client] { client->write("hello", 5); });

    client->on_data([&, client](const char *s, size_t len) {
        data.append(s, len);
        if (data.size() == 5) {
            client->close();
        }
    });

    client->on_error([&, client](int err, const char *msg) {
        fprintf(stderr, "client error: %d (%s)\n", err, msg);
        client->close();
    });

    auto timer = nexer::Timer::create(context.loop, 300);
    auto p = context.proxy;
    auto s = context.server;
    std::weak_ptr<TcpProxy> weak_p = p;
    std::weak_ptr<TcpServer> weak_s = s;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_p, weak_s, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (auto p = weak_p.lock()) {
            p->close();
        }
        if (auto s = weak_s.lock()) {
            s->close();
        }
    });
    timer->start();

    context.loop.run();

    assert(closed);
    assert(data == "hello");
}

// upstream connection succeeded, incoming gone
static void TestUpstreamConnectSuccess2() {
    Context context;

    SetScenario("simple-check");

    auto client = nexer::TcpClient::create(context.loop);

    client->connect(context.config.proxies()[0].port);
    auto timer200 = nexer::Timer::create(context.loop, 60);
    std::weak_ptr<TcpClient> weak_client = client;
    std::weak_ptr<Timer> weak_timer200 = timer200;
    timer200->on_tick([weak_client, weak_timer200] {
        if (auto timer = weak_timer200.lock()) {
            timer->close();
        }
        if (auto client = weak_client.lock()) {
            client->close();
        }
    });
    timer200->start();

    context.StartServer();

    bool closed = false;
    std::string data;

    client->on_close([&] { closed = true; });

    client->on_connect([client] { client->write("hello", 5); });

    client->on_data([&, client](const char *s, size_t len) {
        data.append(s, len);
        if (data.size() == 5) {
            client->close();
        }
    });

    client->on_error([&, client](int err, const char *msg) {
        fprintf(stderr, "client error: %d (%s)\n", err, msg);
        client->close();
    });

    auto timer = nexer::Timer::create(context.loop, 300);
    auto p = context.proxy;
    auto server = context.server;
    std::weak_ptr<TcpProxy> weak_p = p;
    std::weak_ptr<TcpServer> weak_server = server;
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([weak_p, weak_server, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        if (auto p = weak_p.lock()) {
            p->close();
        }
        if (auto s = weak_server.lock()) {
            s->close();
        }
    });
    timer->start();

    context.loop.run();

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
    TestForwarderBufferLimitCloses();
    TestUpstreamConnectFailure();
    TestUpstreamConnectFailure2();
    TestUpstreamConnectSuccess();
    TestUpstreamConnectSuccess2();
}

} // namespace test
} // namespace nexer
