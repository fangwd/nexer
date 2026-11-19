#ifndef NEXER_H_
#define NEXER_H_

#include "nexer/core/handle.h"
#include "nexer/core/tcp_client.h"
#include "nexer/core/tcp_forwarder.h"
#include "nexer/core/timer.h"
#include "nexer/runtime/admin_server.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"
#include "nexer/runtime/tcp_proxy.h"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nexer {

class Nexer : public AdminRuntime, NonCopyable {
  private:
    // A dummy listener owns its accepted connections: libuv keeps reading on
    // them, so they must outlive the connection callback.
    class DummyServer : public std::enable_shared_from_this<DummyServer>,
                        NonCopyable {
      public:
        static std::shared_ptr<DummyServer> CreateTcp(EventLoop &, int port,
                                                      bool echo);
        static std::shared_ptr<DummyServer> CreateUdp(EventLoop &, int port);
        void Close();

      private:
        std::shared_ptr<Handle> handle_;
        std::set<std::shared_ptr<TcpClient>> clients_;
        std::set<std::shared_ptr<TcpForwarder>> forwarders_;

        void Track(std::shared_ptr<TcpClient>, bool echo);
    };

    EventLoop loop_;
    Config &config_;
    ProcessManager *process_manager_;
    std::unique_ptr<AdminServer> admin_server_;
    std::vector<std::shared_ptr<TcpProxy>> proxies_;
    std::vector<std::shared_ptr<DummyServer>> dummy_servers_;
    std::vector<const config::App *> required_apps_;

    uv_signal_t sigint_{};
    uv_signal_t sigterm_{};
    bool signals_installed_ = false;

    std::shared_ptr<Timer> startup_timer_;
    std::shared_ptr<Timer> shutdown_timer_;
    std::shared_ptr<Timer> drain_timer_;

    bool startup_complete_ = false;
    bool startup_failed_ = false;
    bool shutdown_requested_ = false;
    bool shutting_down_ = false;
    bool loop_running_ = false;
    int exit_code_ = 0;
    std::function<void()> on_ready_;

    bool start_dummy_server(config::Dummy &);
    bool install_signal_handlers();
    void collect_required_apps();
    void start_apps();
    void finish_startup(uint64_t elapsed_ms);
    void fail_startup(const char *reason);
    void begin_shutdown(ShutdownOptions);
    void drain_proxies(ShutdownOptions);
    void stop_processes(ShutdownOptions);
    void finish_shutdown(bool clean);
    void close_listeners();
    void close_signal_handlers();
    static void on_signal(uv_signal_t *, int signum);

  public:
    Nexer(Config &config);
    ~Nexer();

    // Starts listeners and auto_start apps, runs the loop until shutdown
    // completes and returns the process exit code: 0 after a requested
    // shutdown, 1 after a startup failure or incomplete cleanup.
    int run();

    // Called once when startup succeeds.
    inline void on_ready(std::function<void()> fn) {
        on_ready_ = std::move(fn);
    }

    // Asynchronous, idempotent shutdown shared by signals and the admin API.
    // Returns false when a shutdown is already in progress.
    bool request_shutdown(ShutdownOptions) override;

    Readiness readiness() override;

    // True once a shutdown was requested; mutations are rejected from then on.
    inline bool shutting_down() const override { return shutdown_requested_; }

    // Fallback cleanup; run() performs the orderly shutdown.
    void close();
};

} // namespace nexer

#endif // NEXER_H_
