#include "nexer/core/async_work.h"
#include "nexer/core/logger.h"

namespace nexer {

AsyncWork::AsyncWork(EventLoop &loop) : loop_(loop) {}

std::shared_ptr<AsyncWork> AsyncWork::Create(EventLoop &loop) {
    // using new because constructor is private
    return std::shared_ptr<AsyncWork>(new AsyncWork(loop));
}

void AsyncWork::Start(std::function<void()> work) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        log_error("async_work: Start called while work is still running");
        if (on_end_) {
            on_end_(UV_EBUSY);
        }
        return;
    }

    work_ = std::move(work);

    auto request = new uv_work_t();
    *request = {};
    request->loop = loop_;

    auto self_ptr = new std::shared_ptr<AsyncWork>(shared_from_this());
    request->data = self_ptr;

    int status = uv_queue_work(
        loop_, request,
        [](uv_work_t *req) {
            auto *s_ptr =
                reinterpret_cast<std::shared_ptr<AsyncWork> *>(req->data);
            (*s_ptr)->work_();
        },
        [](uv_work_t *req, int status) {
            auto *s_ptr =
                reinterpret_cast<std::shared_ptr<AsyncWork> *>(req->data);
            auto self = *s_ptr;
            self->busy_.store(false, std::memory_order_release);
            if (self->on_end_) {
                self->on_end_(status);
            }
            delete s_ptr;
            delete req;
        });

    if (status != 0) {
        log_error("uv_queue_work failed: %s", uv_strerror(status));
        delete self_ptr;
        busy_.store(false, std::memory_order_release);
        if (on_end_) {
            on_end_(status);
        }
        delete request;
    }
}

} // namespace nexer
