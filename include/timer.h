#ifndef NEXER_TIMER_H_
#define NEXER_TIMER_H_

#include "event_loop.h"
#include "handle.h"

namespace nexer {

class Timer :public Handle {
  private:
    uv_timer_t timer_;
    uint64_t interval_;
    uv_timeval64_t start_time_;

    inline uv_handle_t *handle() override {
        return (uv_handle_t*) &timer_;
    }

    static void OnTick(uv_timer_t* timer);
    std::function<void()> on_tick_;

    Timer(uv_loop_t*, uint64_t);

  public:
    static Timer& Create(EventLoop&, uint64_t);
    static Timer& Create(uv_loop_t*, uint64_t);

    inline void OnTick(std::function<void()> fn) {
        on_tick_ = fn;
    }

    inline uint64_t interval() {
        return interval_;
    }

    bool Start();
    bool Stop();

    // Elapsed time in milliseconds since start
    uint64_t GetElapsedTime();

    static uint64_t Now();
};

}  // namespace nexer

#endif  // NEXER_TIMER_H_
