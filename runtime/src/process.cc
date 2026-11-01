#include "nexer/runtime/process.h"
#include "nexer/core/string_buffer.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sstream>

namespace nexer {

std::shared_ptr<Process> Process::create(EventLoop &loop, const char *file) {
    auto process =
        std::shared_ptr<Process>(new Process(PassKey{0}, loop, file));
    process->on_start_.set_owner(process);
    process->on_exit_.set_owner(process);
    process->on_data_.set_owner(process);
    process->on_error_.set_owner(process);
    return process;
}

std::shared_ptr<Process> Process::create(EventLoop &loop,
                                         const config::Command &cmd) {
    auto process = create(loop, cmd.file.data());
    process->init(cmd);
    return process;
}

void Process::init(const config::Command &cmd) {
    for (auto &arg : cmd.args) {
        push_arg(arg.data());
    }

    for (auto &env : cmd.env) {
        set_env(env.data());
    }

    if (cmd.timeout > 0) {
        set_timeout(cmd.timeout);
    }

    set_cwd(cmd.cwd.empty() ? nullptr : cmd.cwd.c_str());
}

Process::Process(PassKey, EventLoop &loop, const char *file)
    : loop_(loop), status_{0}, data_(nullptr) {
    args_.push_back(strdup(file));
    memset(pipes_, 0, sizeof(pipes_));
    memset(&request_, 0, sizeof request_);
    request_.data = this;
    init_env();
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
    if (timer_ptr_) {
        timer_ptr_->close();
    }
}

void Process::handle_closed() {
    if (--closing_count_ <= 0) {
        close_guard_.reset();
    }
}

void OnProcessHandleClose(uv_handle_t *handle) {
    auto process = static_cast<Process *>(handle->data);
    if (process) {
        process->handle_closed();
    }
}

void OnProcessExit(uv_process_t *req, int64_t exit_code, int term_signal) {
    Process *process = (Process *)(req->data);

    // Ensure process stays alive during callback
    auto self = process->shared_from_this();
    process->close_guard_ = self;

    if (process->timer_ptr_) {
        process->timer_ptr_->stop();
        process->timer_ptr_->close();
        process->timer_ptr_.reset();
    }

    process->status_ = {false, exit_code, term_signal};

    process->closing_count_ = 0;
    for (int i = 0; i < 3; i++) {
        if (process->pipes_[i].data &&
            !uv_is_closing((uv_handle_t *)&process->pipes_[i])) {
            process->closing_count_++;
            uv_close((uv_handle_t *)&process->pipes_[i], OnProcessHandleClose);
        }
    }

    if (!process->suppress_exit_callback_) {
        process->on_exit_.invoke(exit_code, term_signal);
    }

    if (req->loop && !uv_is_closing((uv_handle_t *)req)) {
        process->closing_count_++;
        uv_close((uv_handle_t *)req, OnProcessHandleClose);
    }

    if (process->closing_count_ == 0) {
        process->close_guard_.reset();
    }
}

void Process::push_arg(const char *key, const char *value) {
    args_.push_back(strdup(key));
    if (value) {
        args_.push_back(strdup(value));
    }
}

// A configured value replaces an inherited entry with the same key.
void Process::set_env(const char *env) {
    const char *eq = strchr(env, '=');
    size_t key_len = eq ? static_cast<size_t>(eq - env) : strlen(env);
    for (auto it = env_.begin(); it != env_.end();) {
        if (*it && strncmp(*it, env, key_len) == 0 && (*it)[key_len] == '=') {
            free(*it);
            it = env_.erase(it);
        } else {
            ++it;
        }
    }
    env_.push_back(strdup(env));
}

void Process::set_cwd(const char *path) {
    if (path && *path) {
        cwd_ = path;
    } else {
        cwd_.clear();
    }
}

void Process::set_timeout(uint64_t ms) {
    if (timer_ptr_) {
        on_error_.invoke(-EINVAL);
        return;
    }
    if (ms > 0) {
        timer_ptr_ = Timer::create(loop_, ms);
        auto timer_ref = timer_ptr_.get();
        std::weak_ptr<Process> weak_self = shared_from_this();

        timer_ptr_->on_tick([weak_self, timer_ref] {
            auto self = weak_self.lock();
            if (self) {
                timer_ref->stop();
                timer_ref->close();
                self->timer_ptr_.reset();
                if (self->is_running()) {
                    self->kill(SIGTERM);
                } else {
                    OnProcessExit(&self->request_, -2, 0);
                }
            }
        });
        timer_ptr_->start();
    }
}

int Process::init_stdio() {
    int err;
    for (size_t i = 0; i < 3; i++) {
        if ((err = uv_pipe_init(loop_, &pipes_[i], 0))) {
            return err;
        }
        auto flags =
            UV_CREATE_PIPE | (i == 0 ? UV_READABLE_PIPE : UV_WRITABLE_PIPE);
        stdio_[i].flags = uv_stdio_flags(flags);
        stdio_[i].data.stream = (uv_stream_t *)&pipes_[i];
        pipes_[i].data = this;
    }
    options_.stdio = stdio_;
    options_.stdio_count = 3;
    return 0;
}

void Process::init_env() {
    uv_env_item_t *items;
    int count;

    if (uv_os_environ(&items, &count) == 0) {
        StringBuffer sb;
        for (int i = 0; i < count; i++) {
            sb.Clear();
            sb.Printf("%s=%s", items[i].name, items[i].value);
            set_env(sb.data());
        }
        uv_os_free_environ(items, count);
    }
}

static void OnAlloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

void Process::on_read(int fd, uv_stream_t *pipe, ssize_t nread,
                      const uv_buf_t *rdbuf) {
    auto process = (Process *)pipe->data;
    auto self = process->shared_from_this();

    if (nread <= 0 && nread != UV_EOF) {
        process->on_error_.invoke(nread);
    } else if (nread > 0) {
        process->on_data_.invoke(fd, rdbuf->base, nread);
    }
    free(rdbuf->base);
}

void Process::on_read_stdout(uv_stream_t *pipe, ssize_t nread,
                             const uv_buf_t *rdbuf) {
    on_read(1, pipe, nread, rdbuf);
}
void Process::on_read_stderr(uv_stream_t *pipe, ssize_t nread,
                             const uv_buf_t *rdbuf) {
    on_read(2, pipe, nread, rdbuf);
}

int Process::spawn() {
    int err;

    args_.push_back(nullptr);
    env_.push_back(nullptr);

    memset(&options_, 0, sizeof options_);

    options_.file = args_[0];
    options_.args = args_.data();

    if (env_.size() > 1) {
        options_.env = env_.data();
    }

    if ((err = init_stdio())) {
        return err;
    }

    options_.exit_cb = OnProcessExit;
    options_.cwd = cwd_.empty() ? nullptr : cwd_.c_str();
    if (own_process_group_) {
        options_.flags |= UV_PROCESS_DETACHED;
    }

    status_.running = true;

    if ((err = uv_spawn(loop_, &request_, &options_))) {
        status_.running = false;
        return err;
    }

    if ((err = uv_read_start((uv_stream_t *)&pipes_[1], OnAlloc,
                             on_read_stdout))) {
        return err;
    }

    if ((err = uv_read_start((uv_stream_t *)&pipes_[2], OnAlloc,
                             on_read_stderr))) {
        return err;
    }

    return 0;
}

bool Process::start() {
    close_guard_ = shared_from_this();
    int err = spawn();

    if (err == 0) {
        return true;
    }

    on_error_.invoke(err);

    if (status_.running) {
        kill();
    } else {
        suppress_exit_callback_ = true;
        OnProcessExit(&request_, -1, -1);
        suppress_exit_callback_ = false;
    }

    return false;
}

int Process::kill(int signal) { return uv_process_kill(&request_, signal); }

int Process::kill_group(int signal) {
    if (!own_process_group_ || request_.pid <= 0) {
        return kill(signal);
    }
    if (::kill(-request_.pid, signal) != 0) {
        return -errno;
    }
    return 0;
}

bool Process::group_alive() const {
    if (!own_process_group_ || request_.pid <= 0) {
        return status_.running;
    }
    if (::kill(-request_.pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

struct ProcessWriteRequest {
    uv_write_t req;
    uv_buf_t buf;
    std::shared_ptr<Process> proc;
};

void Process::on_write_after(uv_write_t *req, int status) {
    auto wr = reinterpret_cast<ProcessWriteRequest *>(req);
    if (status != 0) {
        wr->proc->on_error_.invoke(status);
    }
    free(wr->buf.base);
    delete wr;
}

int Process::input(const char *s, size_t len) {
    auto *wr = new ProcessWriteRequest();
    wr->proc = shared_from_this();
    wr->buf.base = (char *)malloc(len);
    if (!wr->buf.base) {
        delete wr;
        on_error_.invoke(-ENOMEM);
        return -ENOMEM;
    }
    wr->buf.len = (unsigned int)len;
    memcpy(wr->buf.base, s, len);
    int status = uv_write((uv_write_t *)wr, (uv_stream_t *)&pipes_[0], &wr->buf,
                          1, on_write_after);
    if (status != 0) {
        free(wr->buf.base);
        delete wr;
    }
    return status;
}

} // namespace nexer
