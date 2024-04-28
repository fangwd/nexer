/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_TCP_PROXY_H_
#define NEXER_TCP_PROXY_H_

#include "config.h"
#include "tcp_server.h"
#include "http_server.h"
#include "process_manager.h"
#include <sstream>

namespace nexer {

class TcpProxy : public TcpServer {
  private:
    config::Upstream& upstream_;
    ProcessManager *process_manager_;

    void Init();

    void CheckUpstreamProcess(std::function<void(int)>);

    TcpProxy(EventLoop&, config::Upstream&, ProcessManager*);

  public:
    static TcpProxy &Create(EventLoop &, config::Upstream&, ProcessManager*);
};

}  // namespace nexer

#endif  // NEXER_TCP_PROXY_H_
