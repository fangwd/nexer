#include <assert.h>
#include <string.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "helper.h"
#include "nexer/core/logger.h"
#include "nexer/core/string_buffer.h"
#include "nexer/core/tcp_client.h"
#include "nexer/core/timer.h"
#include "nexer/http/http_client.h"
#include "nexer/http/http_server.h"

namespace nexer {
namespace test {

struct ServerHarness {
    int port = 0;
    bool ready = false;
    bool listen_ok = false;
    std::mutex mu;
    std::condition_variable cv;
};

struct RawResult {
    std::string data;
    bool closed_by_peer = false;
};

static RawResult
FetchRawResponseWithCloseFlag(int port, const char *request, int timeout_ms,
                              const std::string &stop_token = {});
static void SendShutdown(int port);

static void test_keep_alive(ServerHarness *harness) {
    EventLoop loop;
    auto server = http::Server::create(loop);

    std::weak_ptr<http::Server> weak_server = server;
    server->on_request(
        [weak_server](std::shared_ptr<http::incoming::Request> req,
                      http::outgoing::Response &res) {
            res.body() << "Hello";
            res.end();

            if (req->url().path == "/shutdown") {
                if (auto server = weak_server.lock()) {
                    server->close();
                }
            }
        });

    int port = PickFreePort();
    bool listen_ok = port > 0 && server->Listen(port);
    {
        std::lock_guard<std::mutex> lock(harness->mu);
        harness->port = port;
        harness->listen_ok = listen_ok;
        harness->ready = true;
    }
    harness->cv.notify_one();
    if (!listen_ok) {
        return;
    }
    loop.run();
}

const char *url_of(int port, const char *path) {
    static char url[1024];
    snprintf(url, sizeof url, "http://localhost:%d%s", port, path);
    return url;
}

static void make_keep_alive_requests(int port) {
    HttpClient client;
    auto res = client.get(url_of(port, "/hello"));
    assert(res->body() == "Hello");

    SendShutdown(port);
}

static void test_async_server(ServerHarness *harness) {
    EventLoop loop;
    auto server = http::Server::create(loop);

    std::weak_ptr<http::Server> weak_server = server;
    server->on_request(
        [weak_server, &loop](std::shared_ptr<http::incoming::Request> req,
                             http::outgoing::Response &res) {
            if (req->url().path == "/async_ok") {
                auto timer = Timer::create(loop, 100);
                timer->on_tick([req, timer]() {
                    req->response().body() << "Async OK";
                    req->response().end();
                    timer->close();
                });
                timer->start();
            } else if (req->url().path == "/async_early_close") {
                auto timer = Timer::create(loop, 200);
                timer->on_tick([req, timer]() {
                    req->response().body() << "Should not crash";
                    req->response().end();
                    timer->close();
                });
                timer->start();
            } else if (req->url().path == "/shutdown") {
                res.end();
                if (auto server = weak_server.lock()) {
                    server->close();
                }
            } else {
                res.end(404);
            }
        });

    int port = PickFreePort();
    bool listen_ok = port > 0 && server->Listen(port);
    {
        std::lock_guard<std::mutex> lock(harness->mu);
        harness->port = port;
        harness->listen_ok = listen_ok;
        harness->ready = true;
    }
    harness->cv.notify_one();
    if (!listen_ok) {
        return;
    }
    loop.run();
}

static void make_async_requests(int port) {
    // 1. Async OK
    {
        HttpClient client;
        auto res = client.get(url_of(port, "/async_ok"));
        assert(res->body() == "Async OK");
    }

    // 2. Async Early Close
    {
        EventLoop loop;
        auto client = TcpClient::create(loop);
        assert(client);
        bool connected = false;

        client->connect("127.0.0.1", port);
        auto weak_client = std::weak_ptr<TcpClient>(client);
        client->on_connect([weak_client, &loop, &connected]() {
            auto client = weak_client.lock();
            if (!client)
                return;
            connected = true;
            const char *req =
                "GET /async_early_close HTTP/1.1\r\nHost: localhost\r\n\r\n";
            client->write(req, strlen(req));

            // Close after 50ms
            auto t = Timer::create(loop, 50);
            t->on_tick([weak_client, t]() {
                auto client = weak_client.lock();
                if (!client)
                    return;
                client->close();
                t->close();
            });
            t->start();
        });

        // Ensure we run long enough for server to try to reply (server waits
        // 200ms)
        auto stopper = Timer::create(loop, 400);
        stopper->on_tick([stopper]() { stopper->close(); });
        stopper->start();

        loop.run();
        assert(connected);
    }

    // Shutdown
    {
        const char *req = "GET /shutdown HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        FetchRawResponseWithCloseFlag(port, req, 1000);
    }
}

static void test_streaming_server(ServerHarness *harness) {
    EventLoop loop;
    auto server = http::Server::create(loop);

    std::weak_ptr<http::Server> weak_server = server;
    server->on_request(
        [weak_server](std::shared_ptr<http::incoming::Request> req,
                      http::outgoing::Response &res) {
            if (req->url().path == "/stream") {
                res.set_header("Content-Type", "text/plain");
                res.write_body("chunk-", 6);
                res.write_body("one-", 4);
                res.write_body("two", 3);
                res.end(200);
            } else if (req->url().path == "/stream_count") {
                res.set_header("Content-Type", "text/plain");
                res.set_header("X-Test", "one-write");
                res.write_body("a", 1);
                res.write_body("b", 1);
                res.end(200);
            } else if (req->url().path == "/chunked") {
                res.set_header("Content-Type", "text/plain");
                res.begin_chunked_encoding();
                res.write_chunk("hello");
                res.write_chunk("world");
                res.end_chunk_encoding();
            } else if (req->url().path == "/chunked_async") {
                res.set_header("Content-Type", "text/plain");
                res.begin_chunked_encoding();
                res.write_chunk("first");
                std::weak_ptr<http::incoming::Request> weak_req = req;
                auto timer = Timer::create(req->server().loop(), 80);
                timer->on_tick([weak_req, timer]() {
                    if (auto req_locked = weak_req.lock()) {
                        req_locked->response().write_chunk("second");
                        req_locked->response().end_chunk_encoding();
                    }
                    timer->close();
                });
                timer->start(false);
            } else if (req->url().path == "/chunked_trailer") {
                res.set_header("Content-Type", "text/plain");
                res.begin_chunked_encoding();
                res.write_chunk("trail");
                res.write_trailer("X-Trailer: yes");
                res.end_chunk_encoding();
            } else if (req->url().path == "/chunked_with_length") {
                res.set_header("Content-Length", "999");
                res.begin_chunked_encoding();
                res.write_chunk("len");
                res.end_chunk_encoding();
            } else if (req->url().path == "/chunked_close_mid") {
                res.set_header("Content-Type", "text/plain");
                res.begin_chunked_encoding();
                res.write_chunk("first");
                std::weak_ptr<http::incoming::Request> weak_req = req;
                auto timer = Timer::create(req->server().loop(), 120);
                timer->on_tick([weak_req, timer]() {
                    if (auto req_locked = weak_req.lock()) {
                        req_locked->response().write_chunk("second");
                        req_locked->response().end_chunk_encoding();
                    }
                    timer->close();
                });
                timer->start(false);
            } else if (req->url().path == "/large") {
                res.set_header("Content-Type", "text/plain");
                for (int i = 0; i < 512; ++i) {
                    res.write_body("0123456789", 10);
                }
                res.end(200);
            } else if (req->url().path == "/shutdown") {
                res.end(200);
                if (auto server = weak_server.lock()) {
                    server->close();
                }
            } else {
                res.end(404);
            }
        });

    int port = PickFreePort();
    bool listen_ok = port > 0 && server->Listen(port);
    {
        std::lock_guard<std::mutex> lock(harness->mu);
        harness->port = port;
        harness->listen_ok = listen_ok;
        harness->ready = true;
    }
    harness->cv.notify_one();
    if (!listen_ok) {
        return;
    }
    loop.run();
}

static std::string FetchRawResponse(int port, const char *request,
                                    const std::string &stop_token) {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    std::string out;
    bool closed = false;

    client->on_data([&out, stop_token, client](const char *s, size_t len) {
        out.append(s, len);
        if (!stop_token.empty() && out.find(stop_token) != std::string::npos) {
            client->close();
        }
    });

    client->on_close([&closed]() { closed = true; });

    client->connect("127.0.0.1", port);
    auto weak_client = std::weak_ptr<TcpClient>(client);
    client->on_connect([weak_client, request]() {
        auto client = weak_client.lock();
        if (!client)
            return;
        client->write(request, strlen(request));
    });

    auto stopper = Timer::create(loop, 400);
    stopper->on_tick([weak_client, stopper]() {
        if (auto client = weak_client.lock()) {
            if (!client->is_closing()) {
                client->close();
            }
        }
        stopper->close();
    });
    stopper->start(false);

    loop.run();
    assert(closed);
    return out;
}

static RawResult FetchRawResponseWithCloseFlag(int port, const char *request,
                                               int timeout_ms,
                                               const std::string &stop_token) {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    RawResult result;
    bool forced_close = false;

    client->on_data([&result, stop_token, client](const char *s, size_t len) {
        result.data.append(s, len);
        if (!stop_token.empty() &&
            result.data.find(stop_token) != std::string::npos) {
            client->close();
        }
    });

    client->on_close([&result, &forced_close]() {
        if (!forced_close) {
            result.closed_by_peer = true;
        }
    });

    client->connect("127.0.0.1", port);
    auto weak_client = std::weak_ptr<TcpClient>(client);
    client->on_connect([weak_client, request]() {
        auto client = weak_client.lock();
        if (!client)
            return;
        client->write(request, strlen(request));
    });

    auto stopper = Timer::create(loop, static_cast<uint64_t>(timeout_ms));
    stopper->on_tick([weak_client, stopper, &forced_close]() {
        if (auto client = weak_client.lock()) {
            if (!client->is_closing()) {
                forced_close = true;
                client->close();
            }
        }
        stopper->close();
    });
    stopper->start(false);

    loop.run();
    return result;
}

static void SendShutdown(int port) {
    const char *req = "GET /shutdown HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";
    FetchRawResponseWithCloseFlag(port, req, 1000);
}

static void test_limits_server(ServerHarness *harness) {
    EventLoop loop;
    auto server = http::Server::create(loop);
    server->set_limits(64, 32, 32);

    std::weak_ptr<http::Server> weak_server = server;
    server->on_request(
        [weak_server](std::shared_ptr<http::incoming::Request> req,
                      http::outgoing::Response &res) {
            if (req->url().path == "/too_big") {
                res.set_header("Content-Type", "text/plain");
                res.body() << "this response is intentionally too large";
                res.end(200);
            } else if (req->url().path == "/chunked_too_big") {
                res.set_header("Content-Type", "text/plain");
                res.begin_chunked_encoding();
                res.write_chunk("this chunk exceeds the limit");
                res.end_chunk_encoding();
            } else if (req->url().path == "/shutdown") {
                res.end(200);
                if (auto server = weak_server.lock()) {
                    server->close();
                }
            } else {
                res.end(200);
            }
        });

    int port = PickFreePort();
    bool listen_ok = port > 0 && server->Listen(port);
    {
        std::lock_guard<std::mutex> lock(harness->mu);
        harness->port = port;
        harness->listen_ok = listen_ok;
        harness->ready = true;
    }
    harness->cv.notify_one();
    if (!listen_ok) {
        return;
    }
    loop.run();
}

static void make_streaming_requests(int port) {
    {
        HttpClient client;
        auto res = client.get(url_of(port, "/stream"));
        assert(res->body() == "chunk-one-two");
    }

    {
        HttpClient client;
        auto res = client.get(url_of(port, "/chunked"));
        assert(res->body() == "helloworld");
    }

    {
        const char *req = "GET /stream_count HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        auto res = FetchRawResponseWithCloseFlag(port, req, 1000);
        size_t header_end = res.data.find("\r\n\r\n");
        assert(header_end != std::string::npos);
        std::string headers = res.data.substr(0, header_end);
        assert(headers.find("X-Test: one-write") != std::string::npos);
        assert(headers.find("Content-Length: 2") != std::string::npos);
    }

    {
        HttpClient client;
        auto res = client.get(url_of(port, "/chunked_async"));
        assert(res->body() == "firstsecond");
    }

    {
        EventLoop loop;
        auto client = TcpClient::create(loop);
        assert(client);
        bool closed = false;

        client->connect("127.0.0.1", port);
        auto weak_client = std::weak_ptr<TcpClient>(client);
        client->on_connect([weak_client]() {
            auto client = weak_client.lock();
            if (!client)
                return;
            const char *req = "GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n";
            client->write(req, strlen(req));
        });
        client->on_close([&closed]() { closed = true; });

        auto closer = Timer::create(loop, 20);
        closer->on_tick([weak_client, closer]() {
            if (auto client = weak_client.lock()) {
                client->close();
            }
            closer->close();
        });
        closer->start(false);

        auto stopper = Timer::create(loop, 400);
        stopper->on_tick([stopper]() { stopper->close(); });
        stopper->start(false);

        loop.run();
        assert(closed);
    }

    {
        const char *req = "GET /chunked_trailer HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        std::string response =
            FetchRawResponse(port, req, "\r\n0\r\nX-Trailer: yes\r\n\r\n");
        assert(response.find("Transfer-Encoding: chunked") !=
               std::string::npos);
        assert(response.find("\r\n0\r\nX-Trailer: yes\r\n\r\n") !=
               std::string::npos);
    }

    {
        const char *req = "GET /chunked_with_length HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        std::string response = FetchRawResponse(port, req, "\r\n0\r\n\r\n");
        auto header_end = response.find("\r\n\r\n");
        std::string headers = header_end == std::string::npos
                                  ? response
                                  : response.substr(0, header_end);
        assert(headers.find("Content-Length") == std::string::npos);
        assert(headers.find("Transfer-Encoding: chunked") != std::string::npos);
    }

    {
        EventLoop loop;
        auto client = TcpClient::create(loop);
        assert(client);
        bool closed = false;

        client->connect("127.0.0.1", port);
        auto weak_client = std::weak_ptr<TcpClient>(client);
        client->on_connect([weak_client]() {
            auto client = weak_client.lock();
            if (!client)
                return;
            const char *req =
                "GET /chunked_close_mid HTTP/1.1\r\nHost: localhost\r\n\r\n";
            client->write(req, strlen(req));
        });
        client->on_data([weak_client](const char *, size_t) {
            if (auto client = weak_client.lock()) {
                client->close();
            }
        });
        client->on_close([&closed]() { closed = true; });

        auto stopper = Timer::create(loop, 300);
        stopper->on_tick([stopper]() { stopper->close(); });
        stopper->start(false);

        loop.run();
        assert(closed);
    }

    {
        EventLoop loop;
        auto client = TcpClient::create(loop);
        assert(client);
        bool closed = false;

        client->connect("127.0.0.1", port);
        auto weak_client = std::weak_ptr<TcpClient>(client);
        client->on_connect([weak_client]() {
            auto client = weak_client.lock();
            if (!client)
                return;
            const char *req = "BADREQUEST\r\n\r\n";
            client->write(req, strlen(req));
        });
        client->on_close([&closed]() { closed = true; });

        auto stopper = Timer::create(loop, 400);
        stopper->on_tick([stopper]() { stopper->close(); });
        stopper->start(false);

        loop.run();
        assert(closed);
    }

    {
        EventLoop loop;
        auto client = TcpClient::create(loop);
        assert(client);

        client->connect("127.0.0.1", port);
        auto weak_client = std::weak_ptr<TcpClient>(client);
        client->on_connect([weak_client]() {
            auto client = weak_client.lock();
            if (!client)
                return;
            const char *req = "GET /stream HTTP/1.1\r\nHost: localhost\r\n";
            client->write(req, strlen(req));
            client->close();
        });

        auto stopper = Timer::create(loop, 200);
        stopper->on_tick([stopper]() { stopper->close(); });
        stopper->start(false);

        loop.run();
    }

    SendShutdown(port);
}

static void make_limit_requests(int port) {
    {
        const char *req =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Too-Large: "
            "012345678901234567890123456789012345678901234567890123456789\r\n"
            "\r\n";
        auto res = FetchRawResponseWithCloseFlag(port, req, 2000);
        assert(res.closed_by_peer);
        assert(res.data.find("HTTP/1.1") == std::string::npos);
    }

    {
        const char *req =
            "POST / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 64\r\n"
            "\r\n"
            "0123456789012345678901234567890123456789012345678901234567890123";
        auto res = FetchRawResponseWithCloseFlag(port, req, 2000);
        assert(res.closed_by_peer);
    }

    {
        const char *req = "GET /too_big HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        auto res = FetchRawResponseWithCloseFlag(port, req, 2000);
        assert(res.closed_by_peer);
        assert(res.data.find("HTTP/1.1") == std::string::npos);
    }

    {
        const char *req = "GET /chunked_too_big HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        auto res = FetchRawResponseWithCloseFlag(port, req, 2000, "0\r\n\r\n");
        assert(res.closed_by_peer ||
               res.data.find("0\r\n\r\n") != std::string::npos);
    }

    SendShutdown(port);
}

void TestHttpServer() {
    {
        ServerHarness harness;
        std::thread t1(test_keep_alive, &harness);
        {
            std::unique_lock<std::mutex> lock(harness.mu);
            harness.cv.wait(lock, [&harness]() { return harness.ready; });
        }
        assert(harness.listen_ok);
        make_keep_alive_requests(harness.port);
        t1.join();
    }

    {
        ServerHarness harness;
        std::thread t2(test_async_server, &harness);
        {
            std::unique_lock<std::mutex> lock(harness.mu);
            harness.cv.wait(lock, [&harness]() { return harness.ready; });
        }
        assert(harness.listen_ok);
        make_async_requests(harness.port);
        t2.join();
    }

    {
        ServerHarness harness;
        std::thread t3(test_streaming_server, &harness);
        {
            std::unique_lock<std::mutex> lock(harness.mu);
            harness.cv.wait(lock, [&harness]() { return harness.ready; });
        }
        assert(harness.listen_ok);
        make_streaming_requests(harness.port);
        t3.join();
    }

    {
        ServerHarness harness;
        std::thread t4(test_limits_server, &harness);
        {
            std::unique_lock<std::mutex> lock(harness.mu);
            harness.cv.wait(lock, [&harness]() { return harness.ready; });
        }
        assert(harness.listen_ok);
        make_limit_requests(harness.port);
        t4.join();
    }
}

} // namespace test
} // namespace nexer
