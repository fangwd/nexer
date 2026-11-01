#include "runner.h"
#include "nexer/core/logger.h"
#include "nexer/runtime/process.h"
#include <string.h>

namespace nexer {
namespace test {

char exename[sizeof exename];

void TestProcess();
void TestProcessManager();
void TestAutoStart();
void TestHttpServer();
void TestTimer();
void TestDummyServers();
void TestStartupRollback();
void TestTcpClient();
void TestTcpConnector();
void TestTcpProxy();
void TestTcpServer();
void TestUdpServer();
void TestAsyncWork();
void TestConfig();
void TestMemoryPool();
void TestStringBuffer();
void TestFunctionList();
void TestUrl();
void TestAdminServer();
void TestRuntime();

Task tasks[] = {
    {"async-work", TestAsyncWork},
    {"config", TestConfig},
    {"function-list", TestFunctionList},
    {"admin-server", TestAdminServer},
    {"http-server", TestHttpServer},
    {"memory-pool", TestMemoryPool},
    {"process", TestProcess},
    {"process-manager", TestProcessManager},
    {"auto-start", TestAutoStart},
    {"dummy-servers", TestDummyServers},
    {"startup-rollback", TestStartupRollback},
    {"runtime", TestRuntime},
    {"string-buffer", TestStringBuffer},
    {"tcp-client", TestTcpClient},
    {"tcp-connector", TestTcpConnector},
    {"tcp-proxy", TestTcpProxy},
    {"tcp-server", TestTcpServer},
    {"udp-server", TestUdpServer},
    {"timer", TestTimer},
    {"url", TestUrl},
    {nullptr, nullptr},
};

struct ProcessInfo {
    uv_loop_t *loop;
    int64_t status;
    int signal;
    bool has_stderr;
    ProcessInfo(uv_loop_t *loop)
        : loop(loop), status(0), signal(0), has_stderr(false) {}
};

static void on_stderr_read(uv_stream_t *stream, ssize_t nread,
                           const uv_buf_t *buf) {
    if (nread > 0) {
        ProcessInfo *pi = (ProcessInfo *)stream->data;
        pi->has_stderr = true;
        fwrite(buf->base, 1, nread, stdout);
        fflush(stdout);
    }
    if (buf->base) {
        free(buf->base);
    }
}

static void on_stderr_alloc(uv_handle_t *handle, size_t suggested_size,
                            uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

static void on_exit(uv_process_t *process, int64_t exit_status,
                    int term_signal) {
    ProcessInfo *pi = (ProcessInfo *)process->data;
    pi->status = exit_status;
    pi->signal = term_signal;
    // Don't close the loop here - it will be closed in RunInternal after
    // cleanup
}

static void RunTask(Task *tasks, const char *name, const char *type) {
    for (auto *task = &tasks[0]; task->name; task++) {
        if (!strcmp(task->name, name)) {
            task->run();
            return;
        }
    }
    log_fatal("Unknown %s '%s'", type, name);
    assert(0);
}

void Run(const char *name) { RunTask(tasks, name, "test"); }

static int RunInternal(const char *name) {
    // Create a new loop for each test to avoid conflicts
    uv_loop_t *loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    if (uv_loop_init(loop) != 0) {
        fprintf(stderr, "Failed to initialize loop\n");
        free(loop);
        return 1;
    }

    uv_process_t process{0};
    uv_process_options_t options{0};
    uv_pipe_t stderr_pipe{0};

    char *args[3];
    args[0] = exename;
    args[1] = (char *)name;
    args[2] = nullptr;

    ProcessInfo pi(loop);

    // Initialize stderr pipe
    uv_pipe_init(loop, &stderr_pipe, 0);
    stderr_pipe.data = &pi;

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_IGNORE;
    stdio[2].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[2].data.stream = (uv_stream_t *)&stderr_pipe;

    options.exit_cb = on_exit;
    options.file = exename;
    options.args = args;
    options.stdio_count = 3;
    options.stdio = stdio;

    process.data = &pi;

    int r;
    if ((r = uv_spawn(loop, &process, &options))) {
        fprintf(stderr, "%s\n", uv_strerror(r));
        uv_close((uv_handle_t *)&stderr_pipe, nullptr);
        uv_run(loop, UV_RUN_DEFAULT); // Process any pending callbacks
        uv_loop_close(loop);
        free(loop);
        return 1;
    }

    // Start reading from stderr pipe
    uv_read_start((uv_stream_t *)&stderr_pipe, on_stderr_alloc, on_stderr_read);

    if ((r = uv_run(loop, UV_RUN_DEFAULT)) != 0) {
        fprintf(stderr, "%s\n", uv_strerror(r));
        uv_close((uv_handle_t *)&stderr_pipe, nullptr);
        uv_run(loop, UV_RUN_DEFAULT); // Process any pending callbacks
        uv_loop_close(loop);
        free(loop);
        return 1;
    }

    uv_close((uv_handle_t *)&stderr_pipe, nullptr);
    uv_run(loop, UV_RUN_DEFAULT); // Process close callbacks

    // Clean up the loop
    r = uv_loop_close(loop);
    if (r != 0) {
        // Loop still has active handles, walk and close them
        uv_walk(
            loop,
            [](uv_handle_t *handle, void *) {
                if (!uv_is_closing(handle)) {
                    uv_close(handle, nullptr);
                }
            },
            nullptr);
        uv_run(loop, UV_RUN_DEFAULT);
        uv_loop_close(loop);
    }
    free(loop);

    // Return non-zero if:
    // 1. Exit status is non-zero, OR
    // 2. Process was killed by a signal, OR
    // 3. Stderr had any content
    if (pi.signal != 0) {
        // Process was killed by signal, return special code
        return 128 + pi.signal;
    }
    if (pi.has_stderr && pi.status == 0) {
        // Had stderr output but claimed success - treat as failure
        return 1;
    }
    return pi.status;
}

int Run() {
    int total = 0;
    int failed = 0;
    for (auto *task = &tasks[0]; task->name; task++) {
        total++;
        fprintf(stdout, "Running %-16s ...", task->name);
        int status = RunInternal(task->name);
        if (status == 0) {
            fprintf(stdout, " ok\n");
        } else {
            fprintf(stdout, " failed (%d)\n", status);
            failed++;
        }
    }
    fprintf(stdout, "Total %d, ok %d, failed %d\n", total, total - failed,
            failed);
    fflush(stdout);
    return failed;
}

} // namespace test
} // namespace nexer
