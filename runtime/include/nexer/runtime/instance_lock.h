#ifndef NEXER_INSTANCE_LOCK_H_
#define NEXER_INSTANCE_LOCK_H_

#include "nexer/core/non_copyable.h"
#include <string>

namespace nexer {

// OS-backed advisory lock keyed by the current user and the canonical config
// path, so one configuration runs at most once per user. The kernel releases
// it when the holding process exits for any reason.
class InstanceLock : NonCopyable {
  private:
    int fd_ = -1;
    std::string path_;

  public:
    InstanceLock() = default;
    ~InstanceLock();

    // User-owned directory holding lock files.
    static std::string runtime_dir();
    static std::string lock_path(const std::string &config_path);

    // Returns false with a message when another instance holds the lock or
    // the lock file cannot be created.
    bool acquire(const std::string &config_path, std::string &error);
    void release();

    inline bool held() const { return fd_ >= 0; }
    inline const std::string &path() const { return path_; }
};

} // namespace nexer

#endif // NEXER_INSTANCE_LOCK_H_
