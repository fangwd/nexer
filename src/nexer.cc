/* Copyright (c) Weidong Fang. All rights reserved. */

#include "nexer.h"
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

}