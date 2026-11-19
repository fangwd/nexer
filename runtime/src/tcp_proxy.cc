#include "nexer/runtime/tcp_proxy.h"

#include "nexer/core/logger.h"
#include "nexer/core/string_buffer.h"
#include "nexer/core/tcp_connector.h"
#include "nexer/core/timer.h"
#include "nexer/runtime/proxy_protocol.h"
#include <algorithm>
#include <assert.h>

namespace nexer {

std::shared_ptr<TcpProxy> TcpProxy::create(EventLoop &loop,
                                           config::Proxy &config,
                                           ProcessManager *pm,
                                           size_t max_forwarder_buffer_bytes) {
    auto proxy = std::shared_ptr<TcpProxy>(
        new TcpProxy(loop, config, pm, max_forwarder_buffer_bytes));
    proxy->init();
    return proxy;
}

TcpProxy::TcpProxy(EventLoop &loop, config::Proxy &config, ProcessManager *pm,
                   size_t max_forwarder_buffer_bytes)
    : TcpServer(loop), config_(config), process_manager_(pm),
      max_forwarder_buffer_bytes_(max_forwarder_buffer_bytes) {}

static constexpr size_t kMaxHeadBytes = 64 * 1024;

static TcpProxy::Connected attach(std::shared_ptr<TcpForwarder> forwarder) {
    return [forwarder](std::shared_ptr<TcpClient> outgoing, std::string) {
        if (outgoing) {
            forwarder->set_outgoing(outgoing);
        } else {
            forwarder->shutdown();
        }
    };
}

// Sends an HTTP error and closes once it is written.
static void reply(const std::shared_ptr<TcpClient> &client,
                  const char *status) {
    if (client->is_closing()) {
        return;
    }
    std::string response = std::string("HTTP/1.1 ") + status +
                           "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    std::weak_ptr<TcpClient> weak = client;
    client->on_send([weak] {
        if (auto client = weak.lock(); client && !client->is_closing()) {
            client->close();
        }
    });
    client->write(response.data(), response.size());
}

std::weak_ptr<TcpProxy> TcpProxy::weak() {
    return std::static_pointer_cast<TcpProxy>(shared_from_this());
}

void TcpProxy::init() {
    auto self_weak = weak();

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
        if (self->config_.protocol == config::Proxy::Protocol::Tcp) {
            self->route(forwarder, 0, false, nullptr, attach(forwarder));
        } else {
            self->sniff(forwarder, incoming);
        }
    });
}

void TcpProxy::sniff(std::shared_ptr<TcpForwarder> forwarder,
                     std::shared_ptr<TcpClient> incoming) {
    // Owned by the listeners below until finish() unsubscribes them.
    struct Sniff {
        std::function<void()> stop_data;
        std::function<void()> stop_close;
        std::shared_ptr<Timer> timer;

        void finish() {
            stop_data();
            stop_close();
            if (timer) {
                timer->close();
                timer.reset();
            }
        }
    };
    auto sniff = std::make_shared<Sniff>();
    auto self_weak = weak();
    std::weak_ptr<TcpForwarder> weak_forwarder = forwarder;
    std::weak_ptr<TcpClient> weak_incoming = incoming;
    auto parser = std::make_shared<proxy::RequestParser>();

    // A client that never completes its request would otherwise hold its
    // connection, and delay shutdown draining, forever.
    sniff->timer = Timer::create(loop(), head_timeout_ms_);
    std::weak_ptr<Sniff> weak_sniff = sniff;
    sniff->timer->on_tick([weak_sniff, weak_forwarder] {
        if (auto sniff = weak_sniff.lock()) {
            sniff->finish();
        }
        if (auto forwarder = weak_forwarder.lock()) {
            log_info("Closing proxy client that sent no request");
            forwarder->shutdown();
        }
    });
    sniff->stop_close = forwarder->on_close([sniff] { sniff->finish(); });
    // Runs after the forwarder's own handler has buffered the data.
    sniff->stop_data =
        incoming->on_data([self_weak, weak_forwarder, weak_incoming, parser,
                           sniff](const char *, size_t) {
            auto self = self_weak.lock();
            auto forwarder = weak_forwarder.lock();
            auto incoming = weak_incoming.lock();
            if (!self || !forwarder || !incoming || incoming->is_closing() ||
                !self->has(forwarder)) {
                return;
            }
            auto request = parser->parse(forwarder->pending(), kMaxHeadBytes);
            if (request.kind == proxy::Request::Kind::Incomplete) {
                return;
            }
            sniff->finish();
            self->dispatch(forwarder, incoming, std::move(request), parser);
        });
    sniff->timer->start(false);
}

void TcpProxy::dispatch(std::shared_ptr<TcpForwarder> forwarder,
                        std::shared_ptr<TcpClient> incoming,
                        proxy::Request request,
                        std::shared_ptr<proxy::RequestParser> parser) {
    using Kind = proxy::Request::Kind;
    switch (request.kind) {
    case Kind::Socks:
        route(forwarder, 0, false, nullptr, attach(forwarder));
        return;
    case Kind::Connect:
        forwarder->pending().erase(0, request.head_size);
        break;
    case Kind::Forward: {
        // One request per origin connection: client bytes after it are a
        // pipelined or reused request that may be meant for another host, so
        // they are dropped. The origin closes after its response (the
        // request says Connection: close), which ends the client side too.
        using Result = proxy::RequestParser::Result;
        auto &pending = forwarder->pending();
        size_t used = 0;
        auto result = parser->feed(pending.data() + request.head_size,
                                   pending.size() - request.head_size, used);
        if (result == Result::Error) {
            log_info("Rejecting malformed proxy request");
            reply(incoming, "400 Bad Request");
            return;
        }
        if (result == Result::Complete) {
            pending.resize(request.head_size + used);
        }
        pending.replace(0, request.head_size, request.head);
        std::weak_ptr<TcpClient> weak_incoming = incoming;
        forwarder->set_incoming_filter(
            [parser, weak_incoming](const char *s, size_t len) -> size_t {
                size_t used = 0;
                if (parser->feed(s, len, used) == Result::Error) {
                    if (auto incoming = weak_incoming.lock();
                        incoming && !incoming->is_closing()) {
                        incoming->close();
                    }
                }
                return used;
            });
        break;
    }
    default:
        log_info("Rejecting malformed proxy request");
        reply(incoming, "400 Bad Request");
        return;
    }

    auto self_weak = weak();
    bool tunnel = request.kind == Kind::Connect;
    auto host = request.host;
    int port = request.port;
    auto negotiate = [self_weak, forwarder, host,
                      port](std::shared_ptr<TcpClient> outgoing,
                            const config::Upstream &upstream, Negotiated done) {
        auto self = self_weak.lock();
        if (!self) {
            done(Negotiation::Stalled, "");
            return;
        }
        self->socks5(forwarder, outgoing, host, port,
                     static_cast<uint64_t>(upstream.connect_timeout),
                     std::move(done));
    };
    route(forwarder, 0, false, negotiate,
          [incoming, forwarder, tunnel](std::shared_ptr<TcpClient> outgoing,
                                        std::string early) {
              if (!outgoing) {
                  reply(incoming, "502 Bad Gateway");
                  return;
              }
              if (tunnel) {
                  static const std::string established =
                      "HTTP/1.1 200 Connection established\r\n\r\n";
                  incoming->write(established.data(), established.size());
              }
              if (!early.empty()) {
                  incoming->write(early.data(), early.size());
              }
              forwarder->set_outgoing(outgoing);
          });
}

void TcpProxy::socks5(std::shared_ptr<TcpForwarder> forwarder,
                      std::shared_ptr<TcpClient> outgoing,
                      const std::string &host, int port, uint64_t timeout_ms,
                      Negotiated done) {
    // Owned by the listeners below until finish() unsubscribes them.
    struct Handshake {
        std::string buffer;
        bool greeted = false;
        std::vector<std::function<void()>> unsubscribe;
        std::shared_ptr<Timer> timer;
        Negotiated done;

        void finish(Negotiation result) {
            if (!done) {
                return;
            }
            auto then = std::move(done);
            done = nullptr;
            if (timer) {
                timer->close();
                timer.reset();
            }
            for (auto &fn : unsubscribe) {
                fn();
            }
            unsubscribe.clear();
            then(result,
                 result == Negotiation::Ok ? std::move(buffer) : std::string());
        }
    };
    auto hs = std::make_shared<Handshake>();
    hs->done = std::move(done);
    std::weak_ptr<TcpClient> weak_outgoing = outgoing;
    auto close_outgoing = [weak_outgoing] {
        if (auto outgoing = weak_outgoing.lock();
            outgoing && !outgoing->is_closing()) {
            outgoing->close();
        }
    };

    hs->unsubscribe.push_back(outgoing->on_data(
        [hs, weak_outgoing, host, port](const char *s, size_t len) {
            hs->buffer.append(s, len);
            if (!hs->greeted) {
                int n = proxy::Socks5GreetingReply(hs->buffer);
                if (n == 0) {
                    return;
                }
                if (n < 0) {
                    hs->finish(Negotiation::Refused);
                    return;
                }
                hs->buffer.erase(0, n);
                hs->greeted = true;
                if (auto outgoing = weak_outgoing.lock()) {
                    auto request = proxy::Socks5Connect(host, port);
                    outgoing->write(request.data(), request.size());
                }
                return;
            }
            int n = proxy::Socks5ConnectReply(hs->buffer);
            if (n == 0) {
                return;
            }
            if (n > 0) {
                hs->buffer.erase(0, n);
            }
            hs->finish(n > 0 ? Negotiation::Ok : Negotiation::Refused);
        }));
    // ssh -D refuses a destination by closing, with no SOCKS reply.
    hs->unsubscribe.push_back(
        outgoing->on_close([hs] { hs->finish(Negotiation::Refused); }));
    hs->unsubscribe.push_back(outgoing->on_error(
        [close_outgoing](int, const char *) { close_outgoing(); }));
    // The client left mid-handshake.
    hs->unsubscribe.push_back(forwarder->on_close(close_outgoing));

    // An upstream can accept TCP and then stall, e.g. ssh whose link died.
    if (timeout_ms > 0) {
        hs->timer = Timer::create(outgoing->loop(), timeout_ms);
        std::weak_ptr<Handshake> weak_hs = hs;
        hs->timer->on_tick([weak_hs, host] {
            if (auto hs = weak_hs.lock()) {
                log_info("SOCKS negotiation for %s timed out", host.c_str());
                hs->finish(Negotiation::Stalled);
            }
        });
        hs->timer->start(false);
    }

    auto greeting = proxy::Socks5Greeting();
    outgoing->write(greeting.data(), greeting.size());
}

bool TcpProxy::ready(const config::Upstream &upstream) {
    return !upstream.app ||
           process_manager_->snapshot(*upstream.app).observed ==
               ObservedState::Ready;
}

void TcpProxy::route(std::shared_ptr<TcpForwarder> forwarder, size_t index,
                     bool waiting, Negotiate negotiate, Connected done,
                     std::shared_ptr<std::vector<Failed>> failed) {
    if (!has(forwarder)) {
        return;
    }
    auto &upstreams = config_.upstreams;
    if (!failed) {
        failed = std::make_shared<std::vector<Failed>>(upstreams.size());
    }
    if (index == upstreams.size()) {
        if (!waiting) {
            route(forwarder, 0, true, std::move(negotiate), std::move(done),
                  std::move(failed));
        } else {
            log_info("No upstream available");
            done(nullptr, "");
        }
        return;
    }

    auto *upstream = &upstreams[index];
    auto self_weak = weak();
    auto next = [self_weak, forwarder, index, waiting, negotiate, done,
                 failed] {
        if (auto self = self_weak.lock()) {
            self->route(forwarder, index + 1, waiting, negotiate, done, failed);
        }
    };

    if (waiting ? (*failed)[index].settled : !ready(*upstream)) {
        next();
        return;
    }

    auto connect = [self_weak, forwarder, upstream, index, waiting, next,
                    negotiate, done, failed] {
        auto self = self_weak.lock();
        if (!self || !self->has(forwarder))
            return;

        log_debug("Connecting %s:%d", upstream->host.c_str(), upstream->port);
        TcpConnectOptions opts{
            .host = upstream->host,
            .port = upstream->port,
            .deadline_ms = static_cast<uint64_t>(upstream->connect_timeout),
            .retry_interval_ms = 500,
            .max_attempts = waiting ? 0 : 1,
        };

        auto on_connect = [self_weak, forwarder, upstream, index, next,
                           negotiate, done,
                           failed](std::shared_ptr<TcpClient> outgoing) {
            auto self = self_weak.lock();
            if (!self || !self->has(forwarder)) {
                if (outgoing) {
                    outgoing->close();
                }
                return;
            }
            if (!outgoing) {
                log_info("Upstream %s:%d unavailable", upstream->host.c_str(),
                         upstream->port);
                next();
                return;
            }
            log_debug("Connected to upstream %s:%d", upstream->host.c_str(),
                      upstream->port);
            if (!negotiate) {
                done(outgoing, "");
                return;
            }
            uint64_t launches =
                upstream->app
                    ? self->process_manager_->snapshot(*upstream->app).launches
                    : 0;
            negotiate(
                outgoing, *upstream,
                [self_weak, forwarder, outgoing, upstream, index, next, done,
                 failed, launches](Negotiation result, std::string early) {
                    auto self = self_weak.lock();
                    if (!self || !self->has(forwarder) ||
                        result != Negotiation::Ok) {
                        if (!outgoing->is_closing()) {
                            outgoing->close();
                        }
                        if (self && self->has(forwarder)) {
                            auto &failure = (*failed)[index];
                            if (upstream->app &&
                                (result == Negotiation::Stalled ||
                                 !self->ready(*upstream))) {
                                failure.launches = launches;
                            } else {
                                failure.settled = true;
                            }
                            log_info("Upstream %s:%d negotiation failed",
                                     upstream->host.c_str(), upstream->port);
                            next();
                        }
                        return;
                    }
                    done(outgoing, std::move(early));
                });
        };
        auto connector =
            TcpConnector::create(self->loop(), std::move(opts), on_connect);
        self->track_connector(connector);
    };

    if (!waiting || !upstream->app) {
        connect();
        return;
    }
    // Requiring a ready app re-runs its checker, which relaunches one that
    // hung.
    auto launches = (*failed)[index].launches;
    process_manager_->Require(
        *upstream->app, [pm = process_manager_, upstream, launches, connect,
                         next](std::shared_ptr<Process>, int error) {
            if (error) {
                log_info("Upstream %s:%d app not ready (%s)",
                         upstream->host.c_str(), upstream->port,
                         error_string(error).c_str());
                next();
                return;
            }
            if (launches &&
                pm->snapshot(*upstream->app).launches == *launches) {
                log_info("Upstream %s:%d not tried again: app not relaunched",
                         upstream->host.c_str(), upstream->port);
                next();
                return;
            }
            connect();
        });
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
