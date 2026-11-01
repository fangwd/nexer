#ifndef NEXER_HTTP_SERVER_H_
#define NEXER_HTTP_SERVER_H_

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "nexer/core/tcp_server.h"
#include "nexer/http/http_message.h"
#include "uv.h"

namespace nexer {
namespace http {

class Server : public TcpServer {
  private:
    static void on_request_complete(incoming::Message *);

    std::function<void(TcpClient &)> on_connection_;
    std::function<void(std::shared_ptr<incoming::Request>,
                       outgoing::Response &)>
        on_request_;
    std::unordered_set<std::shared_ptr<incoming::Request>> active_requests_;
    size_t max_header_bytes_ = 64 * 1024;
    size_t max_body_bytes_ = 2 * 1024 * 1024;
    size_t max_response_bytes_ = 2 * 1024 * 1024;

    void Init();

    Server(uv_loop_t *);

  public:
    static std::shared_ptr<Server> create(EventLoop &);

    inline void
    on_request(std::function<void(std::shared_ptr<incoming::Request>,
                                  outgoing::Response &)>
                   fn) {
        on_request_ = fn;
    }

    inline void on_connection(std::function<void(TcpClient &)> fn) {
        on_connection_ = fn;
    }

    void close();

    inline void set_limits(size_t max_header_bytes, size_t max_body_bytes,
                           size_t max_response_bytes) {
        max_header_bytes_ = max_header_bytes;
        max_body_bytes_ = max_body_bytes;
        max_response_bytes_ = max_response_bytes;
    }

    inline size_t max_header_bytes() const { return max_header_bytes_; }
    inline size_t max_body_bytes() const { return max_body_bytes_; }
    inline size_t max_response_bytes() const { return max_response_bytes_; }
};

} // namespace http
} // namespace nexer

#endif // NEXER_HTTP_SERVER_H_
