/* Copyright (c) Weidong Fang. All rights reserved. */

#include <assert.h>

#include <thread>

#include "logger.h"
#include "string_buffer.h"
#include "tcp_server.h"

namespace nexer {
namespace test {

#define TEST_PORT 19000

static void start_tcp_server(int port) {
    EventLoop loop;
    TcpServer& server = TcpServer::Create(loop);

    server.OnConnection([&server](TcpClient& client) {
        client.OnData([&](const char* s, size_t len) {
            static char copy[256];
            memcpy(copy, s, len);
            client.Write(copy, len);
            if (len > 1 && memcmp(s, "QS", 2) == 0) {
                client.Close();
            } else if (len > 0 && s[0] == 'Q') {
                server.Close();
            }
        });
    });

    server.Listen(port);

    loop.Run();
}

void run_test_echo_server(void (*start_server)(int));

void TestTcpServer() {
    run_test_echo_server(start_tcp_server);
}

}  // namespace test
}  // namespace nexer
