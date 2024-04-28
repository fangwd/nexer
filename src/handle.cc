/* Copyright (c) Weidong Fang. All rights reserved. */

#include "handle.h"
#include "logger.h"

namespace nexer {

void Handle::OnClose(uv_handle_t *handle) {
    auto self = static_cast<Handle *>(handle->data);
    if (self->on_close_) {
        self->on_close_();
    }
    delete self;
}

void Handle::OnError(const char *ctx, int err) {
    const char *errmsg =  uv_strerror(err);
    if (on_error_) {
        on_error_(err, errmsg);
    } else {
        log_error("%s: %s (%d)", ctx, errmsg, err);
    }
}

void Handle::Close() {
    if (!uv_is_closing(handle())) {
        uv_close(handle(), OnClose);
    }
}

bool Handle::IsClosing() {
    return uv_is_closing(handle());
}

}  // namespace nexer