#ifndef NEXER_TCP_PROXY_H_
#define NEXER_TCP_PROXY_H_

#include "nexer/core/tcp_forwarder.h"
#include "nexer/core/tcp_server.h"
#include "nexer/http/http_server.h"
#include "nexer/runtime/config.h"
#include "nexer/runtime/process_manager.h"
#include <cstddef>
#include <memory>
#include <set>
#include <vector>

namespace nexer {

class TcpConnector;

class TcpProxy : public TcpServer {
  private:
    std::string name_;
    config::Upstream &upstream_;
    ProcessManager *process_manager_;
    std::set<std::shared_ptr<TcpForwarder>> forwarders_;
    std::vector<std::weak_ptr<class TcpConnector>> connectors_;
    size_t max_forwarder_buffer_bytes_ = 2 * 1024 * 1024;
    bool shutting_down_ = false;

    void init();

    void check_upstream_process(std::function<void(int)>);
    bool has(std::shared_ptr<TcpForwarder>);
    void track_connector(const std::shared_ptr<class TcpConnector> &);
    void cleanup_connectors();

    TcpProxy(EventLoop &, config::Upstream &, ProcessManager *,
             size_t max_forwarder_buffer_bytes);

  public:
    static std::shared_ptr<TcpProxy>
    create(EventLoop &, config::Upstream &, ProcessManager *,
           size_t max_forwarder_buffer_bytes = 2 * 1024 * 1024);
    void remove(std::shared_ptr<TcpForwarder>);
    void shutdown();
    inline size_t connections() const { return forwarders_.size(); }
};

} // namespace nexer

#endif // NEXER_TCP_PROXY_H_
