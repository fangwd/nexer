#ifndef NEXER_TIMER_H_
#define NEXER_TIMER_H_

#include "nexer/core/event_loop.h"
#include "nexer/core/handle.h"
#include "uv.h"
#include <memory>

namespace nexer {

class Timer : public Handle {
  private:
    uv_timer_t timer_;
    uint64_t interval_;
    uv_timeval64_t start_time_;
    std::function<void()> on_tick_;
    bool cleanup_hook_registered_ = false;

    inline uv_handle_t *handle() override { return (uv_handle_t *)&timer_; }

    static void handle_tick(uv_timer_t *);

    Timer(uv_loop_t *, uint64_t);

  public:
    static std::shared_ptr<Timer> create(EventLoop &, uint64_t interval);
    static std::shared_ptr<Timer> create(uv_loop_t *, uint64_t interval);

    inline void on_tick(std::function<void()> fn) { on_tick_ = fn; }

    void close();
    bool start(bool repeat = true);
    bool stop();
    uint64_t get_elapsed_time();
    static uint64_t now();
};

} // namespace nexer

#endif // NEXER_TIMER_H_
