#ifndef NEXER_HANDLE_H_
#define NEXER_HANDLE_H_

#include "nexer/core/event_loop.h"
#include "nexer/core/function_list.h"
#include "nexer/core/non_copyable.h"
#include <functional>
#include <memory>

namespace nexer {

class Handle : public std::enable_shared_from_this<Handle>, NonCopyable {
  private:
    void *data_ = nullptr;
    std::function<void()> data_freer_;
    std::shared_ptr<Handle> close_guard_;

  protected:
    static void on_close(uv_handle_t *handle);
    FunctionList<void> on_close_;

    void on_error(const char *context, int error_code);
    FunctionList<void, int, const char *> on_error_;

    virtual ~Handle();

  public:
    virtual uv_handle_t *handle() = 0;

    void close();
    bool is_closing();

    inline auto on_error(std::function<void(int, const char *)> fn) {
        return on_error_.add(fn);
    }

    inline auto on_close(std::function<void()> fn) { return on_close_.add(fn); }

    inline EventLoop &loop() {
        return *reinterpret_cast<EventLoop *>(handle()->loop->data);
    }

    inline void *data() { return data_; }

    inline void set_data(void *data, std::function<void()> freer = {}) {
        data_ = data;
        data_freer_ = freer;
    }
};

} // namespace nexer

#endif // NEXER_HANDLE_H_
