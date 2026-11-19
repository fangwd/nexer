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
    config::Proxy conf;
    conf.upstreams.push_back({.host = "localhost", .port = HTTP_PORT});
    proxy = TcpProxy::create(loop, conf, nullptr);
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
        upstreams: [{
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
        }]
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

    Context(size_t max_buffer_bytes = 2 * 1024 * 1024, bool disable_app = false,
            const std::string &code = sample_config)
        : manager(loop) {
        assert(Config::parse(config, code));
        auto &conf = config.proxies()[0];
        int listen_port = PickFreePort();
        if (listen_port > 0) {
            conf.port = listen_port;
        }
        for (auto &upstream : conf.upstreams) {
            int upstream_port = PickFreePort();
            if (upstream_port > 0) {
                upstream.port = upstream_port;
            }
        }
        if (disable_app) {
            conf.upstreams[0].app = nullptr;
        }
        proxy = nexer::TcpProxy::create(loop, conf, &manager, max_buffer_bytes);
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

    void StartServer(size_t upstream_index = 0) {
        auto &conf = config.proxies()[0];
        server = TcpServer::Create(loop);
        server->Listen(conf.upstreams[upstream_index].port);
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
    delete ((config::App *)(context.config.proxies()[0].upstreams[0].app))
        ->checker;
    ((config::App *)(context.config.proxies()[0].upstreams[0].app))->checker =
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

std::string failover_app_config = R"conf({
    proxies: [{
        listen: 19500,
        upstreams: [
            {
                host: '127.0.0.1',
                port: 19501,
                connect_timeout: 200,
                app: {
                    name: primary,
                    command: {
                        file: ./build/run_tests,
                        args: [ helper, sleep ],
                        env: [ NAME=a ]
                    }
                    max_start_time: 50
                }
            },
            { host: '127.0.0.1', port: 19502, connect_timeout: 200 },
        ]
    }]
})conf";

std::string failover_config = R"conf({
    proxies: [{
        listen: 19500,
        upstreams: [
            { host: '127.0.0.1', port: 19501, connect_timeout: 5000 },
            { host: '127.0.0.1', port: 19502, connect_timeout: 200 },
        ]
    }]
})conf";

std::string all_down_config = R"conf({
    proxies: [{
        listen: 19500,
        upstreams: [
            { host: '127.0.0.1', port: 19501, connect_timeout: 200 },
            { host: '127.0.0.1', port: 19502, connect_timeout: 200 },
        ]
    }]
})conf";

// Sends `request` through the proxy; returns what came back once `expect`
// bytes arrived, and whether the client was closed before `timeout_ms`.
static std::pair<std::string, bool>
Exchange(Context &context, uint64_t timeout_ms,
         const std::string &request = "hello", size_t expect = 5) {
    auto client = nexer::TcpClient::create(context.loop);
    client->connect(context.config.proxies()[0].port);

    std::string data;
    bool closed = false;
    client->on_connect(
        [client, request] { client->write(request.data(), request.size()); });
    auto stop_data = client->on_data([&, client](const char *s, size_t len) {
        data.append(s, len);
        if (data.size() >= expect) {
            context.loop.stop();
        }
    });
    auto stop_close = client->on_close([&] {
        closed = true;
        context.loop.stop();
    });

    auto timer = nexer::Timer::create(context.loop, timeout_ms);
    timer->on_tick([&] { context.loop.stop(); });
    timer->start(false);

    context.loop.run();

    timer->close();
    // The close completes after this frame is gone; drop callbacks that
    // capture its locals first.
    stop_data();
    stop_close();
    if (!client->is_closing()) {
        client->close();
    }
    return {data, closed};
}

// A ready standby is used without starting or waiting on the primary's app.
static void TestFailoverSkipsUnreadyApp() {
    Context context(2 * 1024 * 1024, false, failover_app_config);
    context.StartServer(1);

    auto [data, closed] = Exchange(context, 1000);
    assert(data == "hello");

    auto &primary = *context.config.proxies()[0].upstreams[0].app;
    assert(context.manager.snapshot(primary).launches == 0);
}

// A refused upstream is skipped after one attempt, not retried until its
// 5000ms connect_timeout.
static void TestFailoverSkipsRefusedUpstream() {
    Context context(2 * 1024 * 1024, false, failover_config);
    context.StartServer(1);

    auto [data, closed] = Exchange(context, 1000);
    assert(data == "hello");
}

static void TestFailoverAllDownCloses() {
    Context context(2 * 1024 * 1024, false, all_down_config);

    auto [data, closed] = Exchange(context, 2000);
    assert(closed);
    assert(data.empty());
}

// SOCKS5 server that accepts one CONNECT per connection, records the target
// and echoes everything after it.
struct FakeSocks {
    std::shared_ptr<TcpServer> server;
    std::list<std::shared_ptr<TcpClient>> clients;
    std::string host;
    int port = 0;
    bool refuse = false;
    // Accepts TCP but never answers, like ssh whose link died.
    bool silent = false;
    // Closes instead of replying to CONNECT, like ssh -D refusing a
    // destination.
    bool hang_up = false;
    // Runs when a CONNECT request arrives, before it is answered.
    std::function<void()> on_request;
    // Everything received after the CONNECT reply.
    std::string received;

    FakeSocks(EventLoop &loop, int listen_port) {
        server = TcpServer::Create(loop);
        assert(server->Listen("127.0.0.1", listen_port));
        server->OnConnection([this](std::shared_ptr<TcpClient> client) {
            clients.push_back(client);
            auto buffer = std::make_shared<std::string>();
            auto stage = std::make_shared<int>(0);
            std::weak_ptr<TcpClient> weak = client;
            client->on_data([this, weak, buffer, stage](const char *s,
                                                        size_t len) {
                auto client = weak.lock();
                if (!client)
                    return;
                buffer->append(s, len);
                if (silent)
                    return;
                if (*stage == 0 && buffer->size() >= 3) {
                    buffer->erase(0, 3);
                    client->write("\x05\x00", 2);
                    *stage = 1;
                }
                if (*stage == 1 && buffer->size() >= 5) {
                    size_t n = static_cast<unsigned char>((*buffer)[4]);
                    if (buffer->size() < 7 + n)
                        return;
                    host = buffer->substr(5, n);
                    port = (static_cast<unsigned char>((*buffer)[5 + n]) << 8) |
                           static_cast<unsigned char>((*buffer)[6 + n]);
                    buffer->erase(0, 7 + n);
                    if (on_request) {
                        on_request();
                    }
                    if (hang_up) {
                        client->close();
                        return;
                    }
                    std::string reply(
                        "\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
                    if (refuse) {
                        reply[1] = 0x05;
                    }
                    client->write(reply.data(), reply.size());
                    *stage = 2;
                }
                if (*stage == 2 && !buffer->empty()) {
                    received += *buffer;
                    client->write(buffer->data(), buffer->size());
                    buffer->clear();
                }
            });
        });
    }

    ~FakeSocks() {
        for (auto &client : clients) {
            if (!client->is_closing()) {
                client->close();
            }
        }
        if (!server->is_closing()) {
            server->close();
        }
    }
};

std::string http_socks_config = R"conf({
    proxies: [{
        listen: 19500,
        protocol: 'http+socks',
        upstreams: [
            { host: '127.0.0.1', port: 19501, connect_timeout: 200 },
            { host: '127.0.0.1', port: 19502, connect_timeout: 200 },
        ]
    }]
})conf";

static const std::string kEstablished =
    "HTTP/1.1 200 Connection established\r\n\r\n";

static int UpstreamPort(Context &context, size_t index) {
    return context.config.proxies()[0].upstreams[index].port;
}

// Bytes after the CONNECT head are held until the tunnel is up.
static void TestHttpConnect() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    auto [data, closed] = Exchange(context, 1000,
                                   "CONNECT example.com:443 HTTP/1.1\r\n"
                                   "Host: example.com:443\r\n\r\nping",
                                   kEstablished.size() + 4);
    assert(data == kEstablished + "ping");
    assert(socks.host == "example.com");
    assert(socks.port == 443);
}

static void TestHttpForward() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    std::string rewritten = "GET /a?b=1 HTTP/1.1\r\n"
                            "Host: example.com:8080\r\n"
                            "Connection: close\r\n\r\n";
    auto [data, closed] =
        Exchange(context, 1000,
                 "GET http://example.com:8080/a?b=1 HTTP/1.1\r\n"
                 "Host: example.com:8080\r\n"
                 "Proxy-Connection: keep-alive\r\n\r\n",
                 rewritten.size());
    assert(data == rewritten);
    assert(socks.host == "example.com");
    assert(socks.port == 8080);
}

static const std::string kForwardHead = "POST /a HTTP/1.1\r\n"
                                        "Host: a.test\r\n"
                                        "Content-Length: 3\r\n"
                                        "Connection: close\r\n\r\n";
static const std::string kForwardRequest = "POST http://a.test/a HTTP/1.1\r\n"
                                           "Host: a.test\r\n"
                                           "Content-Length: 3\r\n\r\nabc";
static const std::string kOtherRequest = "GET http://b.test/ HTTP/1.1\r\n"
                                         "Host: b.test\r\n"
                                         "Cookie: secret\r\n\r\n";

// A pipelined request for another origin never reaches the first origin.
static void TestHttpForwardDropsPipelined() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    auto expected = kForwardHead + "abc";
    auto [data, closed] = Exchange(
        context, 1000, kForwardRequest + kOtherRequest, expected.size());
    assert(data == expected);
    assert(socks.received == expected);
}

// Writes `first`, then `second` once `after` bytes came back (or 100ms
// later when `after` is 0). Runs until the client closes or 500ms pass.
static std::pair<std::string, bool> TwoWrites(Context &context,
                                              const std::string &first,
                                              const std::string &second,
                                              size_t after) {
    auto client = nexer::TcpClient::create(context.loop);
    client->connect(context.config.proxies()[0].port);
    std::string data;
    bool closed = false;
    bool sent = false;
    auto send_second = [&, client] {
        if (!sent) {
            sent = true;
            client->write(second.data(), second.size());
        }
    };
    std::shared_ptr<nexer::Timer> delay;
    auto stop_connect = client->on_connect([&, client] {
        client->write(first.data(), first.size());
        if (after == 0) {
            delay = nexer::Timer::create(context.loop, 100);
            delay->on_tick(send_second);
            delay->start(false);
        }
    });
    auto stop_data = client->on_data([&](const char *s, size_t len) {
        data.append(s, len);
        if (after > 0 && data.size() >= after) {
            send_second();
        }
    });
    auto stop_close = client->on_close([&] {
        closed = true;
        context.loop.stop();
    });
    auto timer = nexer::Timer::create(context.loop, 500);
    timer->on_tick([&] { context.loop.stop(); });
    timer->start(false);

    context.loop.run();
    timer->close();
    if (delay) {
        delay->close();
    }
    // The close completes after this frame is gone; drop callbacks that
    // capture its locals first.
    stop_connect();
    stop_data();
    stop_close();
    if (!client->is_closing()) {
        client->close();
    }
    assert(sent);
    return {data, closed};
}

// A request sent on the connection after the response is dropped, not sent
// to the first origin; the first exchange is left intact.
static void TestHttpForwardReuseDropped() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    auto expected = kForwardHead + "abc";
    auto [data, closed] =
        TwoWrites(context, kForwardRequest, kOtherRequest, expected.size());
    assert(!closed);
    assert(data == expected);
    assert(socks.received == expected);
}

// A later read holding the end of the body and a pipelined request forwards
// the body tail and drops the rest.
static void TestHttpForwardSplitUpload() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    auto expected = "POST /a HTTP/1.1\r\n"
                    "Host: a.test\r\n"
                    "Content-Length: 6\r\n"
                    "Connection: close\r\n\r\nabcdef";
    auto [data, closed] = TwoWrites(context,
                                    "POST http://a.test/a HTTP/1.1\r\n"
                                    "Host: a.test\r\n"
                                    "Content-Length: 6\r\n\r\nabc",
                                    "def" + kOtherRequest, 0);
    assert(!closed);
    assert(data == expected);
    assert(socks.received == expected);
}

static void TestHttpForwardExtensionMethod() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    std::string expected = "VERSION-CONTROL /doc HTTP/1.1\r\n"
                           "Host: a.test\r\n"
                           "Connection: close\r\n\r\n";
    auto [data, closed] =
        Exchange(context, 1000,
                 "VERSION-CONTROL http://a.test/doc HTTP/1.1\r\n"
                 "Host: a.test\r\n\r\n",
                 expected.size());
    assert(data == expected);
}

static void TestSocksPassthrough() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    auto [data, closed] =
        Exchange(context, 1000, std::string("\x05\x01\x00", 3), 2);
    assert(data == std::string("\x05\x00", 2));
}

static void TestHttpFailover() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 1));

    auto [data, closed] =
        Exchange(context, 1000, "CONNECT example.com:443 HTTP/1.1\r\n\r\nping",
                 kEstablished.size() + 4);
    assert(data == kEstablished + "ping");
}

static bool IsStatus(const std::string &data, const char *status) {
    auto prefix = std::string("HTTP/1.1 ") + status + " ";
    return data.compare(0, prefix.size(), prefix) == 0;
}

static void TestHttpSocksRefused() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));
    socks.refuse = true;

    auto [data, closed] =
        Exchange(context, 1000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
}

// A SOCKS-level refusal moves on to the standby instead of returning 502.
static void TestHttpSocksRefusedFailsOver() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks primary(context.loop, UpstreamPort(context, 0));
    FakeSocks standby(context.loop, UpstreamPort(context, 1));
    primary.refuse = true;

    auto [data, closed] =
        Exchange(context, 1000, "CONNECT example.com:443 HTTP/1.1\r\n\r\nping",
                 kEstablished.size() + 4);
    assert(data == kEstablished + "ping");
    assert(standby.host == "example.com");
}

// A primary that accepts TCP but stalls is abandoned after connect_timeout.
static void TestHttpSocksStalledFailsOver() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks primary(context.loop, UpstreamPort(context, 0));
    FakeSocks standby(context.loop, UpstreamPort(context, 1));
    primary.silent = true;

    auto [data, closed] =
        Exchange(context, 1000, "CONNECT example.com:443 HTTP/1.1\r\n\r\nping",
                 kEstablished.size() + 4);
    assert(data == kEstablished + "ping");
    assert(!primary.clients.empty());
}

static void TestHttpNoUpstream() {
    Context context(2 * 1024 * 1024, false, http_socks_config);

    auto [data, closed] =
        Exchange(context, 2000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
}

static void TestHttpMalformed() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));

    auto [data, closed] = Exchange(
        context, 1000, "GET /relative HTTP/1.1\r\n\r\n", std::string::npos);
    assert(closed);
    assert(IsStatus(data, "400"));
    assert(socks.clients.empty());
}

// An upstream that connected and then failed negotiation is not retried in
// the waiting pass.
static void TestHttpSocksRefusedEverywhere() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks primary(context.loop, UpstreamPort(context, 0));
    FakeSocks standby(context.loop, UpstreamPort(context, 1));
    primary.refuse = true;
    standby.refuse = true;

    auto [data, closed] =
        Exchange(context, 2000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
    assert(primary.clients.size() == 1);
    assert(standby.clients.size() == 1);
}

// A silent upstream with no app has nothing to require, so it is not tried
// again.
static void TestHttpSocksStalledNotRetried() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks primary(context.loop, UpstreamPort(context, 0));
    primary.silent = true;

    auto [data, closed] =
        Exchange(context, 2000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
    assert(primary.clients.size() == 1);
}

std::string http_socks_app_config = R"conf({
    proxies: [{
        listen: 19500,
        protocol: 'http+socks',
        upstreams: [{
            host: '127.0.0.1',
            port: 19501,
            connect_timeout: 100,
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
                max_start_time: 1000
                restart: { policy: on_failure, delay_ms: 10 }
            }
        }]
    }]
})conf";

// Starts the first upstream's app and returns once it is ready.
static const config::App &ReadyApp(Context &context) {
    auto &app = *context.config.proxies()[0].upstreams[0].app;
    context.manager.Require(app, [&](std::shared_ptr<Process>, int error) {
        assert(error == 0);
        context.loop.stop();
    });
    context.loop.run();
    assert(context.manager.snapshot(app).observed == ObservedState::Ready);
    return app;
}

// A ready app that stalls the SOCKS handshake is required again. Its checker
// passing means the destination stalled, so the upstream is not tried again.
static void TestHttpSocksStalledHealthyApp() {
    SetScenario("recheck-pass");
    Context context(2 * 1024 * 1024, false, http_socks_app_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));
    socks.silent = true;
    auto &app = ReadyApp(context);

    auto [data, closed] =
        Exchange(context, 3000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
    assert(socks.clients.size() == 1);
    assert(context.manager.snapshot(app).launches == 1);
}

// Its checker failing relaunches the app, and the upstream is tried again.
static void TestHttpSocksStalledHungApp() {
    SetScenario("recheck-fail");
    Context context(2 * 1024 * 1024, false, http_socks_app_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));
    socks.silent = true;
    auto &app = ReadyApp(context);

    auto [data, closed] =
        Exchange(context, 3000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
    assert(socks.clients.size() == 2);
    assert(context.manager.snapshot(app).launches == 2);
}

// An upstream that closes the handshake is not required or tried again:
// closing is how ssh -D refuses a destination.
static void TestHttpSocksClosedNotRetried() {
    SetScenario("recheck-pass");
    Context context(2 * 1024 * 1024, false, http_socks_app_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));
    socks.hang_up = true;
    ReadyApp(context);

    auto [data, closed] =
        Exchange(context, 2000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 std::string::npos);
    assert(closed);
    assert(IsStatus(data, "502"));
    assert(socks.host == "example.com");
    assert(socks.clients.size() == 1);
}

// A handshake cut off by its app going down is not a refusal: the client
// waits for the relaunched app and is served through it.
static void TestHttpSocksClosedByRestart() {
    SetScenario("recheck-fail");
    Context context(2 * 1024 * 1024, false, http_socks_app_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));
    auto &app = ReadyApp(context);
    socks.on_request = [&] {
        socks.hang_up = socks.clients.size() == 1;
        if (socks.hang_up) {
            context.manager.Restart(app, [](std::shared_ptr<Process>, int) {});
        }
    };

    auto [data, closed] =
        Exchange(context, 3000, "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                 kEstablished.size());
    assert(data == kEstablished);
    assert(socks.clients.size() == 2);
    assert(context.manager.snapshot(app).launches == 2);
}

static void TestHttpHeadTimeout() {
    Context context(2 * 1024 * 1024, false, http_socks_config);
    FakeSocks socks(context.loop, UpstreamPort(context, 0));
    context.proxy->set_head_timeout(200);

    auto started = nexer::Timer::now();
    auto [data, closed] = Exchange(context, 2000, "GET http://a/ HTTP/1.1\r\n",
                                   std::string::npos);
    assert(closed);
    assert(data.empty());
    assert(nexer::Timer::now() - started < 1000);
    assert(context.proxy->connections() == 0);
    assert(socks.clients.empty());
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
    TestFailoverSkipsUnreadyApp();
    TestFailoverSkipsRefusedUpstream();
    TestFailoverAllDownCloses();
    TestHttpConnect();
    TestHttpForward();
    TestSocksPassthrough();
    TestHttpFailover();
    TestHttpSocksRefused();
    TestHttpNoUpstream();
    TestHttpMalformed();
    TestHttpSocksRefusedFailsOver();
    TestHttpSocksStalledFailsOver();
    TestHttpSocksStalledNotRetried();
    TestHttpSocksStalledHealthyApp();
    TestHttpSocksStalledHungApp();
    TestHttpSocksClosedNotRetried();
    TestHttpSocksClosedByRestart();
    TestHttpForwardDropsPipelined();
    TestHttpForwardReuseDropped();
    TestHttpForwardSplitUpload();
    TestHttpForwardExtensionMethod();
    TestHttpSocksRefusedEverywhere();
    TestHttpHeadTimeout();
}

} // namespace test
} // namespace nexer
