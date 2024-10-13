#include "tcp_proxy.h"

#include "logger.h"
#include "string_buffer.h"
#include "tcp_proxy.h"
#include <assert.h>
#include <sstream>

namespace nexer {

TcpProxy &TcpProxy::Create(EventLoop &loop,config::Upstream &upstream, ProcessManager *pm) {
    auto proxy = new TcpProxy(loop, upstream, pm);
    return *proxy;
}

TcpProxy::TcpProxy(EventLoop& loop, config::Upstream& upstream, ProcessManager *pm)
    :TcpServer(loop), upstream_(upstream), process_manager_(pm) {
    std::stringstream ss;
    ss << upstream_.host << ':' << upstream_.port;
    name_ = ss.str();
    Init();
}

void TcpProxy::Init() {
    TcpServer::OnConnection([&](TcpClient &incoming) {
        auto& forwarder = TcpForwarder::Create(incoming);
        forwarder.OnClose([&] {
            Remove(forwarder);
        });
        forwarders_.insert(&forwarder);
        CheckUpstreamProcess([&](int error) {
            if (error) {
                log_info("Upstream check failed (%d)", error);
                if (Has(forwarder)) {
                    log_debug("Closing incoming connection due to upstream error");
                    incoming.Close();
                }
                return;
            }

            log_debug("Connecting %s", name_.data());
            TcpClient::Connect(loop(), upstream_.host.data(), upstream_.port, upstream_.connect_timeout, [&](TcpClient *outgoing) {
                log_debug("Connecting to %s completed (success: %s)", name_.data(), outgoing ? "true" : "false");
                if (!Has(forwarder)) {
                    if (outgoing) {
                        log_info("Closing upstream connection to %s (incoming already closed)", name_.data());
                        outgoing->Close();
                    } else {
                        log_debug("No outgoing or forwarder");
                    }
                } else if (outgoing) {
                    log_debug("Forwarder establised for %s", name_.data());
                    forwarder.SetOutgoing(*outgoing);
                } else {
                    log_info("Closing incoming for %s (upstream connection failed)", name_.data());
                    incoming.Close();
                }
            });
        });
    });
}

void TcpProxy::CheckUpstreamProcess(std::function<void(int)> then) {
    if (!upstream_.app) {
        then(0);
    } else {
        process_manager_->Require(*upstream_.app, [&, then](Process*, int error) {
            then(error);
        });
    }
}

bool TcpProxy::Has(TcpForwarder &forwarder) {
    auto it = forwarders_.find(&forwarder);
    return it != forwarders_.end();
}

void TcpProxy::Remove(TcpForwarder &forwarder) {
    auto it = forwarders_.find(&forwarder);
    if (it == forwarders_.end()) {
        log_critical("forwarder %p not found");
        assert(0);
    }
    forwarders_.erase(it);
}

}  // namespace nexer
