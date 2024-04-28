/* Copyright (c) Weidong Fang. All rights reserved. */

#include "timer.h"

#include "logger.h"
#include <assert.h>

namespace nexer {

Timer::Timer(uv_loop_t* loop, uint64_t interval) : interval_(interval) {
    if (int status = uv_timer_init(loop, &timer_)) {
        log_fatal("uv_timer_init: %s", uv_strerror(status));
    }

    timer_.data = this;
}

void Timer::OnTick(uv_timer_t* handle) {
    auto timer = reinterpret_cast<Timer*>(handle->data);
    if (timer->on_tick_ && timer->GetElapsedTime() >= timer->interval_) {
        timer->on_tick_();
    }
}

Timer& Timer::Create(EventLoop& loop, uint64_t interval) {
    auto timer = new Timer(loop, interval);
    return *timer;
}

Timer& Timer::Create(uv_loop_t* loop, uint64_t interval) {
    auto timer = new Timer(loop, interval);
    return *timer;
}

bool Timer::Start() {
    int status = uv_timer_start(&timer_, OnTick, 0, interval_);
    if (status != 0) {
        log_error("uv_timer_start: %s", uv_strerror(status));
        return false;
    }
    if (uv_gettimeofday(&start_time_)) {
        log_error("uv_timer_start: %s", uv_strerror(status));
        return false;
    }
    return true;

}

uint64_t Timer::GetElapsedTime() {
    uv_timeval64_t end;
    assert(uv_gettimeofday(&end) == 0);
    uint64_t start_ms = start_time_.tv_sec * 1000LL + start_time_.tv_usec / 1000;
    uint64_t end_ms = end.tv_sec * 1000LL + end.tv_usec / 1000;
    return end_ms - start_ms;
}

uint64_t Timer::Now() {
    uv_timeval64_t now;
    assert(uv_gettimeofday(&now) == 0);
    return now.tv_sec * 1000LL + now.tv_usec / 1000;
}

bool Timer::Stop() {
    if (int status = uv_timer_stop(&timer_)) {
        log_fatal("uv_timer_stop: %s", uv_strerror(status));
        return false;
    }
    return true;
}

}  // namespace nexer