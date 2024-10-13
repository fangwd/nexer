#include "udp_server.h"

#include "logger.h"

namespace nexer {

UdpServer::UdpServer(uv_loop_t* loop) {
    if (int status = uv_udp_init(loop, &udp_)) {
        log_fatal("uv_udp_init: %s", uv_strerror(status));
    }

    udp_.data = this;
}

UdpServer& UdpServer::Create(EventLoop& loop) {
    auto server = new UdpServer(loop);
    return *server;
}

bool UdpServer::Listen(int port) {
    struct sockaddr_in addr;
    int err;

    if ((err = uv_ip4_addr("0.0.0.0", port, &addr))) {
        log_error("ip4 addr: %s (%d)", uv_strerror(err), err);
        return false;
    }

    if ((err = uv_udp_bind(&udp_, (const struct sockaddr*)&addr, 0))) {
        log_error("udp bind: %s (%d)", uv_strerror(err), err);
        return false;
    }

    int backlog = 8;

    if ((err = uv_udp_recv_start(&udp_, OnAlloc, OnRecv))) {
        log_error("listen: %s (%d)", uv_strerror(err), err);
        return false;
    }

    return true;
}

void UdpServer::OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    auto server = (UdpServer*)handle;
    buf->base = server->slab_;
    buf->len = sizeof(server->slab_);
}

void UdpServer::OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* rcvbuf, const struct sockaddr* addr,
                       unsigned flags) {
    auto server = (UdpServer*)handle->data;

    if (nread == 0) {
        return;
    }

    if (nread < 0) {
        server->on_error_.Invoke(nread, uv_strerror(nread));
        return;
    }

    server->on_recv_.Invoke(rcvbuf->base, nread, addr);
}

}  // namespace nexer