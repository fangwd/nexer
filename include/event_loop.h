/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_EVENT_LOOP_H_
#define NEXER_EVENT_LOOP_H_

#include "uv.h"
#include <functional>

namespace nexer {

class EventLoop {
  private:
    uv_loop_t loop_;

    void Close();

  public:
    EventLoop();
    ~EventLoop();

    inline operator uv_loop_t* () {
        return &loop_;
    }

    bool Run();
};

}  // namespace nexer

#include "then.h"

#endif // NEXER_EVENT_LOOP_H_