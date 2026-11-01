#include "nexer/runtime/instance_lock.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nexer {

static uint64_t fnv1a(const std::string &text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

InstanceLock::~InstanceLock() { release(); }

std::string InstanceLock::runtime_dir() {
    if (const char *dir = std::getenv("XDG_RUNTIME_DIR"); dir && *dir) {
        return std::string(dir) + "/nexer";
    }
    if (const char *dir = std::getenv("XDG_STATE_HOME"); dir && *dir) {
        return std::string(dir) + "/nexer";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/state/nexer";
    }
    return "/tmp/nexer-" + std::to_string(getuid());
}

std::string InstanceLock::lock_path(const std::string &config_path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(config_path, ec);
    std::string key = ec ? config_path : canonical.string();
    std::string name = std::filesystem::path(key).filename().string();
    for (auto &ch : name) {
        if (!isalnum(static_cast<unsigned char>(ch)) && ch != '.' &&
            ch != '-' && ch != '_') {
            ch = '_';
        }
    }
    char hash[32];
    snprintf(hash, sizeof hash, "%016llx", (unsigned long long)fnv1a(key));
    return runtime_dir() + "/" + name + "-" + hash + ".lock";
}

bool InstanceLock::acquire(const std::string &config_path, std::string &error) {
    release();
    auto path = lock_path(config_path);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        error = "cannot create " + path + ": " + ec.message();
        return false;
    }
    chmod(std::filesystem::path(path).parent_path().c_str(), 0700);

    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot open " + path + ": " + strerror(errno);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        close(fd);
        if (saved == EWOULDBLOCK) {
            error = "another instance is already running for " + config_path +
                    " (lock " + path + ")";
        } else {
            error = "cannot lock " + path + ": " + strerror(saved);
        }
        return false;
    }
    // Informational only; the flock is the authority.
    if (ftruncate(fd, 0) == 0) {
        auto pid = std::to_string(getpid()) + "\n";
        if (write(fd, pid.data(), pid.size()) < 0) {
            // Ignored: the lock itself is what matters.
        }
    }
    fd_ = fd;
    path_ = path;
    return true;
}

void InstanceLock::release() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    path_.clear();
}

} // namespace nexer
