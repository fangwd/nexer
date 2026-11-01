#include "nexer/core/tcp_client.h"

#include "nexer/core/logger.h"
#include "nexer/core/timer.h"
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace nexer {

TcpClient::TcpClient(uv_loop_t *loop) : flags_{0} {
    if (!loop) {
        log_error("tcp_client: null event loop");
        return;
    }

    if (int status = uv_tcp_init(loop, &tcp_)) {
        log_error("uv_tcp_init: %s", uv_strerror(status));
        return;
    }

    tcp_.data = this;
    initialized_ = true;
}

std::shared_ptr<TcpClient> TcpClient::create(EventLoop &loop) {
    if (!loop.is_initialised()) {
        log_error("tcp_client: event loop not initialized");
        return nullptr;
    }

    auto self = std::shared_ptr<TcpClient>(new TcpClient(loop));
    if (!self->initialized_) {
        return nullptr;
    }
    self->on_connect_.set_owner(self);
    self->on_send_.set_owner(self);
    self->on_data_.set_owner(self);
    self->on_close_.set_owner(self);
    self->on_error_.set_owner(self);
    return self;
}

std::shared_ptr<TcpClient> TcpClient::create(uv_loop_t *loop) {
    auto self = std::shared_ptr<TcpClient>(new TcpClient(loop));
    if (!self->initialized_) {
        return nullptr;
    }
    self->on_connect_.set_owner(self);
    self->on_send_.set_owner(self);
    self->on_data_.set_owner(self);
    self->on_close_.set_owner(self);
    self->on_error_.set_owner(self);
    return self;
}

namespace {

template <typename ReqT> struct UvRequestHolder {
    ReqT req;
    std::shared_ptr<TcpClient> owner;

    explicit UvRequestHolder(std::shared_ptr<TcpClient> o)
        : owner(std::move(o)) {
        req.data = this;
    }
};

struct TcpClientWriteRequest {
    std::shared_ptr<TcpClient> client;
    uv_write_t req;
    uv_buf_t buf;
    std::vector<char> storage;
    std::shared_ptr<std::string> shared_storage;
};

struct GetAddrInfoRequest {
    std::shared_ptr<TcpClient> client;
    uv_getaddrinfo_t req;
};

} // namespace

// Internal callbacks

void TcpClient::on_addr_info(uv_getaddrinfo_t *resolver, int status,
                             struct addrinfo *res) {
    auto req = reinterpret_cast<GetAddrInfoRequest *>(resolver->data);
    auto client = req->client;

    client->flags_.name_resolving = 0;

    if (status < 0) {
        client->on_error("getaddrinfo", status);
        delete req;
        return;
    }

    client->connect_addr((const struct sockaddr *)res->ai_addr);

    uv_freeaddrinfo(res);

    delete req;
}

void TcpClient::read_start() {
    if (!initialized_) {
        log_error("tcp_client: read start on uninitialized client");
        return;
    }
    if (int status = uv_read_start((uv_stream_t *)&tcp_, on_alloc, on_read)) {
        on_error("uv_read_start", status);
    }
}

void TcpClient::on_connect(uv_connect_t *req, int status) {
    auto holder = static_cast<UvRequestHolder<uv_connect_t> *>(req->data);
    std::shared_ptr<TcpClient> client;

    if (holder) {
        client = holder->owner;
        delete holder;
    }

    if (!client) {
        return;
    }

    client->flags_.connecting = 0;
    if (status) {
        client->on_error("connect", status);
    } else {
        client->on_connect_.invoke();
        client->read_start();
    }
}

void TcpClient::on_alloc(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

void TcpClient::on_read(uv_stream_t *stream, ssize_t nread,
                        const uv_buf_t *buf) {
    auto client = static_cast<TcpClient *>(stream->data);
    auto self = client->shared_from_this();
    if (nread < 0) {
        if (nread == UV_EOF) {
            client->close();
        } else {
            client->on_error("read", nread);
        }
    } else {
        client->on_data_.invoke(buf->base, nread);
    }
    free(buf->base);
}

void TcpClient::on_write(uv_write_t *req, int status) {
    auto request = reinterpret_cast<TcpClientWriteRequest *>(req->data);
    auto client = request->client;
    if (status) {
        client->on_error("write", status);
    } else {
        client->on_send_.invoke();
    }
    delete request;
}

// Private methods:

void TcpClient::get_addr_info(const char *node, const char *service) {
    if (!initialized_) {
        log_error("tcp_client: getaddrinfo on uninitialized client");
        return;
    }
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = 0;

    auto req = new GetAddrInfoRequest;

    req->client = std::static_pointer_cast<TcpClient>(shared_from_this());
    req->req.data = req;

    flags_.name_resolving = 1;
    if (int status = uv_getaddrinfo(tcp_.loop, &req->req, on_addr_info, node,
                                    service, &hints)) {
        on_error("getaddrinfo", status);
        flags_.name_resolving = 0;
        delete req;
    }
}

void TcpClient::connect_addr(const struct sockaddr *addr) {
    auto holder = new UvRequestHolder<uv_connect_t>(
        std::static_pointer_cast<TcpClient>(shared_from_this()));
    flags_.connecting = 1;
    if (int status = uv_tcp_connect(&holder->req, &tcp_, addr, on_connect)) {
        delete holder;
        on_error("uv_tcp_connect", status);
        flags_.connecting = 0;
    }
}

// Public methods

void TcpClient::connect(const char *host, int port) {
    if (!initialized_) {
        log_error("tcp_client: connect on uninitialized client");
        return;
    }
    char service[32];
    snprintf(service, sizeof service, "%d", port);
    get_addr_info(host, service);
}

void TcpClient::connect(int port) { connect("127.0.0.1", port); }

void TcpClient::write(const char *data, size_t len) {
    if (!initialized_) {
        log_error("tcp_client: write on uninitialized client");
        return;
    }
    auto *req = new TcpClientWriteRequest();
    int status;

    if (len > std::numeric_limits<unsigned int>::max()) {
        delete req;
        on_error("write", UV_E2BIG);
        return;
    }

    req->client = std::static_pointer_cast<TcpClient>(shared_from_this());
    req->req.data = req;
    req->storage.assign(data, data + len);
    req->buf = uv_buf_init(req->storage.empty() ? nullptr : req->storage.data(),
                           static_cast<unsigned int>(req->storage.size()));

    if ((status = uv_write(&req->req, (uv_stream_t *)&tcp_, &req->buf, 1,
                           on_write)) != 0) {
        delete req;
        on_error("write", status);
    }
}

void TcpClient::write(std::shared_ptr<std::string> data) {
    if (!initialized_) {
        log_error("tcp_client: write on uninitialized client");
        return;
    }
    if (!data) {
        on_error("write", UV_EINVAL);
        return;
    }
    if (data->size() > std::numeric_limits<unsigned int>::max()) {
        on_error("write", UV_E2BIG);
        return;
    }

    auto *req = new TcpClientWriteRequest();
    req->client = std::static_pointer_cast<TcpClient>(shared_from_this());
    req->req.data = req;
    req->shared_storage = std::move(data);
    auto *buf_data = req->shared_storage->empty()
                         ? nullptr
                         : const_cast<char *>(req->shared_storage->data());
    req->buf = uv_buf_init(
        buf_data, static_cast<unsigned int>(req->shared_storage->size()));

    int status =
        uv_write(&req->req, (uv_stream_t *)&tcp_, &req->buf, 1, on_write);
    if (status != 0) {
        delete req;
        on_error("write", status);
    }
}

void TcpClient::close() {
    if (!initialized_) {
        return;
    }
    on_connect_.clear();
    on_send_.clear();
    on_data_.clear();
    Handle::close();
}

} // namespace nexer
