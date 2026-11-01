#ifndef NEXER_TCP_CLIENT_H_
#define NEXER_TCP_CLIENT_H_

#include "nexer/core/event_loop.h"
#include "nexer/core/function_list.h"
#include "nexer/core/handle.h"
#include <memory>
#include <string>

namespace nexer {

class TcpClient : public Handle {
  private:
    uv_tcp_t tcp_;
    bool initialized_ = false;

    inline uv_handle_t *handle() override { return (uv_handle_t *)&tcp_; }

    static void on_addr_info(uv_getaddrinfo_t *resolver, int status,
                             struct addrinfo *res);
    static void on_connect(uv_connect_t *, int status);
    static void on_alloc(uv_handle_t *, size_t, uv_buf_t *);
    static void on_read(uv_stream_t *, ssize_t nread, const uv_buf_t *);
    static void on_write(uv_write_t *, int status);

    void get_addr_info(const char *node, const char *service);
    void connect_addr(const sockaddr *addr);

    FunctionList<void> on_connect_;
    FunctionList<void> on_send_;
    FunctionList<void, const char *, size_t> on_data_;

    struct {
        unsigned connecting : 1;
        unsigned name_resolving : 1;
    } flags_;

    void read_start();

    TcpClient(uv_loop_t *);

    friend class TcpServer;

  public:
    static std::shared_ptr<TcpClient> create(EventLoop &);
    static std::shared_ptr<TcpClient> create(uv_loop_t *);

    inline auto on_connect(std::function<void()> fn) {
        return on_connect_.add(fn);
    }

    inline auto on_data(std::function<void(const char *, size_t)> fn) {
        return on_data_.add(fn);
    }

    inline auto on_send(std::function<void()> fn) { return on_send_.add(fn); }

    void connect(const char *host, int port);
    void connect(int port);
    // Copies data into an internal buffer; caller may release after return.
    void write(const char *, size_t);
    // Keeps shared data alive until the write completes.
    void write(std::shared_ptr<std::string> data);
    void close();

    inline bool is_writable() const {
        return uv_is_writable((const uv_stream_t *)&tcp_);
    }
};

} // namespace nexer

#endif // NEXER_TCP_CLIENT_H_
