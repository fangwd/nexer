#include <assert.h>

#include <chrono>
#include <future>
#include <thread>

#include "helper.h"
#include "nexer/core/tcp_client.h"
#include "nexer/core/tcp_connector.h"
#include "nexer/core/tcp_server.h"
#include "nexer/core/timer.h"

namespace nexer {
namespace test {

static void StartServerOnce(int port, std::promise<void> ready) {
    EventLoop loop;
    auto server = TcpServer::Create(loop);
    std::weak_ptr<TcpServer> weak_server = server;
    server->OnConnection([weak_server](std::shared_ptr<TcpClient> client) {
        client->close();
        if (auto server = weak_server.lock()) {
            server->close();
        }
    });
    server->Listen(port);
    ready.set_value();
    loop.run();
}

static void StartServerAfterDelay(int port, uint64_t delay_ms,
                                  std::promise<void> ready) {
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
        if (!server->Listen(port)) {
            ready.set_value();
            return;
        }
        ready.set_value();
    };

    std::shared_ptr<Timer> timer;

    if (delay_ms == 0) {
        start_server();
    } else {
        timer = Timer::create(loop, delay_ms);
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
}

static void StartServerImmediateClose(int port, std::promise<void> ready) {
    EventLoop loop;
    auto server = TcpServer::Create(loop);
    std::weak_ptr<TcpServer> weak_server = server;
    server->OnConnection([weak_server](std::shared_ptr<TcpClient> client) {
        client->close();
        if (auto server = weak_server.lock()) {
            server->close();
        }
    });
    server->Listen(port);
    ready.set_value();
    loop.run();
}

static void TestConnectSuccess() {
    int port = PickFreePort();
    assert(port > 0);

    std::promise<void> ready;
    auto ready_future = ready.get_future();
    std::thread server_thread(StartServerOnce, port, std::move(ready));
    ready_future.wait();

    EventLoop loop;
    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "127.0.0.1",
        .port = port,
        .deadline_ms = 250,
        .retry_interval_ms = 30,
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
    server_thread.join();

    assert(connected);
    assert(callback_called == 1);
    assert(try_count >= 1);
}

static void TestConnectDeadline() {
    int port = PickFreePort();
    assert(port > 0);

    EventLoop loop;
    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "127.0.0.1",
        .port = port,
        .deadline_ms = 200,
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

    assert(!connected);
    assert(callback_called == 1);
    assert(try_count > 1);
}

static void TestTimeoutReleasesGuard() {
    int port = PickFreePort();
    assert(port > 0);

    EventLoop loop;
    bool callback_called = false;
    std::weak_ptr<TcpConnector> weak_connector;

    {
        TcpConnectOptions opts{
            .host = "127.0.0.1",
            .port = port,
            .deadline_ms = 150,
            .retry_interval_ms = 40,
        };

        auto connector = TcpConnector::create(
            loop, std::move(opts), [&](std::shared_ptr<TcpClient> client) {
                callback_called = true;
                assert(client == nullptr);
            });

        weak_connector = connector;
    }

    loop.run();

    assert(callback_called);
    assert(weak_connector.expired());
}

static void TestCancelInFlight() {
    int port = PickFreePort();
    assert(port > 0);

    EventLoop loop;
    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "127.0.0.1",
        .port = port,
        .deadline_ms = 300,
        .retry_interval_ms = 50,
    };

    auto connector = TcpConnector::create(
        loop, std::move(opts),
        [&](std::shared_ptr<TcpClient> client) {
            connected = client != nullptr;
            callback_called++;
        },
        [&](TcpClient &) { try_count++; });

    auto timer = Timer::create(loop, 60);
    timer->on_tick([timer, connector] {
        timer->close();
        connector->cancel();
    });
    timer->start();

    loop.run();

    assert(!connected);
    assert(callback_called == 1);
    assert(try_count >= 1);
}

static void TestImmediateCloseServer() {
    int port = PickFreePort();
    assert(port > 0);

    std::promise<void> ready;
    auto ready_future = ready.get_future();
    std::thread server_thread(StartServerImmediateClose, port,
                              std::move(ready));
    ready_future.wait();

    EventLoop loop;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "127.0.0.1",
        .port = port,
        .deadline_ms = 300,
        .retry_interval_ms = 50,
    };

    auto connector = TcpConnector::create(
        loop, std::move(opts), [&](std::shared_ptr<TcpClient> client) {
            (void)client;
            callback_called++;
        });
    (void)connector;

    loop.run();
    server_thread.join();

    assert(callback_called == 1);
}

static void TestRetryThenSuccess() {
    int port = PickFreePort();
    assert(port > 0);

    std::promise<void> ready;
    std::thread server_thread(StartServerAfterDelay, port, 60,
                              std::move(ready));

    EventLoop loop;
    int try_count = 0;
    bool connected = false;
    int callback_called = 0;

    TcpConnectOptions opts{
        .host = "127.0.0.1",
        .port = port,
        .deadline_ms = 260,
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
    server_thread.join();

    assert(connected);
    assert(callback_called == 1);
    assert(try_count > 1);
}

void TestTcpConnector() {
    TestConnectSuccess();
    TestConnectDeadline();
    TestTimeoutReleasesGuard();
    TestCancelInFlight();
    TestImmediateCloseServer();
    TestRetryThenSuccess();
}

} // namespace test
} // namespace nexer
