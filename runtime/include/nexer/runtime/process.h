#ifndef NEXER_PROCESS_H_
#define NEXER_PROCESS_H_

#include "nexer/core/event_loop.h"
#include "nexer/core/function_list.h"
#include "nexer/core/non_copyable.h"
#include "nexer/core/timer.h"
#include "nexer/runtime/config.h"
#include <memory>
#include <vector>

namespace nexer {

class Process : public std::enable_shared_from_this<Process>, NonCopyable {
  private:
    struct PassKey {
        explicit PassKey(int) {}
    };
    EventLoop &loop_;
    uv_process_t request_;
    uv_process_options_t options_;
    uv_pipe_t pipes_[3];
    uv_stdio_container_t stdio_[3];

    struct {
        bool running;
        int64_t exit_code;
        int term_signal;
    } status_;

    void *data_;

    std::string name_;
    std::vector<char *> args_;
    std::vector<char *> env_;
    std::string cwd_;
    bool suppress_exit_callback_ = false;
    bool own_process_group_ = false;

    FunctionList<void> on_start_;
    FunctionList<void, int64_t, int> on_exit_;
    FunctionList<void, int, const char *, size_t> on_data_;
    FunctionList<void, int> on_error_;

    std::shared_ptr<Timer> timer_ptr_;
    std::shared_ptr<Process> close_guard_;
    int closing_count_ = 0;

    void handle_closed();
    int init_stdio();
    void init_env();
    int spawn();

    static void on_read(int fd, uv_stream_t *, ssize_t nread, const uv_buf_t *);
    static void on_read_stdout(uv_stream_t *, ssize_t nread, const uv_buf_t *);
    static void on_read_stderr(uv_stream_t *, ssize_t nread, const uv_buf_t *);
    static void on_write_after(uv_write_t *, int status);

    friend void OnProcessExit(uv_process_t *, int64_t, int);
    friend void OnProcessHandleClose(uv_handle_t *);

  public:
    Process(PassKey, EventLoop &, const char *file);
    ~Process();

    static std::shared_ptr<Process> create(EventLoop &, const char *file);
    static std::shared_ptr<Process> create(EventLoop &,
                                           const config::Command &);

    void init(const config::Command &);

    inline void set_name(const std::string &name) { name_ = name; }

    inline const std::string &name() const { return name_; }

    inline auto on_start(std::function<void()> fn) { return on_start_.add(fn); }
    inline auto on_exit(std::function<void(int64_t, int)> fn) {
        return on_exit_.add(fn);
    }
    inline auto on_data(std::function<void(int, const char *, size_t)> fn) {
        return on_data_.add(fn);
    }
    inline auto on_error(std::function<void(int)> fn) {
        return on_error_.add(fn);
    }

    void push_arg(const char *value) { push_arg(value, nullptr); }
    void push_arg(const char *key, const char *value);
    void set_env(const char *env);
    void set_timeout(uint64_t ms);
    void set_cwd(const char *path);

    // Spawn the child as the leader of a new session/process group so that
    // kill_group() reaches its descendants.
    inline void set_own_process_group(bool enabled) {
        own_process_group_ = enabled;
    }
    inline bool own_process_group() const { return own_process_group_; }

    bool start();
    int kill(int signal = SIGTERM);
    // Signals the whole process group when started with its own group,
    // otherwise only the child. Returns a libuv error code.
    int kill_group(int signal = SIGTERM);
    // True while any member of the child's process group still exists.
    bool group_alive() const;
    int input(const char *s, size_t len);

    inline bool is_running() { return status_.running; }

    inline int pid() const { return request_.pid; }

    inline int64_t exit_code() const { return status_.exit_code; }

    inline int term_signal() const { return status_.term_signal; }
};

} // namespace nexer

#endif // NEXER_PROCESS_H_
