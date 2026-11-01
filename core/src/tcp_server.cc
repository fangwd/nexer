#include "nexer/core/tcp_server.h"

#include "nexer/core/logger.h"
#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

namespace nexer {

TcpServer::TcpServer(uv_loop_t *loop) {
    if (int status = uv_tcp_init(loop, &tcp_)) {
        log_error("uv_tcp_init: %s", uv_strerror(status));
    }

    tcp_.data = this;
}

std::shared_ptr<TcpServer> TcpServer::Create(EventLoop &loop) {
    auto self = std::shared_ptr<TcpServer>(new TcpServer(loop));
    self->on_close_.set_owner(self);
    self->on_error_.set_owner(self);
    return self;
}

void TcpServer::OnConnection(uv_stream_t *stream, int status) {
    auto &server = *reinterpret_cast<TcpServer *>(stream->data);

    if (status != 0) {
        server.on_error("connection", status);
        return;
    }

    auto client = TcpClient::create(server.loop());
    if (!client) {
        server.on_error("accept", UV_ENOMEM);
        return;
    }

    if ((status = server.Accept(*client))) {
        if (!client->is_closing()) {
            client->close();
        }
        server.on_error("accept", status);
        return;
    }

    client->read_start();

    if (server.on_connection_) {
        server.on_connection_(client);
    }
}

int TcpServer::Accept(TcpClient &client) {
    return uv_accept((uv_stream_t *)&tcp_, (uv_stream_t *)&client.tcp_);
}

namespace {

bool FillSockaddr(const std::string &host, int port, sockaddr_storage &storage,
                  int &family) {
    memset(&storage, 0, sizeof(storage));

    sockaddr_in addr4;
    if (uv_ip4_addr(host.c_str(), port, &addr4) == 0) {
        memcpy(&storage, &addr4, sizeof(addr4));
        family = AF_INET;
        return true;
    }

#if defined(AF_INET6)
    sockaddr_in6 addr6;
    if (uv_ip6_addr(host.c_str(), port, &addr6) == 0) {
        memcpy(&storage, &addr6, sizeof(addr6));
        family = AF_INET6;
        return true;
    }
#endif

    log_error("tcp bind: unable to parse bind address %s", host.c_str());
    return false;
}

} // namespace

bool TcpServer::Listen(const std::string &host, int port) {
    sockaddr_storage storage;
    int family = AF_INET;
    if (!FillSockaddr(host, port, storage, family)) {
        return false;
    }

    uv_os_fd_t fd;
    if (uv_fileno(reinterpret_cast<uv_handle_t *>(&tcp_), &fd) == 0) {
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
            log_warn("tcp reuseaddr: %s", strerror(errno));
        }
#if defined(IPV6_V6ONLY)
        if (family == AF_INET6) {
            opt = 0;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) !=
                0) {
                log_warn("tcp v6only: %s", strerror(errno));
            }
        }
#endif
    }

    int err;
    if ((err = uv_tcp_bind(
             &tcp_, reinterpret_cast<const struct sockaddr *>(&storage), 0))) {
        log_error("tcp bind: %s (%d)", uv_strerror(err), err);
        return false;
    }

    int backlog = SOMAXCONN;

    if ((err = uv_listen((uv_stream_t *)&tcp_, backlog, OnConnection))) {
        log_error("listen: %s (port %d)", uv_strerror(err), port);
        return false;
    }

    if (on_listening_) {
        on_listening_();
    }

    return true;
}

bool TcpServer::Listen(int port) { return Listen("0.0.0.0", port); }

} // namespace nexer
