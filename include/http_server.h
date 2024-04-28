/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_HTTP_SERVER_H_
#define NEXER_HTTP_SERVER_H_

#include <string>
#include <vector>

#include "http_message.h"
#include "tcp_server.h"
#include "uv.h"

namespace nexer {
namespace http {

class Server : public TcpServer {
  private:
    static void OnRequestComplete(incoming::Message*);

    std::function<void(TcpClient&)> on_connection_;
    std::function<void(incoming::Request&, outgoing::Response&)> on_request_;

    void Init();

    Server(uv_loop_t*);

  public:
    static Server& Create(EventLoop&);

    inline void OnRequest(std::function<void(incoming::Request&, outgoing::Response&)> fn) {
        on_request_ = fn;
    }

    inline void OnConnection(std::function<void(TcpClient&)> fn) {
        on_connection_ = fn;
    }
};

}  // namespace http
}  // namespace nexer

#endif  // NEXER_HTTP_SERVER_H_
