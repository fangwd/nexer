#ifndef NEXER_ASYNC_WORK_H_
#define NEXER_ASYNC_WORK_H_

#include <functional>

#include "event_loop.h"
#include "non_copyable.h"

namespace nexer {

class AsyncWork : NonCopyable {
  private:
    uv_work_t request_;
    std::function<void()> work_;
    std::function<void(int)> on_end_;

    static void Work(uv_work_t *work);
    static void AfterWork(uv_work_t *work, int status);

  public:
    AsyncWork(EventLoop &loop);
    inline void OnEnd(std::function<void(int)> cb) {
        on_end_ = cb;
    }
    void Start(std::function<void()> work);
};

}  // namespace nexer

#endif  // NEXER_ASYNC_WORK_H_
