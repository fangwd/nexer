#include "nexer/core/tcp_connector.h"

#include <cassert>

#include "nexer/core/logger.h"
#include "nexer/core/tcp_client.h"
#include "nexer/core/timer.h"

namespace nexer {
namespace {

uint64_t NormalizeInterval(uint64_t interval_ms) {
    return interval_ms == 0 ? 500 : interval_ms;
}

} // namespace

std::shared_ptr<TcpConnector> TcpConnector::create(EventLoop &loop,
                                                   TcpConnectOptions options,
                                                   ResultCallback cb,
                                                   TryCallback on_try) {
    auto connector = std::shared_ptr<TcpConnector>(new TcpConnector(
        loop, std::move(options), std::move(cb), std::move(on_try)));
    connector->start();
    return connector;
}

TcpConnector::TcpConnector(EventLoop &loop, TcpConnectOptions options,
                           ResultCallback cb, TryCallback on_try)
    : loop_(loop), options_(std::move(options)), callback_(std::move(cb)),
      on_try_(std::move(on_try)) {}

TcpConnector::~TcpConnector() {
#ifndef NDEBUG
    assert(self_guard_ == nullptr &&
           "TcpConnector destroyed with active self_guard_");
#endif
}

void TcpConnector::start() {
    started_ms_ = Timer::now();
    self_guard_ = shared_from_this();

    timer_ =
        Timer::create(loop_, NormalizeInterval(options_.retry_interval_ms));
    auto weak_self = std::weak_ptr<TcpConnector>(self_guard_);

    timer_->on_tick([weak_self] {
        if (auto self = weak_self.lock()) {
            self->tick();
        }
    });

    timer_->on_close([weak_self] {
        if (auto self = weak_self.lock()) {
            self->retry_timer_active_ = false;
            self->timer_.reset();
        }
    });

    retry_timer_active_ = timer_->start();
    if (!retry_timer_active_) {
        log_error("tcp_connector: failed to start retry timer");
        timer_->close();
    }

    try_once();
}

void TcpConnector::cancel() {
    if (finished_ || cancel_pending_) {
        return;
    }
    cancel_pending_ = true;
    if (timer_) {
        if (retry_timer_active_) {
            timer_->stop();
        }
        timer_->close();
        retry_timer_active_ = false;
        timer_.reset();
    }
    if (!attempt_inflight_) {
        finish(nullptr);
    }
}

void TcpConnector::tick() {
    if (finished_) {
        return;
    }

    if (cancel_pending_) {
        return;
    }

    if (deadline_exceeded()) {
        log_debug("tcp_connector: deadline exceeded for %s:%d",
                  options_.host.c_str(), options_.port);
        finish(nullptr);
        return;
    }

    if (!attempt_inflight_) {
        try_once();
    }
}

void TcpConnector::try_once() {
    if (finished_ || attempt_inflight_) {
        return;
    }

    auto client = TcpClient::create(loop_);
    if (!client) {
        log_error("tcp_connector: failed to create tcp client");
        finish(nullptr);
        return;
    }
    auto weak_self = std::weak_ptr<TcpConnector>(self_guard_);

    log_debug("tcp_connector: attempting %s:%d", options_.host.c_str(),
              options_.port);

    client->on_connect([weak_self] {
        if (auto self = weak_self.lock()) {
            self->handle_connected();
        }
    });

    client->on_error([weak_self](int err, const char *msg) {
        if (auto self = weak_self.lock()) {
            self->handle_error(err, msg);
        }
    });

    client->on_close([weak_self] {
        if (auto self = weak_self.lock()) {
            self->handle_closed();
        }
    });

    live_client_ = client;
    attempt_inflight_ = true;

    if (on_try_) {
        on_try_(*client);
    }

    client->connect(options_.host.c_str(), options_.port);
}

void TcpConnector::handle_connected() {
    if (finished_) {
        return;
    }

    attempt_inflight_ = false;

    auto client = live_client_;
    if (cancel_pending_) {
        client.reset();
    }
    // Defer finish() to the next tick so libuv can complete the connect
    // callback safely.
    auto weak_self = std::weak_ptr<TcpConnector>(self_guard_);
    finish_timer_ = Timer::create(loop_, 0);
    auto weak_timer = std::weak_ptr<Timer>(finish_timer_);

    finish_timer_->on_tick([weak_self, weak_timer, client]() mutable {
        auto timer = weak_timer.lock();
        if (auto self = weak_self.lock()) {
            if (timer && self->finish_timer_ == timer) {
                self->finish_timer_->close();
                self->finish_timer_.reset();
            }
            self->finish(std::move(client));
        }
    });
    if (!finish_timer_->start(false)) {
        log_error("tcp_connector: failed to start finish timer");
        finish_timer_->close();
        finish_timer_.reset();
        finish(std::move(client));
        return;
    }
}

void TcpConnector::handle_error(int err, const char *msg) {
    if (finished_) {
        return;
    }

    log_debug("tcp_connector: connect error %s:%d (%s)", options_.host.c_str(),
              options_.port, msg ? msg : "unknown");
    if (live_client_ && !live_client_->is_closing()) {
        live_client_->close();
    }
}

void TcpConnector::handle_closed() {
    if (finished_) {
        live_client_.reset();
        attempt_inflight_ = false;
        return;
    }

    if (cancel_pending_) {
        live_client_.reset();
        attempt_inflight_ = false;
        finish(nullptr);
        return;
    }

    live_client_.reset();
    attempt_inflight_ = false;

    if (!retry_timer_active_ && !cancel_pending_) {
        finish(nullptr);
    }
}

void TcpConnector::finish(std::shared_ptr<TcpClient> result) {
    if (finished_) {
        return;
    }

    finished_ = true;

    if (timer_) {
        if (retry_timer_active_) {
            timer_->stop();
        }
        retry_timer_active_ = false;
        timer_->close();
    }
    if (finish_timer_) {
        finish_timer_->stop();
        finish_timer_->close();
        finish_timer_.reset();
    }

    std::shared_ptr<TcpClient> client_result = std::move(result);

    if (!client_result && live_client_) {
        if (!live_client_->is_closing()) {
            live_client_->close();
        }
    }

    attempt_inflight_ = false;

    live_client_.reset();

    if (callback_) {
        callback_(client_result);
    }

    if (client_result && client_result.use_count() == 1 &&
        !client_result->is_closing()) {
        client_result->close();
    }

    callback_ = nullptr;
    on_try_ = nullptr;
    self_guard_.reset();
}

bool TcpConnector::deadline_exceeded() const {
    if (options_.deadline_ms == 0) {
        return false;
    }
    return Timer::now() - started_ms_ >= options_.deadline_ms;
}

} // namespace nexer
