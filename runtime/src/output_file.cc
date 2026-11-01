#include "nexer/runtime/output_file.h"

#include "nexer/core/logger.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nexer {

OutputFile::OutputFile(std::string path, uint64_t max_bytes, int max_files)
    : path_(std::move(path)), max_bytes_(max_bytes), max_files_(max_files) {}

OutputFile::~OutputFile() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool OutputFile::open() {
    if (fd_ >= 0) {
        return true;
    }
    fd_ =
        ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        if (error_.empty()) {
            log_error("output %s: cannot open: %s", path_.c_str(),
                      strerror(errno));
        }
        error_ = std::string("cannot open: ") + strerror(errno);
        return false;
    }
    struct stat st;
    size_ = fstat(fd_, &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
    return true;
}

void OutputFile::rotate() {
    close(fd_);
    fd_ = -1;
    size_ = 0;
    if (max_files_ <= 0) {
        unlink(path_.c_str());
        return;
    }
    unlink((path_ + "." + std::to_string(max_files_)).c_str());
    for (int i = max_files_ - 1; i >= 1; i--) {
        auto from = path_ + "." + std::to_string(i);
        auto to = path_ + "." + std::to_string(i + 1);
        rename(from.c_str(), to.c_str());
    }
    rename(path_.c_str(), (path_ + ".1").c_str());
}

// A write that does not fit the current file rotates first so writes stay
// whole when they can; anything larger than max_bytes is split across files
// so no file ever exceeds max_bytes.
void OutputFile::write(const char *data, size_t len) {
    if (len == 0 || !open()) {
        return;
    }
    size_t remaining = len;
    while (remaining > 0) {
        if (max_bytes_ > 0 && size_ > 0 && size_ + remaining > max_bytes_) {
            rotate();
            if (!open()) {
                return;
            }
        }
        size_t chunk =
            max_bytes_ > 0
                ? static_cast<size_t>(std::min<uint64_t>(remaining, max_bytes_))
                : remaining;
        if (!write_all(data + (len - remaining), chunk)) {
            return;
        }
        size_ += chunk;
        remaining -= chunk;
    }
    if (!error_.empty()) {
        log_info("output %s: writing resumed", path_.c_str());
        error_.clear();
    }
}

bool OutputFile::write_all(const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        auto n = ::write(fd_, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error_.empty()) {
                log_error("output %s: write failed: %s", path_.c_str(),
                          strerror(errno));
            }
            error_ = std::string("write failed: ") + strerror(errno);
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

} // namespace nexer
