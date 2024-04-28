/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_HANDLE_H_
#define NEXER_HANDLE_H_

#include <functional>

#include "event_loop.h"
#include "non_copyable.h"

namespace nexer {

class Handle : NonCopyable {
  private:
    void *data_ = nullptr;

  protected:
    static void OnClose(uv_handle_t* handle);
    std::function<void()> on_close_;

    void OnError(const char *context, int error_code);
    std::function<void(int, const char*)> on_error_;

    virtual uv_handle_t* handle() = 0;

    virtual ~Handle() {}

  public:
    void Close();
    bool IsClosing();

    inline void OnError(std::function<void(int, const char*)> fn) {
        on_error_ = fn;
    }

    inline void OnClose(std::function<void()> fn) {
        on_close_ = fn;
    }

    inline EventLoop& loop() {
        return  *reinterpret_cast<EventLoop*>(handle()->loop->data);
    }

    inline void *data() {
        return data_;
    }

    inline void SetData(void *data) {
        data_ = data;
    }
};

}  // namespace nexer

#endif  // NEXER_HANDLE_H_
