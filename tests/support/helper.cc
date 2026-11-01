#include <arpa/inet.h>
#include <assert.h>
#include <cstring>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "helper.h"
#include "nexer/core/logger.h"
#include "runner.h"

namespace nexer {
namespace test {

static int RunHello(int, char **) {
    std::cout << "hello";
    std::cerr << "world";
    return 0;
}

static int RunStandardInput(int, char **) {
    std::cout << "Message: ";
    std::string input;
    std::getline(std::cin, input);
    std::cout << "Your message is '" << input << "'\n";
    std::cerr << "no error\n";
    return 0;
}

static int RunSetEnv(int, char **) {
    setbuf(stdout, nullptr);
    const char *env1 = getenv("ENV1");
    const char *env2 = getenv("ENV2");
    const char *env3 = getenv("ENV3");
    const char *env4 = getenv("ENV4");
    std::cout << "ENV1=" << (env1 ? env1 : "null");
    std::cout << "ENV2=" << (env2 ? env2 : "null");
    std::cout << "ENV3=" << (env3 ? env3 : "null");
    std::cout << "ENV4=" << (env4 ? env4 : "null");
    return 0;
}

static int RunPrintCwd(int, char **) {
    char path[PATH_MAX];
    if (!getcwd(path, sizeof(path))) {
        return 1;
    }
    std::cout << path;
    return 0;
}

// Each helper process advertises its identity via NAME; tests must provide an
// env var like NAME="a" that matches the `process` column inserted by the
// active scenario. The row defines the fake output, delay, and exit code we
// want the process to emit so higher level tests can assert on behaviour.
static int RunSleep(int argc, char **argv) {
    const char *name = getenv("NAME");
    db::Database db;
    ExecInfo info{
        .sleep_time = 0, .message = "", .exit_code = DEFAULT_EXIT_CODE};
    db.OpenProcess(name ? name : "", info);
    if (info.sleep_time > 0) {
        uv_sleep(info.sleep_time);
    }
    if (info.message.size() > 0) {
        std::cout << info.message << std::endl;
    }
    db.CloseProcess(name ? name : "");
    return info.exit_code;
}

// Prints 20 short lines as separate unbuffered writes so output-file
// rotation can be exercised deterministically.
static int RunLines(int, char **) {
    setbuf(stdout, nullptr);
    for (int i = 1; i <= 20; i++) {
        printf("line-%02d\n", i);
        uv_sleep(5);
    }
    return 0;
}

// Forks a grandchild and reports both pids so tests can verify process-group
// cleanup. Flags: exit-early (leader exits at once, leaving the child),
// ignore-term (leader ignores SIGTERM), child-ignore-term.
static int RunTree(int argc, char **argv) {
    bool exit_early = false;
    bool ignore_term = false;
    bool child_ignore_term = false;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "exit-early")) {
            exit_early = true;
        } else if (!strcmp(argv[i], "ignore-term")) {
            ignore_term = true;
        } else if (!strcmp(argv[i], "child-ignore-term")) {
            child_ignore_term = true;
        }
    }
    setbuf(stdout, nullptr);
    // Dispositions are inherited across fork, so the child's choice is made
    // before it exists and the leader restores its own afterwards.
    signal(SIGTERM, child_ignore_term ? SIG_IGN : SIG_DFL);
    pid_t child = fork();
    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        for (;;) {
            pause();
        }
    }
    signal(SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
    printf("leader=%d child=%d\n", (int)getpid(), (int)child);
    if (exit_early) {
        return 0;
    }
    for (;;) {
        pause();
    }
}

int PickFreePort() {
    uv_loop_t loop;
    if (uv_loop_init(&loop) != 0) {
        return 0;
    }

    uv_tcp_t tcp;
    if (uv_tcp_init(&loop, &tcp) != 0) {
        uv_loop_close(&loop);
        return 0;
    }

    sockaddr_in addr;
    if (uv_ip4_addr("127.0.0.1", 0, &addr) != 0) {
        uv_close(reinterpret_cast<uv_handle_t *>(&tcp), nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        return 0;
    }

    if (uv_tcp_bind(&tcp, reinterpret_cast<const sockaddr *>(&addr), 0) != 0) {
        uv_close(reinterpret_cast<uv_handle_t *>(&tcp), nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        return 0;
    }

    int len = sizeof(addr);
    if (uv_tcp_getsockname(&tcp, reinterpret_cast<sockaddr *>(&addr), &len) !=
        0) {
        uv_close(reinterpret_cast<uv_handle_t *>(&tcp), nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        return 0;
    }

    uv_close(reinterpret_cast<uv_handle_t *>(&tcp), nullptr);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);

    int port = ntohs(addr.sin_port);
    if (port > 0) {
        return port;
    }

    for (int p = 20000; p < 40000; ++p) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return 0;
        }

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr2;
        memset(&addr2, 0, sizeof(addr2));
        addr2.sin_family = AF_INET;
        addr2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr2.sin_port = htons(p);

        if (bind(fd, reinterpret_cast<sockaddr *>(&addr2), sizeof(addr2)) ==
            0) {
            close(fd);
            return p;
        }
        close(fd);
    }

    return 0;
}

// The echo server below is mostly taken from:
// https://github.com/libuv/libuv/blob/v1.x/test/echo-server.c
typedef struct {
    uv_write_t req;
    uv_buf_t buf;
} write_req_t;

static uv_loop_t *loop;

static int server_closed;
static uv_tcp_t tcpServer;
static uv_udp_t udpServer;
static uv_pipe_t pipeServer;
static uv_handle_t *server;
static uv_udp_send_t *send_freelist;

static void after_write(uv_write_t *req, int status);
static void after_read(uv_stream_t *, ssize_t nread, const uv_buf_t *buf);
static void on_close(uv_handle_t *peer);
static void on_server_close(uv_handle_t *handle);
static void on_connection(uv_stream_t *, int status);

static void after_write(uv_write_t *req, int status) {
    write_req_t *wr;

    /* Free the read/write buffer and the request */
    wr = (write_req_t *)req;
    free(wr->buf.base);
    free(wr);

    if (status == 0)
        return;

    fprintf(stderr, "uv_write error: %s - %s\n", uv_err_name(status),
            uv_strerror(status));
}

static void after_shutdown(uv_shutdown_t *req, int status) {
    assert(status == 0);
    uv_close((uv_handle_t *)req->handle, on_close);
    free(req);
}

static void on_shutdown(uv_shutdown_t *req, int status) {
    assert(status == 0);
    free(req);
}

static void after_read(uv_stream_t *handle, ssize_t nread,
                       const uv_buf_t *buf) {
    int i;
    write_req_t *wr;
    uv_shutdown_t *sreq;
    int shutdown = 0;

    if (nread < 0) {
        /* Error or EOF */
        assert(nread == UV_EOF);

        free(buf->base);
        if (uv_is_writable(handle)) {
            sreq = (uv_shutdown_t *)malloc(sizeof *sreq);
            assert(uv_shutdown(sreq, handle, after_shutdown) == 0);
        }

        return;
    }

    if (nread == 0) {
        /* Everything OK, but nothing read. */
        free(buf->base);
        return;
    }

    /*
     * Scan for the letter Q which signals that we should quit the server.
     * If we get QS it means close the stream.
     * If we get QSS it means shutdown the stream.
     * If we get QSH it means disable linger before close the socket.
     */
    for (i = 0; i < nread; i++) {
        if (buf->base[i] == 'Q') {
            if (i + 1 < nread && buf->base[i + 1] == 'S') {
                int reset = 0;
                if (i + 2 < nread && buf->base[i + 2] == 'S')
                    shutdown = 1;
                if (i + 2 < nread && buf->base[i + 2] == 'H')
                    reset = 1;
                if (reset && handle->type == UV_TCP)
                    assert(uv_tcp_close_reset((uv_tcp_t *)handle, on_close) ==
                           0);
                else if (shutdown)
                    break;
                else
                    uv_close((uv_handle_t *)handle, on_close);
                free(buf->base);
                return;
            } else if (!server_closed) {
                uv_close(server, on_server_close);
                server_closed = 1;
            }
        }
    }

    wr = (write_req_t *)malloc(sizeof *wr);
    assert(wr);
    wr->buf = uv_buf_init(buf->base, nread);

    if (uv_write(&wr->req, handle, &wr->buf, 1, after_write)) {
        fprintf(stderr, "uv_write failed\n");
        abort();
    }

    if (shutdown)
        assert(uv_shutdown((uv_shutdown_t *)malloc(sizeof *sreq), handle,
                           on_shutdown) == 0);
}

static void on_close(uv_handle_t *peer) { free(peer); }

static void echo_alloc(uv_handle_t *handle, size_t suggested_size,
                       uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

static void on_connection(uv_stream_t *server, int status) {
    uv_stream_t *stream;
    int r;

    if (status != 0) {
        fprintf(stderr, "Connect error %s\n", uv_err_name(status));
    }
    assert(status == 0);

    stream = (uv_stream_t *)malloc(sizeof(uv_tcp_t));
    assert(stream);
    r = uv_tcp_init(loop, (uv_tcp_t *)stream);
    assert(r == 0);

    /* associate server with stream */
    stream->data = server;

    r = uv_accept(server, stream);
    assert(r == 0);

    r = uv_read_start(stream, echo_alloc, after_read);
    assert(r == 0);
}

static void on_server_close(uv_handle_t *handle) { assert(handle == server); }

void StartEchoServer(int port) {
    struct sockaddr_in addr;
    loop = uv_default_loop();
    assert(uv_ip4_addr("127.0.0.1", port, &addr) == 0);
    server = (uv_handle_t *)&tcpServer;
    assert(!uv_tcp_init(loop, &tcpServer));
    uv_os_fd_t fd;
    if (uv_fileno(reinterpret_cast<uv_handle_t *>(&tcpServer), &fd) == 0) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
    assert(!uv_tcp_bind(&tcpServer, (const struct sockaddr *)&addr, 0));
    assert(!uv_listen((uv_stream_t *)&tcpServer, SOMAXCONN, on_connection));
    uv_run(loop, UV_RUN_DEFAULT);
}

struct Helper {
    const char *name;
    int (*run)(int argc, char **argv);
};

Helper helpers[] = {
    {"hello", RunHello},        {"set-env", RunSetEnv},
    {"sleep", RunSleep},        {"stdin", RunStandardInput},
    {"print-cwd", RunPrintCwd}, {"tree", RunTree},
    {"lines", RunLines},        {nullptr, nullptr},
};

int RunHelper(const char *name, int argc, char **argv) {
    for (auto *helper = &helpers[0]; helper->name; helper++) {
        if (!strcmp(helper->name, name)) {
            return helper->run(argc, argv);
        }
    }
    assert(0);
}

namespace db {

Database::Database() : db_(nullptr) {
    auto error = sqlite3_open(NEXER_TEST_DB, &db_);
    if (error != 0) {
        log_error("Failed to open %s: %s", NEXER_TEST_DB,
                  sqlite3_errstr(error));
    } else {
        sqlite3_busy_timeout(db_, 5000);
        if (table_count() == 0) {
            CreateTables();
        }
    }
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

int Database::table_count() {
    const char *query = "SELECT COUNT(*) FROM sqlite_master WHERE type='table'";
    sqlite3_stmt *stmt;

    int result = 0;

    if (sqlite3_prepare_v2(db_, query, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Error preparing statement: %s", sqlite3_errmsg(db_));
        return -1;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_int(stmt, 0);
    } else {
        log_error("Error preparing statement: %s", sqlite3_errmsg(db_));
        result = -1;
    }

    sqlite3_finalize(stmt);

    return result;
}

int Database::CreateTables() {
    const char *sql = R"sql(
        create table exec_info(
            scenario varchar(60),
            process varchar(60),
            step int,
            message varchar(200),
            sleep_time int,
            exit_code,
            constraint uk_script unique(scenario, process, step)
        );
        create table process(
            name varchar(60),
            step int,
            running int,
            constraint uk_process unique(name)
        );
        create table config(
            name varchar(60),
            value varchar(60),
            constraint uk_config unique(name)
        );
        insert into config(name, value) values ('scenario', '');
    )sql";

    char *errmsg;
    int result = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);

    if (result != SQLITE_OK) {
        log_error("Error executing query: %s", errmsg);
        sqlite3_free(errmsg);
    }

    return result;
}

int Database::Exec(const std::string &query) {
    sqlite3_stmt *stmt;

    int error = sqlite3_prepare_v2(db_, query.data(), -1, &stmt, nullptr);
    if (error != SQLITE_OK) {
        log_error("Error: %s\nQuery was: %s", sqlite3_errmsg(db_),
                  query.data());
        return -1;
    }

    if ((error = sqlite3_step(stmt)) != SQLITE_DONE) {
        log_error("Error: %s (%s)", sqlite3_errstr(error), query.data());
        return -1;
    }

    long long last_id = sqlite3_last_insert_rowid(db_);

    sqlite3_finalize(stmt);

    return (int)last_id;
}

// Reset the sqlite backing store to the requested scenario and seed it with
// deterministic process rows. Scenarios are declared in the table above and
// drive which NAMEs are valid for the current test.
void Database::SetScenario(const char *name) {
    std::stringstream ss;

    ss << "update config set value='" << name << "' where name='scenario';";

    std::string sql = ss.str();

    Exec(sql);
    Exec("delete from exec_info");
    Exec("delete from process");

    auto &query = scenarios_[name];

    Exec(query);
}

int Database::GetProcessStep(const std::string &name) {
    std::stringstream ss;

    ss << "select step from process where name='" << name << "'";

    std::string sql = ss.str();
    sqlite3_stmt *stmt;

    sqlite3_prepare_v2(db_, sql.data(), -1, &stmt, nullptr);

    int step = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        step = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);

    return step;
}

void Database::OpenProcess(const char *name, ExecInfo &exec_info) {
    std::stringstream ss;
    int step = GetProcessStep(name);
    ss << "select message, sleep_time, exit_code "
       << "from exec_info e join config c on c.name = 'scenario' and "
          "e.scenario = c.value "
       << "where e.step=" << (step >= 0 ? step : 0) << " and process = '"
       << name << "'";

    std::string sql = ss.str();
    sqlite3_stmt *stmt;

    sqlite3_prepare_v2(db_, sql.data(), -1, &stmt, nullptr);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            exec_info.message = (const char *)sqlite3_column_text(stmt, 0);
        }
        exec_info.sleep_time = sqlite3_column_int(stmt, 1);
        exec_info.exit_code = sqlite3_column_int(stmt, 2);
    }

    sqlite3_finalize(stmt);

    ss.str("");

    if (step < 0) {
        ss << "insert into process(name, step, running) values ('" << name
           << "', 0, 1)";
    } else {
        ss << "update process set running=1 where name='" << name << "'";
    }

    sql = ss.str();

    Exec(sql);
}

void Database::CloseProcess(const char *name) {
    std::stringstream ss;

    ss << "update process set step=step+1, running=0 where name='" << name
       << "'";

    std::string sql = ss.str();

    Exec(sql);
}

std::map<std::string, std::string> Database::scenarios_ = {
    {
        "simple",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("simple", "a", 0, "a", 0, 0)
        )sql",
    },
    {
        "simple-check",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("simple-check", "a", 0, "a", 0, 0),
                   ("simple-check", "b", 0, "b", 0, 0)
        )sql",
    },
    {
        "simple-fail",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("simple-fail", "a", 0, "a", 0, 1)
        )sql",
    },
    {
        "simple-fail-check",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("simple-fail-check", "a", 0, "a", 0, 1),
                   ("simple-fail-check", "b", 0, "b", 0, 127),
                   ("simple-fail-check", "p", 0, "p", 0, 127)
        )sql",
    },
    {
        "crash",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("crash", "a", 0, "a", 0, 0),
                   ("crash", "b", 0, "b", 0, 15),
                   ("crash", "b", 1, "b", 0, 0),
                   ("crash", "p", 0, "p", 0, 15)
        )sql",
    },
    {
        "simple-with-sleep",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("simple-with-sleep", "a", 0, "a", 300, 0)
        )sql",
    },
    {
        "simple-with-sleep-fail",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("simple-with-sleep-fail", "a", 0, "a", 0,   0),
                   ("simple-with-sleep-fail", "b", 0, "b", 0,   1),
                   ("simple-with-sleep-fail", "b", 1, "b", 300, 1)
        )sql",
    },
    {
        "check",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check", "a", 0, "a", 0, 0),
                   ("check", "b", 0, "b", 0, 1),
                   ("check", "b", 1, "b", 0, 0)
        )sql",
    },
    {
        "check-fail",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-fail", "a", 0, "a", 0,   0),
                   ("check-fail", "b", 0, "b", 40, 1)
        )sql",
    },
    {
        "check-retry",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-retry", "a", 0, "a", 0,   0),
                   ("check-retry", "b", 0, "b", 0,   1),
                   ("check-retry", "b", 1, "b", 50, 1),
                   ("check-retry", "b", 2, "b", 0,   0)
        )sql",
    },
    {
        "check-retry-failed",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-retry-failed", "a", 0, "a", 0,   0),
                   ("check-retry-failed", "b", 0, "b", 0,   1),
                   ("check-retry-failed", "b", 1, "b", 50, 1),
                   ("check-retry-failed", "b", 2, "b", 0,   1)
        )sql",
    },
    {
        "check-timeout",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-timeout", "a", 0, "a", 0,   0),
                   ("check-timeout", "b", 0, "b", 50, 0),
                   ("check-timeout", "b", 1, "b", 50, 0),
                   ("check-timeout", "b", 2, "b", 50, 0),
                   ("check-timeout", "b", 3, "b", 50, 0)
        )sql",
    },
    {
        "check-error",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-error", "a", 0, "a", 0,   0)
        )sql",
    },
    {
        "check-kill",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-kill", "a", 0, "a", 80, 0),
                   ("check-kill", "b", 0, "b", 0,   1),
                   ("check-kill", "b", 1, "b", 10, 1),
                   ("check-kill", "b", 2, "b", 0,   0),
                   ("check-kill", "b", 3, "b", 0,   1),
                   ("check-kill", "b", 4, "b", 0,   0)
        )sql",
    },
    {
        "check-no-start",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("check-no-start", "a", 0, "a", 10, 0),
                   ("check-no-start", "b", 0, "b", 0,   0)
        )sql",
    },
    {
        "preamble-simple",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("preamble-simple", "a", 0, "a", 0,   0),
                   ("preamble-simple", "b", 0, "b", 0,   1),
                   ("preamble-simple", "b", 1, "b", 0,   0),
                   ("preamble-simple", "p", 0, "p", 0,   0)
        )sql",
    },
    {
        "preamble-multi",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("preamble-multi", "a",  0, "a",  0,   0),
                   ("preamble-multi", "b",  0, "b",  0,   1),
                   ("preamble-multi", "b",  1, "b",  0,   0),
                   ("preamble-multi", "p1", 0, "p1", 0,   0),
                   ("preamble-multi", "p2", 0, "p2", 0,   0)
        )sql",
    },
    {
        "preamble-with-checker",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("preamble-with-checker", "a",  0, "a",  0,   0),
                   ("preamble-with-checker", "b",  0, "b",  0,   1),
                   ("preamble-with-checker", "b",  1, "b",  0,   0),
                   ("preamble-with-checker", "p1", 0, "p1", 0,   0),
                   ("preamble-with-checker", "b1", 0, "b1", 0,   1),
                   ("preamble-with-checker", "b1", 1, "b1", 0,   0),
                   ("preamble-with-checker", "p2", 0, "p2", 0,   0),
                   ("preamble-with-checker", "b2", 0, "b2", 30, 1),
                   ("preamble-with-checker", "b2", 1, "b2", 0,   1),
                   ("preamble-with-checker", "b2", 2, "b2", 0,   0)
        )sql",
    },
    {
        "preamble-failure",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("preamble-failure", "a",  0, "a",  0,   0),
                   ("preamble-failure", "b",  0, "b",  0,   1),
                   ("preamble-failure", "b",  1, "b",  0,   0),
                   ("preamble-failure", "p1", 0, "p1", 0,   0),
                   ("preamble-failure", "b1", 0, "b1", 0,   1),
                   ("preamble-failure", "b1", 1, "b1", 0,   0),
                   ("preamble-failure", "p2", 0, "p2", 0,   0),
                   ("preamble-failure", "b2", 0, "b2", 0,   1),
                   ("preamble-failure", "b2", 1, "b2", 0,   0),
                   ("preamble-failure", "p3", 0, "p3", 0,   0),
                   ("preamble-failure", "b3", 0, "b3", 20, 1),
                   ("preamble-failure", "b3", 1, "b3", 20, 2),
                   ("preamble-failure", "b3", 2, "b3", 20, 3)
        )sql",
    },
    {
        "admin-long-run",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("admin-long-run", "a", 0, "admin-long-run", 60000, 0)
        )sql",
    },
    {
        "admin-stateflow",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("admin-stateflow", "a", 0, "stateflow-0", 1000, 0),
                   ("admin-stateflow", "a", 1, "stateflow-1", 1000, 0),
                   ("admin-stateflow", "a", 2, "stateflow-2", 1000, 5)
        )sql",
    },
    {
        "admin-log-burst",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("admin-log-burst", "a", 0,
                replace("L01|L02|L03|L04|L05|L06|L07|L08|L09|L10|L11|L12|L13|L14|L15|L16|L17|L18|L19|L20", "|", char(10)),
                100, 0)
        )sql",
    },
    {
        "admin-restart",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("admin-restart", "a", 0, "restart", 60000, 0),
                   ("admin-restart", "a", 1, "restart", 60000, 0)
        )sql",
    },
    {
        "task-rerun",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("task-rerun", "a", 0, "a", 50, 0),
                   ("task-rerun", "a", 1, "a", 0,  0)
        )sql",
    },
    {
        "runtime-ready",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("runtime-ready", "b", 0, "b", 400, 1),
                   ("runtime-ready", "b", 1, "b", 0,   0),
                   ("runtime-ready", "b", 2, "b", 0,   0),
                   ("runtime-ready", "b", 3, "b", 0,   0)
        )sql",
    },
    {
        "keep-alive",
        R"sql(
            insert into exec_info(scenario, process, step, message, sleep_time, exit_code)
            values ("keep-alive", "a", 0, "a", 0, 1),
                   ("keep-alive", "a", 1, "a", 300, 0)
        )sql",
    },

};

} // namespace db

// Call this at the start of a test to select the scenario before any helper
// processes spawn. The corresponding scenario must insert rows whose `process`
// values match the NAME env of the processes you plan to launch.
void SetScenario(const char *name) {
    db::Database db;
    log_debug("-------------------- Scenario %s --------------------", name);
    db.SetScenario(name);
}

} // namespace test
} // namespace nexer
