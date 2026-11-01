#include "nexer/core/udp_server.h"

#include "nexer/core/logger.h"

namespace nexer {

UdpServer::UdpServer(uv_loop_t *loop) {
    if (int status = uv_udp_init(loop, &udp_)) {
        log_fatal("uv_udp_init: %s", uv_strerror(status));
    }

    udp_.data = this;
}

std::shared_ptr<UdpServer> UdpServer::create(EventLoop &loop) {
    auto self = std::shared_ptr<UdpServer>(new UdpServer(loop));
    self->on_close_.set_owner(self);
    self->on_error_.set_owner(self);
    self->on_recv_.set_owner(self);
    return self;
}

bool UdpServer::listen(int port) {
    struct sockaddr_in addr;
    int err;

    if ((err = uv_ip4_addr("0.0.0.0", port, &addr))) {
        log_error("ip4 addr: %s (%d)", uv_strerror(err), err);
        return false;
    }

    if ((err = uv_udp_bind(&udp_, (const struct sockaddr *)&addr, 0))) {
        log_error("udp bind: %s (%d)", uv_strerror(err), err);
        return false;
    }

    int backlog = 8;

    if ((err = uv_udp_recv_start(&udp_, on_alloc, handle_recv))) {
        log_error("listen: %s (%d)", uv_strerror(err), err);
        return false;
    }

    return true;
}

void UdpServer::on_alloc(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf) {
    auto server = static_cast<UdpServer *>(handle->data);
    buf->base = server->slab_;
    buf->len = sizeof(server->slab_);
}

void UdpServer::handle_recv(uv_udp_t *handle, ssize_t nread,
                            const uv_buf_t *rcvbuf, const struct sockaddr *addr,
                            unsigned flags) {
    auto server = static_cast<UdpServer *>(handle->data);
    auto self = server->shared_from_this();

    if (nread == 0) {
        return;
    }

    if (nread < 0) {
        server->on_error_.invoke(nread, uv_strerror(nread));
        return;
    }

    server->on_recv_.invoke(rcvbuf->base, nread, addr);
}

} // namespace nexer
