#include "nexer/core/timer.h"

#include "nexer/core/logger.h"
#include <assert.h>

namespace nexer {

Timer::Timer(uv_loop_t *loop, uint64_t interval) : interval_(interval) {
    if (int status = uv_timer_init(loop, &timer_)) {
        log_fatal("uv_timer_init: %s", uv_strerror(status));
    }

    timer_.data = this;
}

void Timer::handle_tick(uv_timer_t *handle) {
    if (uv_is_closing((uv_handle_t *)handle))
        return;

    auto timer = reinterpret_cast<Timer *>(handle->data);

    // Use shared_ptr to protect against destruction during callback
    auto self = std::static_pointer_cast<Timer>(timer->shared_from_this());

    if (self->on_tick_) {
        self->on_tick_();
    }
}

std::shared_ptr<Timer> Timer::create(EventLoop &loop, uint64_t interval) {
    auto self = std::shared_ptr<Timer>(new Timer(loop, interval));
    self->on_close_.set_owner(self);
    self->on_error_.set_owner(self);
    return self;
}

std::shared_ptr<Timer> Timer::create(uv_loop_t *loop, uint64_t interval) {
    auto self = std::shared_ptr<Timer>(new Timer(loop, interval));
    self->on_close_.set_owner(self);
    self->on_error_.set_owner(self);
    return self;
}

bool Timer::start(bool repeat) {
    // libuv timer repeats if the last argument is non-zero.
    // uv_timer_start(handle, cb, timeout, repeat)
    int status =
        uv_timer_start(&timer_, handle_tick, interval_, repeat ? interval_ : 0);
    if (status != 0) {
        log_error("uv_timer_start: %s", uv_strerror(status));
        return false;
    }
    if (uv_gettimeofday(&start_time_)) {
        log_error("uv_gettimeofday failed");
        return false;
    }
    return true;
}

void Timer::close() {
    if (!cleanup_hook_registered_) {
        cleanup_hook_registered_ = true;
        auto weak_self = std::weak_ptr<Timer>(
            std::static_pointer_cast<Timer>(shared_from_this()));
        on_close_.add([weak_self] {
            if (auto self = weak_self.lock()) {
                self->on_tick_ = {};
                self->cleanup_hook_registered_ = false;
            }
        });
    }
    Handle::close();
}

uint64_t Timer::get_elapsed_time() {
    uv_timeval64_t end;
    assert(uv_gettimeofday(&end) == 0);
    uint64_t start_ms =
        start_time_.tv_sec * 1000LL + start_time_.tv_usec / 1000;
    uint64_t end_ms = end.tv_sec * 1000LL + end.tv_usec / 1000;
    return end_ms - start_ms;
}

uint64_t Timer::now() {
    uv_timeval64_t now;
    assert(uv_gettimeofday(&now) == 0);
    return now.tv_sec * 1000LL + now.tv_usec / 1000;
}

bool Timer::stop() {
    if (int status = uv_timer_stop(&timer_)) {
        log_error("uv_timer_stop: %s", uv_strerror(status));
        return false;
    }
    return true;
}

} // namespace nexer
