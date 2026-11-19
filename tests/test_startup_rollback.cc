#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define private public
#include "nexer/runtime/nexer.h"
#undef private

namespace nexer {
namespace test {

void TestStartupRollback() {
    Config config;

    auto reserve_port = []() {
        int last_errno = 0;
        for (int attempt = 0; attempt < 128; ++attempt) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                last_errno = errno;
                continue;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;

            if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
                0) {
                socklen_t len = sizeof(addr);
                if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr),
                                &len) == 0) {
                    int port = ntohs(addr.sin_port);
                    close(fd);
                    if (port > 0) {
                        return port;
                    }
                }
            } else {
                last_errno = errno;
            }

            close(fd);
        }
        fprintf(stderr, "reserve_port failed errno=%d (%s)\n", last_errno,
                strerror(last_errno));
        assert(!"failed to find free port");
        return 0;
    };

    int proxy_port = reserve_port();

    config::Proxy proxy;
    proxy.port = proxy_port;
    // Never reached; avoids accidental success connections.
    proxy.upstreams.push_back({.host = "127.0.0.1", .port = 65535});

    config.proxies().push_back(proxy);
    config.proxies().push_back(proxy); // Duplicate port to force bind failure.

    Nexer nexer(config);

    assert(nexer.run() == 1);

    assert(nexer.loop_.run());

    assert(nexer.proxies_.empty());
    assert(nexer.dummy_servers_.empty());

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(proxy_port);

    int bind_result =
        bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (bind_result != 0) {
        fprintf(stderr, "bind failed: %s (%d)\n", strerror(errno), errno);
    }
    close(fd);

    assert(bind_result == 0 && "Port should be released after startup failure");
}

} // namespace test
} // namespace nexer
