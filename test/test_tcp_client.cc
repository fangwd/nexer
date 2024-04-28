/* Copyright (c) Weidong Fang. All rights reserved. */

#include <assert.h>

#include <thread>

#include "string_buffer.h"
#include "tcp_client.h"
#include "tcp_server.h"
#include "timer.h"

#define TEST_PORT 19007

namespace nexer {
namespace test {

void StartEchoServer(int port);

static int shutdown_echo_server() {
    EventLoop loop;
    TcpClient &client = TcpClient::Create(loop);

    int error = 0;

    client.OnError([&](int err, const char *errmsg) {
        error |= err;
    });

    client.OnConnect([&]() {
        client.Write("Q", sizeof "Q" - 1);
    });

    client.OnData([&](const char *s, size_t len) {
        assert(len == 1);
        assert(s[0] == 'Q');
        client.Close();
    });

    client.Connect(TEST_PORT);

    loop.Run();

    assert(error == 0);

    return 0;
}

static int test_read_write() {
    EventLoop loop;
    TcpClient &client = TcpClient::Create(loop);

    StringBuffer sb;
    int connected = 0;
    int sent = 0;
    int closed = 0;

    client.OnError([&](int err, const char *errmsg) {
        assert(0);
    });

    client.OnConnect([&]() {
        connected++;
        client.Write("hello", sizeof "hello" - 1);
    });

    client.OnData([&](const char *s, size_t len) {
        sb.Write(s, len);
        client.Close();
    });

    client.OnSend([&]() {
        sent++;
    });

    client.OnClose([&]() {
        closed++;
    });

    client.Connect(TEST_PORT);

    loop.Run();

    assert(connected == 1);
    assert(sent == 1);
    assert(closed == 1);
    assert(strcmp(sb.data(), "hello") == 0);

    return 0;
}

static int test_server_close() {
    EventLoop loop;
    TcpClient &client = TcpClient::Create(loop);

    int closed = 0;

    client.OnError([&](int err, const char *errmsg) {
        assert(0);
    });

    client.OnConnect([&]() {
        client.Write("QSS", sizeof "QSS" - 1);
    });

    client.OnData([&](const char *s, size_t len) {
        assert(len == 3);
        assert(memcmp(s, "QSS", 3) == 0);
    });

    client.OnClose([&]() {
        closed++;
    });

    client.Connect(TEST_PORT);

    loop.Run();

    assert(closed == 1);

    return 0;
}

static int test_server_close_reset() {
    EventLoop loop;
    TcpClient &client = TcpClient::Create(loop);

    int closed = 0;

    client.OnError([&](int err, const char *errmsg) {
        assert(err == UV_ECONNRESET);
        client.Close();
    });

    client.OnConnect([&]() {
        client.Write("QSH", sizeof "QSH" - 1);
    });

    client.OnData([&](const char *s, size_t len) {
        assert(len == 3);
        assert(memcmp(s, "QSH", 3) == 0);
    });

    client.OnClose([&]() {
        closed++;
    });

    client.Connect(TEST_PORT);

    loop.Run();

    assert(closed == 1);

    return 0;
}

void run_test_echo_server(void (*start_server)(int)) {
    std::thread t1(start_server, TEST_PORT);

    test_read_write();
    test_server_close();
    test_server_close_reset();

    shutdown_echo_server();

    t1.join();
}

static void connect_to_unknown_server() {
    EventLoop loop;

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpClient::Connect(loop, "some-unknown-host", TEST_PORT, 3000, [&](TcpClient* client) {
        connected = client != nullptr;
        callback_called++;
    }, [&](TcpClient& tcp) {
        try_count++;
    });

    loop.Run();

    assert(try_count > 1);
    assert(!connected);
    assert(callback_called == 1);
}

static void start_server_with_delay(uint64_t delay) {
    EventLoop loop;
    Timer &timer = Timer::Create(loop, delay);
    timer.OnTick([&] {
        timer.Close();
        TcpServer &server = TcpServer::Create(loop);
        server.OnConnection([&](TcpClient& client) {
            client.Close();
            server.Close();
        });
        server.Listen(TEST_PORT);
    });
    timer.Start();
    loop.Run();
}

static void connect_to_non_listening_server() {
    EventLoop loop;

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpClient::Connect(loop, "localhost", TEST_PORT, 3000, [&](TcpClient* client) {
        connected = client != nullptr;
        callback_called++;
    }, [&](TcpClient& tcp) {
        try_count++;
    });

    loop.Run();

    assert(try_count > 1);
    assert(!connected);
    assert(callback_called == 1);
}

static void connect_to_listening_server() {
    EventLoop loop;

    std::thread t1(start_server_with_delay, 0);

    uv_sleep(1000);

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpClient::Connect(loop, "localhost", TEST_PORT, 4000, [&](TcpClient* client) {
        connected = client != nullptr;
        callback_called++;
    }, [&](TcpClient& tcp) {
        try_count++;
    });

    loop.Run();

    t1.join();

    assert(try_count == 1);
    assert(connected);
    assert(callback_called == 1);
}

static void connect_to_server_with_slow_start() {
    EventLoop loop;

    std::thread t1(start_server_with_delay, 3000);

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpClient::Connect(loop, "localhost", TEST_PORT, 4000, [&](TcpClient* client) {
        connected = client != nullptr;
        callback_called++;
    }, [&](TcpClient& tcp) {
        try_count++;
    });

    loop.Run();

    t1.join();

    assert(try_count > 1);
    assert(connected);
    assert(callback_called == 1);
}

static void test_connect_with_retry() {
    connect_to_unknown_server();
    connect_to_non_listening_server();
    connect_to_listening_server();
    connect_to_server_with_slow_start();
}

void TestTcpClient() {
    run_test_echo_server(StartEchoServer);
    test_connect_with_retry();
}

}  // namespace test
}  // namespace nexer
