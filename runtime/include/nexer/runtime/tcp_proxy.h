#ifndef NEXER_TCP_PROXY_H_
#define NEXER_TCP_PROXY_H_

#include "nexer/core/tcp_forwarder.h"
#include "nexer/core/tcp_server.h"
#include "nexer/http/http_server.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"
#include "nexer/runtime/proxy_protocol.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace nexer {

class TcpConnector;

class TcpProxy : public TcpServer {
  public:
    // `early`: bytes the upstream sent during negotiation.
    using Connected =
        std::function<void(std::shared_ptr<TcpClient>, std::string early)>;
    // Refused: the upstream said no or closed the connection. Stalled: it
    // did not answer within connect_timeout.
    enum class Negotiation { Ok, Refused, Stalled };
    using Negotiated = std::function<void(Negotiation, std::string early)>;
    // Runs on a fresh upstream connection before it is handed to Connected;
    // a failure moves on to the next upstream.
    using Negotiate = std::function<void(std::shared_ptr<TcpClient>,
                                         const config::Upstream &, Negotiated)>;

  private:
    config::Proxy &config_;
    ProcessManager *process_manager_;
    std::set<std::shared_ptr<TcpForwarder>> forwarders_;
    std::vector<std::weak_ptr<class TcpConnector>> connectors_;
    size_t max_forwarder_buffer_bytes_ = 2 * 1024 * 1024;
    uint64_t head_timeout_ms_ = 10000;
    bool shutting_down_ = false;

    // A first-pass negotiation failure, as seen by the second pass.
    struct Failed {
        // Skipped: no app to require, or a refusal while the app was ready.
        bool settled = false;
        // The app's launch count at connect, after a stall or a refusal while
        // the app went down: retried only once the app has relaunched.
        std::optional<uint64_t> launches;
    };

    void init();
    std::weak_ptr<TcpProxy> weak();

    // Passes `done` the first upstream from `index` that connects and
    // negotiates, or nullptr. The first pass tries only ready upstreams, so a
    // healthy standby beats waiting; the second requires each app and retries
    // until connect_timeout, as `failed` allows.
    void route(std::shared_ptr<TcpForwarder>, size_t index, bool waiting,
               Negotiate negotiate, Connected done,
               std::shared_ptr<std::vector<Failed>> failed = nullptr);
    // http+socks: waits for the client's first request, then dispatches it.
    void sniff(std::shared_ptr<TcpForwarder>, std::shared_ptr<TcpClient>);
    void dispatch(std::shared_ptr<TcpForwarder>, std::shared_ptr<TcpClient>,
                  proxy::Request, std::shared_ptr<proxy::RequestParser>);
    // SOCKS5 CONNECT to host:port over `outgoing`, failing after
    // `timeout_ms`; `done` receives any bytes the server sent after its reply.
    void socks5(std::shared_ptr<TcpForwarder>, std::shared_ptr<TcpClient>,
                const std::string &host, int port, uint64_t timeout_ms,
                Negotiated done);
    bool ready(const config::Upstream &);
    bool has(std::shared_ptr<TcpForwarder>);
    void track_connector(const std::shared_ptr<class TcpConnector> &);
    void cleanup_connectors();

    TcpProxy(EventLoop &, config::Proxy &, ProcessManager *,
             size_t max_forwarder_buffer_bytes);

  public:
    static std::shared_ptr<TcpProxy>
    create(EventLoop &, config::Proxy &, ProcessManager *,
           size_t max_forwarder_buffer_bytes = 2 * 1024 * 1024);
    void remove(std::shared_ptr<TcpForwarder>);
    void shutdown();
    inline size_t connections() const { return forwarders_.size(); }
    // http+socks: how long a client may take to send its first request.
    inline void set_head_timeout(uint64_t ms) { head_timeout_ms_ = ms; }
};

} // namespace nexer

#endif // NEXER_TCP_PROXY_H_
