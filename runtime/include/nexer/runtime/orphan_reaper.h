#ifndef NEXER_ORPHAN_REAPER_H_
#define NEXER_ORPHAN_REAPER_H_

#include "nexer/core/non_copyable.h"
#include <cstdint>
#include <sys/types.h>

namespace nexer {

// Helper process that outlives its owner and terminates the tracked process
// groups once the owner is gone, however it died. Groups that end while the
// owner lives are forgotten.
class OrphanReaper : NonCopyable {
  private:
    int fd_ = -1;

  public:
    OrphanReaper();
    ~OrphanReaper();

    // Once the owner is gone the group gets SIGTERM, then SIGKILL if it is
    // still there after `force_after_ms`.
    void track(pid_t group, uint64_t force_after_ms);
};

} // namespace nexer

#endif // NEXER_ORPHAN_REAPER_H_
