#ifndef NEXER_ASYNC_WORK_H_
#define NEXER_ASYNC_WORK_H_

#include <atomic>
#include <functional>
#include <memory>

#include "nexer/core/event_loop.h"
#include "nexer/core/non_copyable.h"

namespace nexer {

class AsyncWork : public std::enable_shared_from_this<AsyncWork>, NonCopyable {
  private:
    uv_loop_t *loop_;
    std::function<void()> work_;
    std::function<void(int)> on_end_;
    std::atomic<bool> busy_{false};

    AsyncWork(EventLoop &loop);

  public:
    static std::shared_ptr<AsyncWork> Create(EventLoop &loop);

    inline void OnEnd(std::function<void(int)> cb) { on_end_ = cb; }
    void Start(std::function<void()> work);
};

} // namespace nexer

#endif // NEXER_ASYNC_WORK_H_
