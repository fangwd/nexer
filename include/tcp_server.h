/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_TCP_SERVER_H_
#define NEXER_TCP_SERVER_H_

#include "event_loop.h"
#include "handle.h"
#include "tcp_client.h"

namespace nexer {

class TcpServer : public Handle {
  protected:
    uv_tcp_t tcp_;

    inline uv_handle_t *handle() override {
        return (uv_handle_t*) &tcp_;
    }

    virtual int Accept(TcpClient&);

    static void OnConnection(uv_stream_t *stream, int status);
    std::function<void(TcpClient&)> on_connection_;
    std::function<void()> on_listening_;

    TcpServer(uv_loop_t*);

  public:
    static TcpServer& Create(EventLoop&);

    inline void OnConnection(std::function<void(TcpClient&)> fn) {
        on_connection_ = fn;
    }

    bool Listen(int port);
};

}  // namespace nexer

#define xnew(t) (t*)malloc(sizeof(t))

#endif  // NEXER_TCP_SERVER_H_
