#include <assert.h>

#include <thread>

#include "nexer/core/logger.h"
#include "nexer/core/string_buffer.h"
#include "nexer/core/tcp_server.h"

namespace nexer {
namespace test {

#define TEST_PORT 19000

static void start_tcp_server(int port) {
    EventLoop loop;
    auto server = TcpServer::Create(loop);

    std::weak_ptr<TcpServer> weak_server = server;
    server->OnConnection([weak_server](std::shared_ptr<TcpClient> client) {
        client->on_data([client, weak_server](const char *s, size_t len) {
            static char copy[256];
            memcpy(copy, s, len);
            client->write(copy, len);
            if (len > 1 && memcmp(s, "QS", 2) == 0) {
                client->close();
            } else if (len > 0 && s[0] == 'Q') {
                if (auto server = weak_server.lock()) {
                    server->close();
                }
            }
        });
    });

    server->Listen(port);

    loop.run();
}

void run_test_echo_server(void (*start_server)(int));

void TestTcpServer() { run_test_echo_server(start_tcp_server); }

} // namespace test
} // namespace nexer
