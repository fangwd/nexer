#include "nexer/runtime/tcp_proxy.h"

#include "nexer/core/logger.h"
#include "nexer/core/string_buffer.h"
#include "nexer/core/tcp_connector.h"
#include <algorithm>
#include <assert.h>
#include <sstream>

namespace nexer {

std::shared_ptr<TcpProxy> TcpProxy::create(EventLoop &loop,
                                           config::Upstream &upstream,
                                           ProcessManager *pm,
                                           size_t max_forwarder_buffer_bytes) {
    auto proxy = std::shared_ptr<TcpProxy>(
        new TcpProxy(loop, upstream, pm, max_forwarder_buffer_bytes));
    proxy->init();
    return proxy;
}

TcpProxy::TcpProxy(EventLoop &loop, config::Upstream &upstream,
                   ProcessManager *pm, size_t max_forwarder_buffer_bytes)
    : TcpServer(loop), upstream_(upstream), process_manager_(pm),
      max_forwarder_buffer_bytes_(max_forwarder_buffer_bytes) {
    std::stringstream ss;
    ss << upstream_.host << ':' << upstream_.port;
    name_ = ss.str();
}

void TcpProxy::init() {
    auto self_weak = std::weak_ptr<TcpProxy>(
        std::static_pointer_cast<TcpProxy>(shared_from_this()));

    TcpServer::OnConnection([self_weak](std::shared_ptr<TcpClient> incoming) {
        auto self = self_weak.lock();
        if (!self)
            return;

        auto forwarder =
            TcpForwarder::create(incoming, self->max_forwarder_buffer_bytes_);
        std::weak_ptr<TcpForwarder> weak_forwarder = forwarder;

        forwarder->on_close([self_weak, weak_forwarder] {
            auto self = self_weak.lock();
            if (self) {
                if (auto forwarder = weak_forwarder.lock()) {
                    self->remove(forwarder);
                }
            }
        });

        self->forwarders_.insert(forwarder);

        self->check_upstream_process([self_weak, forwarder,
                                      incoming](int error) {
            auto self = self_weak.lock();
            if (!self)
                return;

            if (error) {
                log_info("Upstream check failed (%d)", error);
                if (self->has(forwarder)) {
                    log_debug(
                        "Closing incoming connection due to upstream error");
                    incoming->close();
                }
                return;
            }

            log_debug("Connecting %s", self->name_.data());
            TcpConnectOptions opts{
                .host = self->upstream_.host,
                .port = self->upstream_.port,
                .deadline_ms =
                    static_cast<uint64_t>(self->upstream_.connect_timeout),
                .retry_interval_ms = 500,
            };

            auto connector = TcpConnector::create(
                self->loop(), std::move(opts),
                [self_weak, forwarder,
                 incoming](std::shared_ptr<TcpClient> outgoing) {
                    auto self = self_weak.lock();
                    if (!self) {
                        if (outgoing)
                            outgoing->close();
                        return;
                    }

                    log_debug("Connecting to %s completed (success: %s)",
                              self->name_.data(), outgoing ? "true" : "false");
                    if (!self->has(forwarder)) {
                        if (outgoing) {
                            log_info("Closing upstream connection to %s "
                                     "(incoming already closed)",
                                     self->name_.data());
                            outgoing->close();
                        }
                    } else if (outgoing) {
                        log_debug("Forwarder establised for %s",
                                  self->name_.data());
                        forwarder->set_outgoing(outgoing);
                    } else {
                        log_info("Closing incoming for %s (upstream connection "
                                 "failed)",
                                 self->name_.data());
                        incoming->close();
                    }
                });
            self->track_connector(connector);
        });
    });
}

void TcpProxy::check_upstream_process(std::function<void(int)> then) {
    if (!upstream_.app) {
        then(0);
    } else {
        process_manager_->Require(
            *upstream_.app,
            [then](std::shared_ptr<Process>, int error) { then(error); });
    }
}

bool TcpProxy::has(std::shared_ptr<TcpForwarder> forwarder) {
    auto it = forwarders_.find(forwarder);
    return it != forwarders_.end();
}

void TcpProxy::remove(std::shared_ptr<TcpForwarder> forwarder) {
    auto it = forwarders_.find(forwarder);
    if (it == forwarders_.end()) {
        if (!shutting_down_) {
            log_critical("forwarder %p not found", forwarder.get());
        }
        return;
    }
    forwarders_.erase(it);
}

void TcpProxy::cleanup_connectors() {
    connectors_.erase(
        std::remove_if(connectors_.begin(), connectors_.end(),
                       [](const auto &weak) { return weak.expired(); }),
        connectors_.end());
}

void TcpProxy::track_connector(const std::shared_ptr<TcpConnector> &connector) {
    cleanup_connectors();
    connectors_.push_back(connector);
}

void TcpProxy::shutdown() {
    shutting_down_ = true;

    std::vector<std::shared_ptr<TcpForwarder>> forwarders(forwarders_.begin(),
                                                          forwarders_.end());
    for (auto &forwarder : forwarders) {
        if (forwarder) {
            forwarder->shutdown();
        }
    }
    forwarders_.clear();

    for (auto &weak : connectors_) {
        if (auto connector = weak.lock()) {
            connector->cancel();
        }
    }
    connectors_.clear();
}

} // namespace nexer
