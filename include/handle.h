#ifndef NEXER_HANDLE_H_
#define NEXER_HANDLE_H_

#include <functional>

#include "event_loop.h"
#include "function_list.h"
#include "non_copyable.h"

namespace nexer {

class Handle : NonCopyable {
  private:
    void *data_ = nullptr;
    std::function<void()> data_freer_;

  protected:
    static void OnClose(uv_handle_t* handle);
    FunctionList<void> on_close_;

    void OnError(const char *context, int error_code);
    FunctionList<void, int, const char*> on_error_;

    virtual ~Handle() {}

  public:
    virtual uv_handle_t* handle() = 0;

    void Close();
    bool IsClosing();

    inline auto OnError(std::function<void(int, const char*)> fn) {
        return on_error_.Add(fn);
    }

    inline auto OnClose(std::function<void()> fn) {
        return on_close_.Add(fn);
    }

    inline EventLoop& loop() {
        return  *reinterpret_cast<EventLoop*>(handle()->loop->data);
    }

    inline void *data() {
        return data_;
    }

    inline void SetData(void *data, std::function<void()> freer = {}) {
        data_ = data;
        data_freer_ = freer;
    }
};

}  // namespace nexer

#endif  // NEXER_HANDLE_H_
