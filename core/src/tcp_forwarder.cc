#include "nexer/core/tcp_forwarder.h"
#include "nexer/core/logger.h"

namespace nexer {

void TcpForwarder::swap(Client *client) {
    std::swap(client->data_in, client->data_out);
    if (incoming_.tcp == outgoing_.tcp) {
        auto peer = client == &incoming_ ? &outgoing_ : &incoming_;
        std::swap(peer->data_in, peer->data_out);
    }
}

void TcpForwarder::init(Client *client) {
    auto peer = client == &incoming_ ? &outgoing_ : &incoming_;
    log_debug("forwarder initialised");

    auto self_weak = std::weak_ptr<TcpForwarder>(shared_from_this());

    client->tcp->on_data([=](const char *s, size_t len) {
        auto self = self_weak.lock();
        if (!self)
            return;

        if (len > 0) {
            if (self->max_buffer_bytes_ > 0 &&
                client->data_in->size() + len > self->max_buffer_bytes_) {
                if (client->tcp && !client->tcp->is_closing()) {
                    client->tcp->close();
                }
                if (peer->tcp && !peer->tcp->is_closing()) {
                    peer->tcp->close();
                }
                return;
            }
            client->data_in->append(s, len);
            if (peer->tcp && peer->tcp->is_writable() &&
                client->data_out->size() == 0) {
                // Let's use self to access members to be safe.
                auto s_incoming = &self->incoming_;
                auto s_outgoing = &self->outgoing_;
                auto s_client =
                    (client == &self->incoming_) ? s_incoming : s_outgoing;
                auto s_peer =
                    (client == &self->incoming_) ? s_outgoing : s_incoming;

                self->swap(s_client);
                s_peer->tcp->write(s_client->data_out->data(),
                                   s_client->data_out->size());
            }
        }
    });

    client->tcp->on_send([=] {
        auto self = self_weak.lock();
        if (!self)
            return;

        auto s_incoming = &self->incoming_;
        auto s_outgoing = &self->outgoing_;
        auto s_client = (client == &self->incoming_) ? s_incoming : s_outgoing;
        auto s_peer = (client == &self->incoming_) ? s_outgoing : s_incoming;

        s_peer->data_out->clear();
        if (s_peer->data_in->size() > 0) {
            self->swap(s_peer);
            s_client->tcp->write(s_peer->data_out->data(),
                                 s_peer->data_out->size());
        }
    });

    client->tcp->on_error([=](int err, const char *msg) {
        auto self = self_weak.lock();
        if (!self)
            return;

        auto s_incoming = &self->incoming_;
        auto s_outgoing = &self->outgoing_;
        auto s_client = (client == &self->incoming_) ? s_incoming : s_outgoing;
        auto s_peer = (client == &self->incoming_) ? s_outgoing : s_incoming;

        s_client->tcp->close();
        if (s_peer->tcp && !s_peer->tcp->is_closing()) {
            s_peer->tcp->close();
        }
    });

    client->tcp->on_close([=]() {
        auto self = self_weak.lock();
        if (!self)
            return;

        auto s_incoming = &self->incoming_;
        auto s_outgoing = &self->outgoing_;
        auto s_client = (client == &self->incoming_) ? s_incoming : s_outgoing;
        auto s_peer = (client == &self->incoming_) ? s_outgoing : s_incoming;

        if (s_peer->tcp == s_client->tcp) {
            s_peer->tcp = nullptr;
        }
        s_client->tcp = nullptr;
        if (s_peer->tcp == nullptr) {
            self->on_close_.invoke();
            log_debug("forwarder destroyed");
            // No more 'delete this' - shared_ptr handles it.
        } else if (!s_peer->tcp->is_closing()) {
            s_peer->tcp->close();
        }
    });
}

void TcpForwarder::set_outgoing(std::shared_ptr<TcpClient> client) {
    outgoing_.tcp = client;
    if (outgoing_.tcp != incoming_.tcp) {
        init(&outgoing_);
    } else {
        outgoing_.data_in = incoming_.data_in;
        outgoing_.data_out = incoming_.data_out;
    }
    if (incoming_.data_in->size() > 0 && incoming_.data_out->size() == 0) {
        swap(&incoming_);
        outgoing_.tcp->write(incoming_.data_out->data(),
                             incoming_.data_out->size());
    }
}

void TcpForwarder::shutdown() {
    auto close_if_needed = [](const std::shared_ptr<TcpClient> &client) {
        if (client && !client->is_closing()) {
            client->close();
        }
    };

    close_if_needed(incoming_.tcp);
    if (outgoing_.tcp != incoming_.tcp) {
        close_if_needed(outgoing_.tcp);
    }
}

} // namespace nexer
