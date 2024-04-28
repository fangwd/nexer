/* Copyright (c) Weidong Fang. All rights reserved. */

#include "tcp_proxy.h"

#include "logger.h"
#include "string_buffer.h"
#include "tcp_proxy.h"

namespace nexer {

struct ProxyClient {
    TcpClient *tcp;
    std::string data[2];
    std::string *data_out;
    std::string *data_in;

    ProxyClient(TcpClient *tcp = nullptr) : tcp(tcp), data_in(&data[0]), data_out(&data[1]) {}
};

class Forwarder {
  private:
    ProxyClient incoming_;
    ProxyClient outgoing_;

    void Setup(ProxyClient *client) {
        auto peer = client == &incoming_ ? &outgoing_ : &incoming_;
        client->tcp->OnData([=](const char *s, size_t len) {
            if (len > 0) {
                client->data_in->append(s, len);
                if (peer->tcp && peer->tcp->IsWritable() && client->data_out->size() == 0) {
                    std::swap(client->data_in, client->data_out);
                    peer->tcp->Write(client->data_out->data(), client->data_out->size());
                }
            }
        });

        client->tcp->OnSend([=] {
            peer->data_out->clear();
            if (peer->data_in->size() > 0) {
                std::swap(peer->data_in, peer->data_out);
                client->tcp->Write(peer->data_out->data(), peer->data_out->size());
            }
        });

        client->tcp->OnError([=](int err, const char *msg) {
            client->tcp->Close();
            if (peer->tcp && !peer->tcp->IsClosing()) {
                peer->tcp->Close();
            }
        });

        client->tcp->OnClose([=]() {
            client->tcp = nullptr;
            if (peer->tcp == nullptr) {
                delete this;
            } else if (!peer->tcp->IsClosing()) {
                peer->tcp->Close();
            }
        });
    }

    Forwarder(TcpClient &incoming) : incoming_(&incoming) {
        Setup(&incoming_);
    }

  public:
    static Forwarder &Create(TcpClient &client) {
        auto forwarder = new Forwarder(client);
        return *forwarder;
    }

    void SetPeer(TcpClient &client) {
        outgoing_.tcp = &client;
        Setup(&outgoing_);
        if (incoming_.data_in->size() > 0 && incoming_.data_out->size() == 0) {
            std::swap(incoming_.data_in, incoming_.data_out);
            outgoing_.tcp->Write(incoming_.data_out->data(), incoming_.data_out->size());
        }
    }
};

TcpProxy &TcpProxy::Create(EventLoop &loop,config::Upstream &upstream, ProcessManager *pm) {
    auto proxy = new TcpProxy(loop, upstream, pm);
    return *proxy;
}

TcpProxy::TcpProxy(EventLoop& loop, config::Upstream& upstream, ProcessManager *pm)
    :TcpServer(loop), upstream_(upstream), process_manager_(pm) {
    Init();
}

void TcpProxy::Init() {
    TcpServer::OnConnection([this](TcpClient &incoming) {
        auto& forwarder = Forwarder::Create(incoming);
        CheckUpstreamProcess([&](int error) {
            if (error) {
                incoming.Close();
                return;
            }

            TcpClient::Connect(loop(), upstream_.host.data(), upstream_.port, 60000, [&](TcpClient* outgoing) {
                if (!outgoing) {
                    incoming.Close();
                } else {
                    forwarder.SetPeer(*outgoing);
                }
            });
        });
    });
}

void TcpProxy::CheckUpstreamProcess(std::function<void(int)> then) {
    if (!upstream_.app) {
        then(0);
    } else {
        process_manager_->Require(*upstream_.app, [&, then](Process*, int error) {
            log_debug("tcp_proxy: %s required (error %d)", upstream_.app->name.data(), error);
            then(error);
        });
    }
}

}  // namespace nexer
