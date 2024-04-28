/* Copyright (c) Weidong Fang. All rights reserved. */

#include "event_loop.h"

#include "logger.h"

namespace nexer {

EventLoop::EventLoop() {
    if (int status = uv_loop_init(&loop_)) {
        log_fatal("uv_loop_init: %s", uv_strerror(status));
        exit(1);
    }
    uv_loop_set_data(&loop_, this);
}

EventLoop::~EventLoop() {
    Close();
}

static void close_walk_cb(uv_handle_t* handle, void* arg) {
    if (!uv_is_closing(handle)) {
        log_warn("closing handle %p", handle);
        uv_close(handle, NULL);
    }
}

void EventLoop::Close() {
    if (int status = uv_loop_close(&loop_)) {
        log_warn("uv_loop_close: %s", uv_strerror(status));
        uv_walk(&loop_, close_walk_cb, nullptr);
        if (int status = uv_loop_close(&loop_)) {
            log_error("uv_loop_close: %s", uv_strerror(status));
        }
    }
}

bool EventLoop::Run() {
    if (int status = uv_run(&loop_, UV_RUN_DEFAULT)) {
        log_error("uv_run: %s", uv_strerror(status));
        return false;
    }
    return true;
}

}  // namespace nexer