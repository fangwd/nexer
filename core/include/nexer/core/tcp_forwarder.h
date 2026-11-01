#ifndef NEXER_TCP_FORWARDER_H_
#define NEXER_TCP_FORWARDER_H_

#include "nexer/core/function_list.h"
#include "nexer/core/tcp_client.h"
#include <cstddef>
#include <memory>
#include <string>

namespace nexer {

class TcpForwarder : public std::enable_shared_from_this<TcpForwarder> {
    struct Client {
        std::shared_ptr<TcpClient> tcp;
        std::string data[2];
        std::string *data_out;
        std::string *data_in;
        Client(std::shared_ptr<TcpClient> tcp = nullptr)
            : tcp(tcp), data_in(&data[0]), data_out(&data[1]) {}
    };

  private:
    Client incoming_;
    Client outgoing_;
    FunctionList<void> on_close_;
    size_t max_buffer_bytes_;

    void init(Client *client);
    void swap(Client *client);

    TcpForwarder(std::shared_ptr<TcpClient> incoming, size_t max_buffer_bytes)
        : incoming_(incoming), max_buffer_bytes_(max_buffer_bytes) {}

  public:
    static std::shared_ptr<TcpForwarder>
    create(std::shared_ptr<TcpClient> client,
           size_t max_buffer_bytes = 2 * 1024 * 1024) {
        auto forwarder = std::shared_ptr<TcpForwarder>(
            new TcpForwarder(client, max_buffer_bytes));
        forwarder->on_close_.set_owner(forwarder);
        forwarder->init(&forwarder->incoming_);
        return forwarder;
    }

    static std::shared_ptr<TcpForwarder>
    create(std::shared_ptr<TcpClient> incoming,
           std::shared_ptr<TcpClient> outgoing,
           size_t max_buffer_bytes = 2 * 1024 * 1024) {
        auto forwarder = std::shared_ptr<TcpForwarder>(
            new TcpForwarder(incoming, max_buffer_bytes));
        forwarder->on_close_.set_owner(forwarder);
        forwarder->init(&forwarder->incoming_);
        forwarder->set_outgoing(outgoing);
        return forwarder;
    }

    void set_outgoing(std::shared_ptr<TcpClient> client);
    void shutdown();

    inline auto on_close(std::function<void()> fn) { return on_close_.add(fn); }

    inline bool is_closed(std::shared_ptr<TcpClient> tcp) {
        return tcp && incoming_.tcp != tcp && outgoing_.tcp != tcp;
    }
};

} // namespace nexer

#endif // NEXER_TCP_FORWARDER_H_
