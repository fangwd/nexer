/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_PROCESS_H_
#define NEXER_PROCESS_H_

#include "event_loop.h"
#include "config.h"
#include "timer.h"
#include "function_list.h"
#include <string>
#include <vector>
#include <functional>

namespace nexer {
// Represents a child process

class Process {
  private:
    uv_loop_t *loop_;
    uv_process_t request_;
    uv_process_options_t options_;
    uv_stdio_container_t stdio_[3];
    uv_pipe_t pipes_[3];

    std::vector<char *> args_;
    std::vector<char *> env_;

    Timer* timer_;
    void *data_;

    struct {
        bool running;
        int64_t exit_code;
        int term_signal;
    } status_;

    FunctionList<void, int, const char *, size_t> on_data_;
    FunctionList<void, int> on_error_;
    FunctionList<void, int64_t, int> on_exit_;

    friend void OnProcessExit(uv_process_t *, int64_t, int);
    friend void OnProcessHandleClose(uv_handle_t *);

    static void OnRead(int fd, uv_stream_t* pipe, ssize_t nread, const uv_buf_t* rdbuf);
    static void OnReadStdout(uv_stream_t* pipe, ssize_t nread, const uv_buf_t* rdbuf);
    static void OnReadStderr(uv_stream_t* pipe, ssize_t nread, const uv_buf_t* rdbuf);
    static void OnWriteAfter(uv_write_t* req, int status);
    static void OnPipeClose(uv_handle_t*);

    int InitStdio();
    void InitEnv();
    int Spawn();

  protected:
    Process(EventLoop& loop, const char *file);
    virtual ~Process();

  public:
    static Process& Create(EventLoop& loop, const char *file);
    static Process& Create(EventLoop& loop, const config::Command& cmd);

    void Init(const config::Command& cmd);

    void PushArg(const char *key, const char *value = nullptr);
    void SetEnv(const char *);
    void SetTimeout(uint64_t);

    bool Start();
    int Kill(int signal=SIGTERM);

    int Input(const char *, size_t);

    inline auto OnData(std::function<void(int, const char *, size_t)> fn) {
        on_data_.Add(fn);
    }

    inline auto OnError(std::function<void(int)> fn) {
        on_error_.Add(fn);
    }

    inline auto OnExit(std::function<void(int64_t, int)> fn) {
        return on_exit_.Add(fn);
    }

    bool IsRunning() const {
        return status_.running;
    }
    int64_t GetExitCode() const {
        return status_.exit_code;
    }
    int GetTermSignal() const {
        return status_.term_signal;
    }

    inline void SetData(void *data) {
        data_ = data;
    }

    inline void *data() {
        return data_;
    }
};

};  // namespace nexer

#endif // NEXER_PROCESS_H_
