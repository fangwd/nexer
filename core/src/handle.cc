#include "nexer/core/handle.h"
#include "nexer/core/logger.h"

namespace nexer {

Handle::~Handle() {
    if (data_ && data_freer_) {
        data_freer_();
    }
}

void Handle::on_close(uv_handle_t *handle) {
    auto self = static_cast<Handle *>(handle->data);
    auto guard = std::move(self->close_guard_);
    self->on_close_.invoke();
    self->on_close_.clear();
    self->on_error_.clear();
}

void Handle::on_error(const char *ctx, int err) {
    const char *errmsg = uv_strerror(err);
    on_error_.invoke(err, errmsg);
}

void Handle::close() {
    if (!uv_is_closing(handle())) {
        close_guard_ = shared_from_this();
        on_close_.set_owner(close_guard_);
        on_error_.set_owner(close_guard_);
        uv_close(handle(), on_close);
    }
}

bool Handle::is_closing() { return uv_is_closing(handle()); }

} // namespace nexer
