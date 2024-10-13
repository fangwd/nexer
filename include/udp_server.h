#ifndef NEXER_UDP_SERVER_H_
#define NEXER_UDP_SERVER_H_

#include "event_loop.h"
#include "function_list.h"
#include "handle.h"

namespace nexer {

class UdpServer : public Handle {
  protected:
    uv_udp_t udp_;
    char slab_[16 * 64 * 1024];

    static void OnAlloc(uv_handle_t*, size_t, uv_buf_t*);
    static void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* rcvbuf, const struct sockaddr* addr,
                       unsigned flags);

    FunctionList<void, const char*, size_t, const struct sockaddr*> on_recv_;

    UdpServer(uv_loop_t*);

  public:
    static UdpServer& Create(EventLoop&);

    inline uv_handle_t* handle() override {
        return (uv_handle_t*)&udp_;
    }

    inline auto OnRecv(std::function<void(const char*, size_t, const struct sockaddr*)> fn) {
        return on_recv_.Add(fn);
    }
    bool Listen(int port);
};

}  // namespace nexer

#define xnew(t) (t*)malloc(sizeof(t))

#endif  // NEXER_UDP_SERVER_H_
