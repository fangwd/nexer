#include "nexer/http/http_server.h"

namespace nexer {

namespace http {

Server::Server(uv_loop_t *loop) : TcpServer(loop) {
    incoming::Message::InitParserSettings();
}

std::shared_ptr<Server> Server::create(EventLoop &loop) {
    auto server = std::shared_ptr<Server>(new Server(loop));
    server->Init();
    return server;
}

void Server::Init() {
    auto self_weak = std::weak_ptr<Server>(
        std::static_pointer_cast<Server>(shared_from_this()));
    TcpServer::OnConnection([self_weak](std::shared_ptr<TcpClient> client) {
        auto self = self_weak.lock();
        if (!self)
            return;

        if (self->on_connection_) {
            self->on_connection_(*client);
        }

        auto request = std::make_shared<incoming::Request>(*self, client);
        self->active_requests_.insert(request);

        request->OnComplete(on_request_complete);

        auto client_weak = std::weak_ptr<TcpClient>(client);
        auto request_weak = std::weak_ptr<incoming::Request>(request);

        client->on_data([client_weak, request_weak](const char *s, size_t len) {
            auto client = client_weak.lock();
            auto request = request_weak.lock();
            if (!client || !request)
                return;

            if (request->IsComplete()) {
                // Note: we do not implement strict HTTP/1.1 pipelining
                // semantics.
                request->Reset();
            }
            if (request->Parse(s, len)) {
                client->close();
            }
        });

        client->on_error([client_weak](int err, const char *msg) {
            if (auto client = client_weak.lock()) {
                client->close();
            }
        });

        client->on_close([request_weak, self_weak]() {
            auto self = self_weak.lock();
            if (!self)
                return;
            if (auto request = request_weak.lock()) {
                request->DetachClient();
                self->active_requests_.erase(request);
            }
        });
    });
}

void Server::close() {
    // Close all active client connections so their handles are properly
    // removed from the event loop before the server is destroyed.
    auto requests = std::move(active_requests_);
    for (auto &req : requests) {
        if (auto client = req->client()) {
            if (!client->is_closing()) {
                client->close();
            }
        }
    }
    Handle::close();
}

void Server::on_request_complete(incoming::Message *message) {
    auto request =
        dynamic_cast<incoming::Request *>(message)->shared_from_this();
    auto &response = request->response();
    auto &server = request->server();

    if (server.on_request_) {
        server.on_request_(request, response);
    }
}

} // namespace http
} // namespace nexer
