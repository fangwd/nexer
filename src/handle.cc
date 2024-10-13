#include "handle.h"
#include "logger.h"

namespace nexer {

void Handle::OnClose(uv_handle_t *handle) {
    auto self = static_cast<Handle *>(handle->data);
    self->on_close_.Invoke();
    if (self->data_ && self->data_freer_) {
        self->data_freer_();
    }
    delete self;
}

void Handle::OnError(const char *ctx, int err) {
    const char *errmsg =  uv_strerror(err);
    on_error_.Invoke(err, errmsg);
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