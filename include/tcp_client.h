/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_TCP_CLIENT_H_
#define NEXER_TCP_CLIENT_H_

#include "event_loop.h"
#include "handle.h"

namespace nexer {

class TcpClient: public Handle {
  private:
    uv_tcp_t tcp_;

    inline uv_handle_t *handle() override {
        return (uv_handle_t*) &tcp_;
    }

    static void OnAddrInfo(uv_getaddrinfo_t *resolver, int status, struct addrinfo *res);
    static void OnConnect(uv_connect_t *, int status);
    static void OnAlloc(uv_handle_t *, size_t, uv_buf_t *);
    static void OnRead(uv_stream_t *, ssize_t nread, const uv_buf_t *);
    static void OnWrite(uv_write_t*, int status);
    static void OnWrite2(uv_write_t*, int status);

    void GetAddrInfo(const char *node, const char *service);
    void ConnectAddr(const sockaddr *addr);

    std::function<void()> on_connect_;
    std::function<void()> on_send_;
    std::function<void(const char*, size_t)> on_data_;

    struct {
        unsigned connecting : 1;
        unsigned name_resolving : 1;
    } flags_;

    void ReadStart();

    TcpClient(uv_loop_t*);

    friend class TcpServer;

  public:
    static TcpClient& Create(EventLoop&);
    static TcpClient& Create(uv_loop_t*);

    inline void OnConnect(std::function<void()> fn) {
        on_connect_ = fn;
    }

    inline void OnData(std::function<void(const char*, size_t)> fn) {
        on_data_ = fn;
    }

    inline void OnSend(std::function<void()> fn) {
        on_send_ = fn;
    }

    void Connect(const char *host, int port);
    void Connect(int port);
    void Write(const char *, size_t);
    void Write(uv_buf_t*, size_t);

    inline bool IsWritable() const {
        return uv_is_writable((const uv_stream_t *)&tcp_);
    }

    inline bool IsConnecting() {
        return flags_.connecting || flags_.name_resolving;
    }

    static void Connect(EventLoop& loop, const char *host, int port, uint64_t timeout,
                        std::function<void(TcpClient*)>,
                        std::function<void(TcpClient&)> = {});
};

}  // namespace nexer

#endif  // NEXER_TCP_CLIENT_H_
