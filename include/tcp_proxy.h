#ifndef NEXER_TCP_PROXY_H_
#define NEXER_TCP_PROXY_H_

#include "config.h"
#include "tcp_server.h"
#include "tcp_forwarder.h"
#include "http_server.h"
#include "process_manager.h"
#include <set>

namespace nexer {

class TcpProxy : public TcpServer {
  private:
    std::string name_;
    config::Upstream& upstream_;
    ProcessManager *process_manager_;
    std::set<TcpForwarder*> forwarders_;

    void Init();

    void CheckUpstreamProcess(std::function<void(int)>);
    bool Has(TcpForwarder&);

    TcpProxy(EventLoop&, config::Upstream&, ProcessManager*);

  public:
    static TcpProxy &Create(EventLoop &, config::Upstream&, ProcessManager*);
    void Remove(TcpForwarder&);
};

}  // namespace nexer

#endif  // NEXER_TCP_PROXY_H_
