/* Copyright (c) Weidong Fang. All rights reserved. */

#include "tcp_client.h"

#include "logger.h"
#include "timer.h"

namespace nexer {

TcpClient::TcpClient(uv_loop_t *loop) : flags_{0} {
    if (int status = uv_tcp_init(loop, &tcp_)) {
        log_fatal("uv_tcp_init: %s", uv_strerror(status));
        exit(1);
    }

    tcp_.data = this;
}

TcpClient &TcpClient::Create(EventLoop &loop) {
    auto client = new TcpClient(loop);
    return *client;
}

TcpClient &TcpClient::Create(uv_loop_t *loop) {
    auto client = new TcpClient(loop);
    return *client;
}

struct WriteRequest {
    TcpClient *client;
    uv_write_t req;
    uv_buf_t buf;
};

struct GetAddrInfoRequest {
    TcpClient *client;
    uv_getaddrinfo_t req;
};

// Internal callbacks

void TcpClient::OnAddrInfo(uv_getaddrinfo_t *resolver, int status, struct addrinfo *res) {
    auto req = reinterpret_cast<GetAddrInfoRequest *>(resolver->data);
    auto client = req->client;

    client->flags_.name_resolving = 0;

    if (status < 0) {
        client->OnError("getaddrinfo", status);
        delete req;
        return;
    }

    client->ConnectAddr((const struct sockaddr *)res->ai_addr);

    uv_freeaddrinfo(res);

    delete req;
}

void TcpClient::ReadStart() {
    if (int status = uv_read_start((uv_stream_t *)&tcp_, OnAlloc, OnRead)) {
        OnError("uv_read_start", status);
    }
}

void TcpClient::OnConnect(uv_connect_t *req, int status) {
    auto client = reinterpret_cast<TcpClient *>(req->data);
    client->flags_.connecting = 0;
    if (status) {
        client->OnError("connect", status);
    } else {
        if (client->on_connect_) {
            client->on_connect_();
        }
        client->ReadStart();
    }
    free(req);
}

void TcpClient::OnAlloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

void TcpClient::OnRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    auto client = reinterpret_cast<TcpClient *>(stream->data);
    if (nread < 0) {
        if (nread == UV_EOF) {
            client->Close();
        } else {
            client->OnError("read", nread);
        }
    } else if (client->on_data_) {
        client->on_data_(buf->base, nread);
    }
    free(buf->base);
}

void TcpClient::OnWrite(uv_write_t *req, int status) {
    auto request = reinterpret_cast<WriteRequest *>(req->data);
    auto client = request->client;
    delete request;
    if (status) {
        client->OnError("write", status);
    } else if (client->on_send_) {
        client->on_send_();
    }
}

void TcpClient::OnWrite2(uv_write_t *req, int status) {
    auto client = reinterpret_cast<TcpClient *>(req->data);
    free(req);
    if (status) {
        client->OnError("write", status);
    } else if (client->on_send_) {
        client->on_send_();
    }
}

// Private methods:

void TcpClient::GetAddrInfo(const char *node, const char *service) {
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = 0;

    auto req = new GetAddrInfoRequest;

    req->client = this;
    req->req.data = req;

    flags_.name_resolving = 1;
    if (int status = uv_getaddrinfo(tcp_.loop, &req->req, OnAddrInfo, node, service, &hints)) {
        OnError("getaddrinfo", status);
        flags_.name_resolving = 0;
        delete req;
    }
}

void TcpClient::ConnectAddr(const struct sockaddr *addr) {
    uv_connect_t *req = (uv_connect_t *)malloc(sizeof *req);
    req->data = this;
    flags_.connecting = 1;
    if (int status = uv_tcp_connect(req, &tcp_, addr, OnConnect)) {
        free(req);
        OnError("uv_tcp_connect", status);
        flags_.connecting = 0;
    }
}

// Public methods

void TcpClient::Connect(const char *host, int port) {
    static char service[32];
    snprintf(service, sizeof service, "%d", port);
    GetAddrInfo(host, service);
}

void TcpClient::Connect(int port) {
    Connect("127.0.0.1", port);
}

void TcpClient::Write(const char *data, size_t len) {
    WriteRequest *req = new WriteRequest();
    int status;

    req->client = this;
    req->req.data = req;
    req->buf = uv_buf_init((char *)data, len);

    if ((status = uv_write(&req->req, (uv_stream_t *)&tcp_, &req->buf, 1, OnWrite)) != 0) {
        delete req;
        OnError("write", status);
    }
}

void TcpClient::Write(uv_buf_t *buf, size_t len) {
    uv_write_t *req = (uv_write_t *)malloc(sizeof *req);

    req->data = this;

    if (int status = uv_write(req, (uv_stream_t *)&tcp_, buf, len, OnWrite2)) {
        free(req);
        OnError("write", status);
    }
}

void TcpClient::Connect(EventLoop &loop, const char *host, int port, uint64_t timeout,
                        std::function<void(TcpClient*)> then, std::function<void(TcpClient&)> on_try) {
    nexer::Timer &timer = nexer::Timer::Create(loop, 1000);
    auto connect = [&loop, &timer, host, port, then, on_try] {
        auto& client = TcpClient::Create(loop);
        client.OnConnect([&timer, host, port, then] {
            log_debug("tcp_client: connected to %s:%d", host, port);
            timer.Close();
            auto client = (TcpClient*)timer.data();
            then(client);
        });
        client.OnError([&timer, host, port](int err, const char *msg) {
            log_debug("tcp_client: failed to connect to %s:%d (%s)", host, port, msg);
            ((TcpClient*)timer.data())->Close();
        });
        client.OnClose([&timer] {
            timer.SetData(nullptr);
        });
        if (on_try) {
            on_try(client);
        }
        client.Connect(host, port);
        timer.SetData(&client);
    };

    timer.OnTick([&loop, &timer, timeout, then, connect, host, port] {
        if (timer.GetElapsedTime() >= timeout) {
            auto client = (TcpClient*)timer.data();
            if (client) {
                client->Close();
            }
            timer.Close();
            then(nullptr);
        } else {
            if (!timer.data()) {
                log_debug("tcp_client: connecting %s:%d", host, port);
                connect();
            }
        }
    });

    timer.SetData(nullptr);
    timer.Start();
}

}  // namespace nexer