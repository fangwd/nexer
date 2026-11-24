#include "nexer/runtime/orphan_reaper.h"
#include "nexer/core/logger.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace nexer {

namespace {

constexpr int kPruneIntervalMs = 1000;
constexpr int kExitPollMs = 50;

struct Record {
    int64_t group;
    uint64_t force_after_ms;
};

bool Gone(pid_t group) { return kill(-group, 0) != 0 && errno == ESRCH; }

void Prune(std::vector<pid_t> &groups) {
    groups.erase(std::remove_if(groups.begin(), groups.end(), Gone),
                 groups.end());
}

void Signal(const std::vector<pid_t> &groups, int signum) {
    for (auto group : groups) {
        kill(-group, signum);
    }
}

// Runs in the helper process. The owner holds the only write end of `fd`, so
// end-of-file means the owner is gone.
[[noreturn]] void Serve(int fd) {
    std::vector<pid_t> groups;
    uint64_t force_after_ms = 0;
    for (;;) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, groups.empty() ? -1 : kPruneIntervalMs) > 0) {
            Record record;
            ssize_t n = read(fd, &record, sizeof record);
            if (n == 0) {
                break;
            }
            if (n == sizeof record) {
                groups.push_back(static_cast<pid_t>(record.group));
                force_after_ms = record.force_after_ms;
            }
        }
        Prune(groups);
    }

    Prune(groups);
    Signal(groups, SIGTERM);
    for (uint64_t waited = 0; !groups.empty() && waited < force_after_ms;
         waited += kExitPollMs) {
        poll(nullptr, 0, kExitPollMs);
        Prune(groups);
    }
    Signal(groups, SIGKILL);
    _exit(0);
}

} // namespace

OrphanReaper::OrphanReaper() {
    int fds[2];
    if (pipe(fds) != 0) {
        log_error("orphan reaper: pipe: %s", strerror(errno));
        return;
    }
    // Close-on-exec keeps spawned children from holding the write end open.
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if (pid == 0) {
        // Forks again so the helper is no child of the owner, then leaves
        // the owner's session so signals aimed at it miss the helper.
        pid_t helper = fork();
        if (helper != 0) {
            _exit(helper < 0 ? 1 : 0);
        }
        setsid();
        for (int signum : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE}) {
            signal(signum, SIG_IGN);
        }
        signal(SIGCHLD, SIG_DFL);
        // Drops every inherited descriptor: the pipe's write end, the
        // instance lock, listeners and the terminal.
        int max_fd = getdtablesize();
        for (int fd = 0; fd < max_fd; fd++) {
            if (fd != fds[0]) {
                close(fd);
            }
        }
        Serve(fds[0]);
    }

    close(fds[0]);
    int status = 0;
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_error("orphan reaper: fork failed; child processes will outlive "
                  "an abnormal exit");
        close(fds[1]);
        return;
    }
    fd_ = fds[1];
}

OrphanReaper::~OrphanReaper() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

void OrphanReaper::track(pid_t group, uint64_t force_after_ms) {
    if (fd_ < 0) {
        return;
    }
    Record record = {group, force_after_ms};
    while (write(fd_, &record, sizeof record) < 0 && errno == EINTR) {
    }
}

} // namespace nexer
