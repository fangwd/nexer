#include "nexer.h"
#include "tcp_forwarder.h"
#include "udp_server.h"
#include <assert.h>

namespace nexer {

Nexer::Nexer(Config& config) : config_(config), admin_server_(nullptr) {
    process_manager_ = new ProcessManager(loop_);
    process_manager_->OnProcessData([](const Process*, int, const char *s, size_t len) {
        printf("%.*s", (int)len, s);
    });
}

Nexer::~Nexer() {
    Close();
    delete process_manager_;
}

bool Nexer::StartAdminServer() {
    auto& server = http::Server::Create(loop_);
    server.OnRequest([this](http::incoming::Request& req, http::outgoing::Response& res) {
        if (req.url().path == "/shutdown") {
            Close();
        }
        res.End(200);
    });
    admin_server_ = &server;
    return server.Listen(config_.admin().port);
}

bool Nexer::Start() {
    if (!StartAdminServer()) {
        return false;
    }
    for (auto& config: config_.proxies()) {
        TcpProxy& proxy = TcpProxy::Create(loop_, config.upstream, process_manager_);
        if (!proxy.Listen(config.port)) {
            return false;
        }
        proxies_.push_back(&proxy);
    }
    for (auto& config: config_.dummies()) {
        if (!StartDummyServer(config)) {
            // TODO: close gracefully
            return false;
        }
    }
    loop_.Run();
    return true;
}

void Nexer::Close() {
    for (auto proxy: proxies_) {
        proxy->Close();
    }
    proxies_.clear();
    if (admin_server_) {
        admin_server_->Close();
        admin_server_ = nullptr;
    }
}

bool StartDummyTcpServer(EventLoop& loop, int port, bool echo) {
    auto& server = TcpServer::Create(loop);
    if (echo) {
        server.OnConnection([&](TcpClient& client) {
            TcpForwarder::Create(client, client);
        });
    }
    return server.Listen(port);
}

bool StartDummyUdpServer(EventLoop& loop, int port, bool echo) {
    auto& server = UdpServer::Create(loop);
    server.OnRecv([&](const char* s, size_t len, const struct sockaddr*) {
        log_debug("DD: %.*s", (int)len, s);
    });
    return server.Listen(port);
}

bool Nexer::StartDummyServer(config::Dummy& config) {
    switch(config.type) {
    case config::Dummy::Type::Udp:
        return StartDummyUdpServer(loop_, config.port, config.echo);
    case config::Dummy::Type::Tcp:
        return StartDummyTcpServer(loop_, config.port, config.echo);
    default:
        assert(0);
    }
}

}