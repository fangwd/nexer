#ifndef NEXER_ADMIN_SERVER_H_
#define NEXER_ADMIN_SERVER_H_

#include <memory>
#include <string>
#include <vector>

#include "nexer/core/event_loop.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"

namespace nexer {

struct ShutdownOptions {
    uint64_t delay_ms = 0;
    uint64_t grace_ms = 0;
    uint64_t force_after_ms = 0;
    std::string reason;
};

struct Readiness {
    bool startup_complete = false;
    bool shutting_down = false;
    bool ready = false;
    // Required apps (auto_start roots and their dependencies) not ready.
    std::vector<ProcessSnapshot> blocking;
};

// Runtime operations the admin API delegates to the owning Nexer instance.
class AdminRuntime {
  public:
    virtual ~AdminRuntime() = default;
    // Returns false when a shutdown is already in progress.
    virtual bool request_shutdown(ShutdownOptions) = 0;
    virtual Readiness readiness() = 0;
    virtual bool shutting_down() const = 0;
};

class AdminServer {
  public:
    AdminServer(EventLoop &, Config &, ProcessManager &);
    ~AdminServer();

    void set_runtime(AdminRuntime *);

    bool start();
    void stop();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nexer

#endif // NEXER_ADMIN_SERVER_H_
