/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_H_
#define NEXER_H_

#include "config.h"
#include "tcp_proxy.h"
#include <vector>

namespace nexer {

class Nexer : NonCopyable {
  private:
    EventLoop loop_;
    Config& config_;
    ProcessManager *process_manager_;
    std::vector<TcpProxy*> proxies_;

    http::Server *admin_server_;
    bool StartAdminServer();

  public:
    Nexer(Config& config);
    ~Nexer();

    bool Start();
    void Close();
};

}

#endif // NEXER_H_