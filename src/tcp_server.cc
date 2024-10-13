#include "tcp_server.h"

#include "logger.h"

namespace nexer {

TcpServer::TcpServer(uv_loop_t* loop) {
    if (int status = uv_tcp_init(loop, &tcp_)) {
        log_fatal("uv_tcp_init: %s", uv_strerror(status));
    }

    tcp_.data = this;
}

TcpServer& TcpServer::Create(EventLoop& loop) {
    auto server = new TcpServer(loop);
    return *server;
}

void TcpServer::OnConnection(uv_stream_t *stream, int status) {
    auto &server = *reinterpret_cast<TcpServer *>(stream->data);

    if (status != 0) {
        server.OnError("connection", status);
        return;
    }

    auto& client = TcpClient::Create(server.loop());

    if ((status = server.Accept(client))) {
        server.OnError("accept", status);
        return;
    }

    client.ReadStart();

    if (server.on_connection_) {
        server.on_connection_(client);
    }
}

int TcpServer::Accept(TcpClient& client) {
    return  uv_accept((uv_stream_t*)&tcp_, (uv_stream_t*) &client.tcp_);
}

bool TcpServer::Listen(int port) {
    struct sockaddr_in addr;
    int err;

    if ((err = uv_ip4_addr("0.0.0.0", port, &addr))) {
        log_error("ip4 addr: %s (%d)", uv_strerror(err), err);
        return false;
    }

    if ((err = uv_tcp_bind(&tcp_, (const struct sockaddr *)&addr, 0))) {
        log_error("tcp bind: %s (%d)", uv_strerror(err), err);
        return false;
    }

    int backlog = 8;

    if ((err = uv_listen((uv_stream_t *)&tcp_, backlog, OnConnection))) {
        log_error("listen: %s (port %d)", uv_strerror(err), port);
        return false;
    }

    if (on_listening_) {
        on_listening_();
    }

    return true;
}

}  // namespace nexer