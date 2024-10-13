#include "runner.h"
#include "process.h"
#include "logger.h"

namespace nexer {
namespace test {

char exename[sizeof exename];

void TestProcess();
void TestProcessManager();
void TestHttpServer();
void TestTimer();
void TestTcpClient();
void TestTcpProxy();
void TestTcpServer();
void TestUdpServer();
void TestAsyncWork();
void TestConfig();
void TestMemoryPool();

Task tasks[] = {
    {"async-work", TestAsyncWork},
    {"config", TestConfig},
    {"http-server", TestHttpServer},
    {"memory-pool", TestMemoryPool},
    {"process", TestProcess},
    {"process-manager", TestProcessManager},
    {"tcp-client", TestTcpClient},
    {"tcp-proxy", TestTcpProxy},
    {"tcp-server", TestTcpServer},
    {"udp-server", TestUdpServer},
    {"timer", TestTimer},
    {nullptr, nullptr},
};

struct ProcessInfo {
    uv_loop_t *loop;
    int64_t status;
    int signal;
    ProcessInfo(uv_loop_t *loop) : loop(loop), status(0), signal(0) {}
};

static void on_exit(uv_process_t *process, int64_t exit_status, int term_signal) {
    ProcessInfo *pi = (ProcessInfo *)process->data;
    pi->status = exit_status;
    pi->signal = term_signal;
    uv_loop_close(pi->loop);
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

void Run(const char *name) {
    RunTask(tasks, name, "test");
}

static int RunInternal(const char *name) {
    uv_loop_t *loop = uv_default_loop();
    uv_process_t process{0};
    uv_process_options_t options{0};

    char *args[3];
    args[0] = exename;
    args[1] = (char *)name;
    args[2] = nullptr;

    options.exit_cb = on_exit;
    options.file = exename;
    options.args = args;

    ProcessInfo pi(loop);

    process.data = &pi;

    int r;
    if ((r = uv_spawn(loop, &process, &options))) {
        fprintf(stderr, "%s\n", uv_strerror(r));
        return 1;
    }

    if ((r = uv_run(loop, UV_RUN_DEFAULT)) != 0) {
        fprintf(stderr, "%s\n", uv_strerror(r));
        return 1;
    }

    return pi.status;
}

void Run() {
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
    fprintf(stdout, "Total %d, ok %d, failed %d\n", total, total - failed, failed);
    fflush(stdout);
}


}  // namespace test
}  // namespace nexer
