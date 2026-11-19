#ifndef NEXER_TCP_CONNECTOR_H_
#define NEXER_TCP_CONNECTOR_H_

#include <functional>
#include <memory>
#include <string>

#include "nexer/core/event_loop.h"
#include "nexer/core/non_copyable.h"

namespace nexer {

class TcpClient;
class Timer;

struct TcpConnectOptions {
    std::string host;
    int port = 0;
    uint64_t deadline_ms = 0; // 0 means no deadline
    uint64_t retry_interval_ms = 500;
    int max_attempts = 0; // 0 means retry until the deadline
};

class TcpConnector : public std::enable_shared_from_this<TcpConnector>,
                     NonCopyable {
  public:
    using ResultCallback = std::function<void(std::shared_ptr<TcpClient>)>;
    using TryCallback = std::function<void(TcpClient &)>;

    static std::shared_ptr<TcpConnector> create(EventLoop &loop,
                                                TcpConnectOptions options,
                                                ResultCallback cb,
                                                TryCallback on_try = {});

    void cancel();

    ~TcpConnector();

  private:
    TcpConnector(EventLoop &loop, TcpConnectOptions options, ResultCallback cb,
                 TryCallback on_try);

    void start();
    void try_once();
    void handle_connected();
    void handle_error(int err, const char *msg);
    void handle_closed();
    void tick();
    void finish(std::shared_ptr<TcpClient> result);
    bool deadline_exceeded() const;

    EventLoop &loop_;
    TcpConnectOptions options_;
    ResultCallback callback_;
    TryCallback on_try_;

    std::shared_ptr<Timer> timer_;
    std::shared_ptr<Timer> finish_timer_;
    std::shared_ptr<TcpClient> live_client_;

    uint64_t started_ms_ = 0;
    int attempts_ = 0;
    bool finished_ = false;
    bool attempt_inflight_ = false;
    bool cancel_pending_ = false;
    bool retry_timer_active_ = false;

    std::shared_ptr<TcpConnector> self_guard_;
};

} // namespace nexer

#endif // NEXER_TCP_CONNECTOR_H_
