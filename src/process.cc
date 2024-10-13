#include "process.h"
#include "string_buffer.h"
#include <assert.h>
#include <sstream>

namespace nexer {

Process &Process::Create(EventLoop &loop, const char *file) {
    auto process = new Process(loop, file);
    return *process;
}

Process &Process::Create(EventLoop &loop, const config::Command &cmd) {
    auto &process = Create(loop, cmd.file.data());
    process.Init(cmd);
    return process;
}

void Process::Init(const config::Command& cmd) {
    for (auto &arg : cmd.args) {
        PushArg(arg.data());
    }

    for (auto &env : cmd.env) {
        SetEnv(env.data());
    }

    if (cmd.timeout > 0) {
        SetTimeout(cmd.timeout);
    }
}

Process::Process(EventLoop &loop, const char *file) : loop_(loop), status_{0}, timer_(nullptr), data_(nullptr) {
    args_.push_back(strdup(file));
    memset(pipes_, 0, sizeof(pipes_));
    memset(&request_, 0, sizeof request_);
    request_.data = this;
    InitEnv();
}

Process::~Process() {
    for (auto arg : args_) {
        if (arg) {
            free(arg);
        }
    }
    for (auto env : env_) {
        if (env) {
            free(env);
        }
    }
    if (timer_) {
        timer_->Close();
    }
}

void OnProcessHandleClose(uv_handle_t *handle) {
    Process *process = (Process *)(handle->data);
    process->on_exit_.Invoke(process->status_.exit_code, process->status_.term_signal);
    delete process;
}

void OnProcessExit(uv_process_t *req, int64_t exit_code, int term_signal) {
    Process *process = (Process *)(req->data);
    process->status_ = {false, exit_code, term_signal};
    for (int i = 0; i < 3; i++) {
        if (process->pipes_[i].data) {
            uv_close((uv_handle_t*)&process->pipes_[i], nullptr);
        }
    }
    if (req->loop) {
        uv_close((uv_handle_t *)req, OnProcessHandleClose);
    } else {
        // Start() wasn't called but timer set
        OnProcessHandleClose((uv_handle_t *)req);
    }
}

void Process::PushArg(const char *key, const char *value) {
    args_.push_back(strdup(key));
    if (value) {
        args_.push_back(strdup(value));
    }
}

void Process::SetEnv(const char *env) {
    env_.push_back(strdup(env));
}

void Process::SetTimeout(uint64_t ms) {
    assert(timer_ == nullptr);
    if (ms > 0) {
        auto &timer = Timer::Create(loop_, ms);
        timer.OnTick([this] {
            timer_->Stop();
            if (IsRunning()) {
                Kill(SIGTERM);
            } else {
                OnProcessExit(&request_, -2, 0);
            }
        });
        timer_ = &timer;
        timer.Start();
    }
}

int Process::InitStdio() {
    int err;
    for (size_t i = 0; i < 3; i++) {
        if ((err = uv_pipe_init(loop_, &pipes_[i], 0))) {
            return err;
        }
        auto flags = UV_CREATE_PIPE | (i == 0 ? UV_READABLE_PIPE : UV_WRITABLE_PIPE);
        stdio_[i].flags = uv_stdio_flags(flags);
        stdio_[i].data.stream = (uv_stream_t *)&pipes_[i];
        pipes_[i].data = this;
    }
    options_.stdio = stdio_;
    options_.stdio_count = 3;
    return 0;
}

void Process::InitEnv() {
    uv_env_item_t *items;
    int count;

    assert(uv_os_environ(&items, &count) == 0);

    StringBuffer sb;
    for (int i = 0; i < count; i++) {
        sb.Clear();
        sb.Printf("%s=%s", items[i].name, items[i].value);
        SetEnv(sb.data());
    }

    uv_os_free_environ(items, count);
}

static void OnAlloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

void Process::OnRead(int fd, uv_stream_t *pipe, ssize_t nread, const uv_buf_t *rdbuf) {
    auto process = (Process *)pipe->data;
    if (nread <= 0 && nread != UV_EOF) {
        process->on_error_.Invoke(nread);
    } else if (nread > 0) {
        process->on_data_.Invoke(fd, rdbuf->base, nread);
    }
    free(rdbuf->base);
}

void Process::OnReadStdout(uv_stream_t *pipe, ssize_t nread, const uv_buf_t *rdbuf) {
    OnRead(1, pipe, nread, rdbuf);
}
void Process::OnReadStderr(uv_stream_t *pipe, ssize_t nread, const uv_buf_t *rdbuf) {
    OnRead(2, pipe, nread, rdbuf);
}

int Process::Spawn() {
    int err;

    args_.push_back(nullptr);
    env_.push_back(nullptr);

    memset(&options_, 0, sizeof options_);

    options_.file = args_[0];
    options_.args = args_.data();

    if (env_.size() > 1) {
        options_.env = env_.data();
    }

    if ((err = InitStdio())) {
        return err;
    }

    options_.exit_cb = OnProcessExit;

    status_.running = true;

    if ((err = uv_spawn(loop_, &request_, &options_))) {
        status_.running = false;
        return err;
    }

    if ((err = uv_read_start((uv_stream_t *)&pipes_[1], OnAlloc, OnReadStdout))) {
        return err;
    }

    if ((err = uv_read_start((uv_stream_t *)&pipes_[2], OnAlloc, OnReadStderr))) {
        return err;
    }

    return 0;
}

bool Process::Start() {
    int err = Spawn();

    if (err == 0) {
        return true;
    }

    on_error_.Invoke(err);

    if (status_.running) {
        Kill();
    } else {
        OnProcessExit(&request_, -1, -1);
    }

    return false;
}

int Process::Kill(int signal) {
    return uv_process_kill(&request_, signal);
}

struct WriteRequest {
    uv_write_t req;
    uv_buf_t buf;
    Process *proc;
};

void Process::OnWriteAfter(uv_write_t *req, int status) {
    auto wr = (WriteRequest *)req;
    if (status != 0) {
        wr->proc->on_error_.Invoke(status);
    }
    free(wr->buf.base);
    free(wr);
}

int Process::Input(const char *s, size_t len) {
    WriteRequest *wr = (WriteRequest *)malloc(sizeof *wr);
    wr->buf.base = (char *)malloc(len);
    wr->buf.len = len;
    memcpy(wr->buf.base, s, len);
    return uv_write((uv_write_t *)wr, (uv_stream_t *)&pipes_[0], &wr->buf, 1, OnWriteAfter);
}

}  // namespace nexer
