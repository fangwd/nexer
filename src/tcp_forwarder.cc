#include "tcp_forwarder.h"
#include "logger.h"

namespace nexer {

void TcpForwarder::Swap(Client *client) {
    std::swap(client->data_in, client->data_out);
    if (incoming_.tcp == outgoing_.tcp) {
        auto peer = client == &incoming_ ? &outgoing_ : &incoming_;
        std::swap(peer->data_in, peer->data_out);
    }
}

void TcpForwarder::Init(Client *client) {
    auto peer = client == &incoming_ ? &outgoing_ : &incoming_;
    log_debug("forwarder initialised");
    client->tcp->OnData([=](const char *s, size_t len) {
        if (len > 0) {
            client->data_in->append(s, len);
            if (peer->tcp && peer->tcp->IsWritable() && client->data_out->size() == 0) {
                Swap(client);
                peer->tcp->Write(client->data_out->data(), client->data_out->size());
            }
        }
    });

    client->tcp->OnSend([=] {
        peer->data_out->clear();
        if (peer->data_in->size() > 0) {
            Swap(peer);
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
        if (peer->tcp == client->tcp) {
            peer->tcp = nullptr;
        }
        client->tcp = nullptr;
        if (peer->tcp == nullptr) {
            on_close_.Invoke();
            log_debug("forwarder destroyed");
            delete this;
        } else if (!peer->tcp->IsClosing()) {
            peer->tcp->Close();
        }
    });
}


void TcpForwarder::SetOutgoing(TcpClient &client) {
    outgoing_.tcp = &client;
    if (outgoing_.tcp != incoming_.tcp) {
        Init(&outgoing_);
    } else {
        outgoing_.data_in = incoming_.data_in;
        outgoing_.data_out = incoming_.data_out;
    }
    if (incoming_.data_in->size() > 0 && incoming_.data_out->size() == 0) {
        Swap(&incoming_);
        outgoing_.tcp->Write(incoming_.data_out->data(), incoming_.data_out->size());
    }
}

}  // namespace nexer
