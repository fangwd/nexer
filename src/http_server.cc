/* Copyright (c) Weidong Fang. All rights reserved. */

#include "http_server.h"

namespace nexer {

namespace http {

Server::Server(uv_loop_t* loop) : TcpServer(loop) {
    incoming::Message::InitParserSettings();
    Init();
}

Server& Server::Create(EventLoop& loop) {
    auto server = new Server(loop);
    return *server;
}

void Server::Init() {
    TcpServer::OnConnection([this](TcpClient& client) {
        if (on_connection_) {
            on_connection_(client);
        }

        auto request = new incoming::Request(*this, client);

        request->OnComplete(OnRequestComplete);

        client.OnData([&client, request](const char* s, size_t len) {
            if (request->IsComplete()) {
                request->Reset();
            }
            if (request->Parse(s, len)) {
                client.Close();
            }
        });

        client.OnError([&client](int err, const char* msg) {
            client.Close();
        });

        client.OnClose([request]() {
            delete request;
        });
    });
}

void Server::OnRequestComplete(incoming::Message* message) {
    auto request = dynamic_cast<incoming::Request*>(message);
    auto& response= request->response_;
    auto& server = request->server();

    if (server.on_request_) {
        server.on_request_(*request, response);
    }
}

}  // namespace http
}  // namespace nexer
