#ifndef NEXER_TCP_CLIENT_H_
#define NEXER_TCP_CLIENT_H_

#include "event_loop.h"
#include "function_list.h"
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

    FunctionList<void> on_connect_;
    FunctionList<void> on_send_;
    FunctionList<void, const char *, size_t> on_data_;

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

    inline auto OnConnect(std::function<void()> fn) {
        return on_connect_.Add(fn);
    }

    inline auto OnData(std::function<void(const char*, size_t)> fn) {
        return on_data_.Add(fn);
    }

    inline auto OnSend(std::function<void()> fn) {
        return on_send_.Add(fn);
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
