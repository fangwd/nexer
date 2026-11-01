#include <assert.h>

#include <arpa/inet.h>
#include <cstring>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "nexer/core/string_buffer.h"
#include "nexer/core/tcp_client.h"
#include "nexer/core/tcp_connector.h"
#include "nexer/core/tcp_server.h"
#include "nexer/core/timer.h"

namespace nexer {
namespace test {

namespace {

int AcquirePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        close(fd);
        return 0;
    }

    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

int TestPort() {
    static int port = 0;
    if (port == 0) {
        port = AcquirePort();
        if (port == 0) {
            port = 19007;
        }
    }
    return port;
}

} // namespace

void StartEchoServer(int port);

static int shutdown_echo_server() {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    int error = 0;

    client->on_error([&](int err, const char *errmsg) { error |= err; });

    std::weak_ptr<TcpClient> weak_client = client;
    client->on_connect([weak_client]() {
        if (auto client = weak_client.lock()) {
            client->write("Q", sizeof "Q" - 1);
        }
    });

    client->on_data([weak_client](const char *s, size_t len) {
        if (auto client = weak_client.lock()) {
            assert(len == 1);
            assert(s[0] == 'Q');
            client->close();
        }
    });

    client->connect(TestPort());

    loop.run();

    assert(error == 0);

    return 0;
}

static int test_read_write() {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    StringBuffer sb;
    int connected = 0;
    int sent = 0;
    int closed = 0;

    client->on_error([&](int err, const char *errmsg) { assert(0); });

    std::weak_ptr<TcpClient> weak_client = client;
    client->on_connect([&connected, weak_client]() {
        if (auto client = weak_client.lock()) {
            connected++;
            client->write("hello", sizeof "hello" - 1);
        }
    });

    client->on_data([&sb, weak_client](const char *s, size_t len) {
        if (auto client = weak_client.lock()) {
            sb.Write(s, len);
            client->close();
        }
    });

    client->on_send([&sent]() { sent++; });

    client->on_close([&closed]() { closed++; });

    client->connect(TestPort());

    loop.run();

    assert(connected == 1);
    assert(sent == 1);
    assert(closed == 1);
    assert(strcmp(sb.data(), "hello") == 0);

    return 0;
}

static int test_stack_buffer_write() {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    StringBuffer sb;
    int connected = 0;
    int sent = 0;
    int closed = 0;

    client->on_error([&](int err, const char *errmsg) { assert(0); });

    std::weak_ptr<TcpClient> weak_client = client;
    client->on_connect([&connected, weak_client]() {
        if (auto client = weak_client.lock()) {
            connected++;
            char stack_buffer[] = {'p', 'i', 'n', 'g'};
            client->write(stack_buffer, sizeof(stack_buffer));
            memset(stack_buffer, 0, sizeof(stack_buffer));
        }
    });

    client->on_data([&sb, weak_client](const char *s, size_t len) {
        if (auto client = weak_client.lock()) {
            sb.Write(s, len);
            client->close();
        }
    });

    client->on_send([&sent]() { sent++; });

    client->on_close([&closed]() { closed++; });

    client->connect(TestPort());

    loop.run();

    assert(connected == 1);
    assert(sent == 1);
    assert(closed == 1);
    assert(strcmp(sb.data(), "ping") == 0);

    return 0;
}

static int test_server_close() {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    int closed = 0;

    client->on_error([&](int err, const char *errmsg) { assert(0); });

    std::weak_ptr<TcpClient> weak_client = client;
    client->on_connect([weak_client]() {
        if (auto client = weak_client.lock()) {
            client->write("QSS", sizeof "QSS" - 1);
        }
    });

    client->on_data([&](const char *s, size_t len) {
        assert(len == 3);
        assert(memcmp(s, "QSS", 3) == 0);
    });

    client->on_close([&]() { closed++; });

    client->connect(TestPort());

    loop.run();

    assert(closed == 1);

    return 0;
}

static int test_server_close_reset() {
    EventLoop loop;
    auto client = TcpClient::create(loop);
    assert(client);

    int closed = 0;

    std::weak_ptr<TcpClient> weak_client = client;
    client->on_error([weak_client](int err, const char *errmsg) {
        if (auto client = weak_client.lock()) {
            assert(err == UV_ECONNRESET);
            client->close();
        }
    });

    client->on_connect([weak_client]() {
        if (auto client = weak_client.lock()) {
            client->write("QSH", sizeof "QSH" - 1);
        }
    });

    client->on_data([&](const char *s, size_t len) {
        assert(len == 3);
        assert(memcmp(s, "QSH", 3) == 0);
    });

    client->on_close([&]() { closed++; });

    client->connect(TestPort());

    loop.run();

    assert(closed == 1);

    return 0;
}

void run_test_echo_server(void (*start_server)(int)) {
    std::thread t1(start_server, TestPort());

    test_read_write();
    test_stack_buffer_write();
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

    TcpConnectOptions opts{
        .host = "some-unknown-host",
        .port = TestPort(),
        .deadline_ms = 250,
        .retry_interval_ms = 40,
    };
    auto connector = TcpConnector::create(
        loop, std::move(opts),
        [&](std::shared_ptr<TcpClient> client) {
            connected = client != nullptr;
            callback_called++;
        },
        [&](TcpClient &) { try_count++; });
    (void)connector;

    loop.run();

    assert(try_count > 1);
    assert(!connected);
    assert(callback_called == 1);
}

static void start_server_with_delay(uint64_t delay, std::promise<void> ready) {
    EventLoop loop;
    std::shared_ptr<TcpServer> server;

    auto start_server = [&] {
        server = TcpServer::Create(loop);
        std::weak_ptr<TcpServer> weak_server = server;
        server->OnConnection([weak_server](std::shared_ptr<TcpClient> client) {
            client->close();
            if (auto server = weak_server.lock()) {
                server->close();
            }
        });
        if (!server->Listen(TestPort())) {
            ready.set_value();
            return;
        }
        ready.set_value();
    };

    std::shared_ptr<Timer> timer;

    if (delay == 0) {
        start_server();
    } else {
        timer = Timer::create(loop, delay);
        std::weak_ptr<Timer> weak_timer = timer;
        timer->on_tick([weak_timer, &start_server] {
            if (auto timer = weak_timer.lock()) {
                timer->close();
            }
            start_server();
        });
        timer->start(false);
    }

    loop.run();
    server.reset();
}

static void connect_to_non_listening_server() {
    EventLoop loop;

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "localhost",
        .port = TestPort(),
        .deadline_ms = 250,
        .retry_interval_ms = 40,
    };
    auto connector = TcpConnector::create(
        loop, std::move(opts),
        [&](std::shared_ptr<TcpClient> client) {
            connected = client != nullptr;
            callback_called++;
        },
        [&](TcpClient &) { try_count++; });
    (void)connector;

    loop.run();

    assert(try_count > 1);
    assert(!connected);
    assert(callback_called == 1);
}

static void connect_to_listening_server() {
    EventLoop loop;

    std::promise<void> ready;
    auto ready_future = ready.get_future();
    std::thread t1(start_server_with_delay, 0, std::move(ready));
    ready_future.wait();

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "localhost",
        .port = TestPort(),
        .deadline_ms = 250,
        .retry_interval_ms = 40,
    };
    auto connector = TcpConnector::create(
        loop, std::move(opts),
        [&](std::shared_ptr<TcpClient> client) {
            connected = client != nullptr;
            callback_called++;
        },
        [&](TcpClient &) { try_count++; });
    (void)connector;

    loop.run();
    ready_future.wait();
    t1.join();

    assert(try_count == 1);
    assert(connected);
    assert(callback_called == 1);
}

static void connect_to_server_with_slow_start() {
    EventLoop loop;

    std::promise<void> ready;
    auto ready_future = ready.get_future();
    std::thread t1(start_server_with_delay, 180, std::move(ready));

    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "localhost",
        .port = TestPort(),
        .deadline_ms = 400,
        .retry_interval_ms = 50,
    };
    auto connector = TcpConnector::create(
        loop, std::move(opts),
        [&](std::shared_ptr<TcpClient> client) {
            connected = client != nullptr;
            callback_called++;
        },
        [&](TcpClient &) { try_count++; });
    (void)connector;

    loop.run();
    ready_future.wait();
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

} // namespace test
} // namespace nexer
