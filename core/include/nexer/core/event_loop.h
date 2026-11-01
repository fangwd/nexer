#ifndef NEXER_EVENT_LOOP_H_
#define NEXER_EVENT_LOOP_H_

#include "uv.h"
#include <functional>
#include <mutex>
#include <vector>

namespace nexer {

class EventLoop {
  private:
    uv_loop_t loop_;
    uv_async_t async_{};
    std::mutex post_mutex_;
    std::vector<std::function<void()>> post_queue_;
    bool async_initialized_ = false;

    void close();
    void drain_post_queue();
    static void on_post(uv_async_t *handle);

  public:
    EventLoop();
    ~EventLoop();

    inline operator uv_loop_t *() {
        return uv_loop_get_data(&loop_) == this ? &loop_ : nullptr;
    }

    bool run();
    void stop();

    // Thread-safe: post a callback to run on the event loop thread.
    void post(std::function<void()> fn);

    inline bool is_initialised() const {
        return uv_loop_get_data(const_cast<uv_loop_t *>(&loop_)) == this;
    }
};

} // namespace nexer

#include "nexer/core/then.h"

#endif // NEXER_EVENT_LOOP_H_
