#include "async_work.h"

namespace nexer {
AsyncWork::AsyncWork(EventLoop& loop) {
    request_.loop = loop;
    request_.data = this;
}

void AsyncWork::Work(uv_work_t *work) {
    auto data = reinterpret_cast<AsyncWork*>(work->data);
    data->work_();
}

void AsyncWork::AfterWork(uv_work_t *work, int status) {
    auto data = reinterpret_cast<AsyncWork*>(work->data);
    if (data->on_end_) {
        data->on_end_(status);
    }
}

void AsyncWork::Start(std::function<void()> work) {
    work_ = work;
    uv_queue_work(request_.loop, &request_, Work, AfterWork);
}

}  // namespace nexer