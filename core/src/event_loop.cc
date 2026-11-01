#include "nexer/core/event_loop.h"

#include "nexer/core/handle.h"
#include "nexer/core/logger.h"
#ifndef _WIN32
#include <signal.h>
#endif

namespace nexer {

EventLoop::EventLoop() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (int status = uv_loop_init(&loop_)) {
        log_error("uv_loop_init: %s", uv_strerror(status));
        return;
    }
    uv_loop_set_data(&loop_, this);

    if (uv_async_init(&loop_, &async_, on_post) == 0) {
        async_.data = nullptr; // Avoid close_walk_cb casting to Handle*
        uv_unref(reinterpret_cast<uv_handle_t *>(&async_));
        async_initialized_ = true;
    }
}

EventLoop::~EventLoop() { close(); }

static void close_walk_cb(uv_handle_t *handle, void *) {
    if (uv_is_closing(handle)) {
        return;
    }

    uv_close(handle, nullptr);
}

void EventLoop::close() {
    if (uv_loop_get_data(&loop_) != this) {
        return;
    }

    if (int status = uv_loop_close(&loop_)) {
        uv_walk(&loop_, close_walk_cb, nullptr);
        // Run the loop once to allow close callbacks to fire.
        uv_run(&loop_, UV_RUN_DEFAULT);
        if (uv_loop_close(&loop_)) {
            return;
        }
    }
    uv_loop_set_data(&loop_, nullptr);
}

bool EventLoop::run() {
    if (uv_loop_get_data(&loop_) != this) {
        log_error("uv_run: event loop not initialized");
        return false;
    }
    uv_run(&loop_, UV_RUN_DEFAULT);
    return true;
}

void EventLoop::stop() {
    if (uv_loop_get_data(&loop_) != this) {
        return;
    }
    uv_stop(&loop_);
}

void EventLoop::on_post(uv_async_t *handle) {
    auto *self = static_cast<EventLoop *>(uv_loop_get_data(handle->loop));
    if (self) {
        self->drain_post_queue();
    }
}

void EventLoop::drain_post_queue() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(post_mutex_);
        work.swap(post_queue_);
    }
    for (auto &fn : work) {
        fn();
    }
}

void EventLoop::post(std::function<void()> fn) {
    if (!async_initialized_)
        return;
    {
        std::lock_guard<std::mutex> lock(post_mutex_);
        post_queue_.push_back(std::move(fn));
    }
    uv_async_send(&async_);
}

} // namespace nexer
