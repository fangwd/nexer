#ifndef NEXER_TCP_FORWARDER_H_
#define NEXER_TCP_FORWARDER_H_

#include "function_list.h"
#include "tcp_client.h"

namespace nexer {

class TcpForwarder {
    struct Client {
        TcpClient *tcp;
        std::string data[2];
        std::string *data_out;
        std::string *data_in;
        Client(TcpClient *tcp = nullptr) : tcp(tcp), data_in(&data[0]), data_out(&data[1]) {}
    };

  private:
    Client incoming_;
    Client outgoing_;
    FunctionList<void> on_close_;

    void Init(Client *client);
    void Swap(Client *client);

    TcpForwarder(TcpClient &incoming) : incoming_(&incoming) {
        Init(&incoming_);
    }

  public:
    static TcpForwarder &Create(TcpClient &client) {
        auto forwarder = new TcpForwarder(client);
        return *forwarder;
    }

    static TcpForwarder &Create(TcpClient &incoming, TcpClient& outgoing) {
        auto forwarder = new TcpForwarder(incoming);
        forwarder->SetOutgoing(outgoing);
        return *forwarder;
    }

    void SetOutgoing(TcpClient &client);

    inline auto OnClose(std::function<void()> fn) {
        return on_close_.Add(fn);
    }

    inline bool IsClosed(TcpClient *tcp) {
        return tcp && incoming_.tcp != tcp && outgoing_.tcp != tcp;
    }
};

}  // namespace nexer

#endif  // NEXER_TCP_FORWARDER_H_
