#ifndef NEXER_TCP_SERVER_H_
#define NEXER_TCP_SERVER_H_

#include "nexer/core/event_loop.h"
#include "nexer/core/handle.h"
#include "nexer/core/tcp_client.h"
#include <string>

namespace nexer {

class TcpServer : public Handle {
  protected:
    uv_tcp_t tcp_;

    inline uv_handle_t *handle() override { return (uv_handle_t *)&tcp_; }

    virtual int Accept(TcpClient &);

    static void OnConnection(uv_stream_t *stream, int status);
    std::function<void(std::shared_ptr<TcpClient>)> on_connection_;
    std::function<void()> on_listening_;

    TcpServer(uv_loop_t *);

  public:
    static std::shared_ptr<TcpServer> Create(EventLoop &);

    inline void
    OnConnection(std::function<void(std::shared_ptr<TcpClient>)> fn) {
        on_connection_ = fn;
    }

    bool Listen(int port);
    bool Listen(const std::string &host, int port);
};

} // namespace nexer

#define xnew(t) (t *)malloc(sizeof(t))

#endif // NEXER_TCP_SERVER_H_
