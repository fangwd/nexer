#include <assert.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "helper.h"
#define private public
#include "nexer/runtime/nexer.h"
#undef private
#include "nexer/core/timer.h"

namespace nexer {
namespace test {

namespace {

struct DummyEndpoint {
    config::Dummy::Type type;
    int port;
};

int ReservePort(std::set<int> &used_ports) {
    for (int attempt = 0; attempt < 128; ++attempt) {
        int port = PickFreePort();
        if (port <= 0) {
            port = 55000 + attempt + static_cast<int>(used_ports.size());
        }
        if (used_ports.insert(port).second) {
            return port;
        }
    }
    assert(false && "failed to reserve unique port");
    return 0;
}

// Connects, sends a payload and returns whatever arrives within a second.
std::string ExchangeTcp(int port, const std::string &payload) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    assert(send(fd, payload.data(), payload.size(), 0) ==
           static_cast<ssize_t>(payload.size()));
    timeval tv{};
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buffer[64];
    auto n = recv(fd, buffer, sizeof buffer, 0);
    close(fd);
    return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : std::string();
}

void SendUdp(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    const char payload[] = "pong";
    sendto(fd, payload, sizeof(payload), 0, reinterpret_cast<sockaddr *>(&addr),
           sizeof(addr));
    close(fd);
}

} // namespace

void TestDummyServers() {
    Config config;
    std::set<int> used_ports;
    std::vector<DummyEndpoint> endpoints;

    constexpr int kUdpServers = 10;
    for (int i = 0; i < kUdpServers; ++i) {
        config::Dummy udp;
        udp.type = config::Dummy::Type::Udp;
        udp.echo = true;
        udp.port = ReservePort(used_ports);
        endpoints.push_back({udp.type, udp.port});
        config.dummies().push_back(udp);
    }
    config::Dummy tcp_echo;
    tcp_echo.type = config::Dummy::Type::Tcp;
    tcp_echo.echo = true;
    tcp_echo.port = ReservePort(used_ports);
    config.dummies().push_back(tcp_echo);
    config::Dummy tcp_sink;
    tcp_sink.type = config::Dummy::Type::Tcp;
    tcp_sink.port = ReservePort(used_ports);
    config.dummies().push_back(tcp_sink);

    Nexer nexer(config);

    for (auto &dummy : config.dummies()) {
        assert(nexer.start_dummy_server(dummy));
    }

    assert(nexer.dummy_servers_.size() == endpoints.size() + 2);

    // Close once the sender finished so every connection sees a live listener.
    std::atomic<bool> sent{false};
    auto timer = Timer::create(nexer.loop_, 20);
    std::weak_ptr<Timer> weak_timer = timer;
    timer->on_tick([&] {
        if (!sent.load()) {
            return;
        }
        if (auto t = weak_timer.lock()) {
            t->close();
        }
        nexer.close();
    });
    assert(timer->start());

    std::string echoed;
    std::string sunk;
    std::thread sender_thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (const auto &endpoint : endpoints) {
            SendUdp(endpoint.port);
        }
        // Several connections in a row, including ones closed by the peer
        // while the loop keeps running.
        for (int i = 0; i < 3; ++i) {
            echoed = ExchangeTcp(tcp_echo.port, "ping");
            sunk = ExchangeTcp(tcp_sink.port, "ping");
        }
        sent.store(true);
    });

    nexer.loop_.run();

    sender_thread.join();

    assert(echoed == "ping");
    assert(sunk.empty());
    assert(nexer.dummy_servers_.empty());
}

} // namespace test
} // namespace nexer
