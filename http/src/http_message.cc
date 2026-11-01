#include "nexer/http/http_message.h"
#include "nexer/core/logger.h"
#include "nexer/http/http_server.h"
#include <cctype>

namespace nexer {

namespace http {
namespace incoming {

int Message::ParserOnUrl(llhttp_t *parser, const char *s, size_t len) {
    if (len) {
        auto request = reinterpret_cast<Request *>(parser->data);
        request->url_string().append(s, len);
    }
    return 0;
}

int Message::ParserOnHeaderField(llhttp_t *parser, const char *s, size_t len) {
    if (len > 0) {
        auto *message = reinterpret_cast<Message *>(parser->data);
        if (message->max_header_bytes_ > 0 &&
            message->header_bytes_ + len > message->max_header_bytes_) {
            message->size_exceeded_ = true;
            message->OnSizeExceeded();
            return 1;
        }
        if (message->parser_state_.header_field) {
            message->headers_.back().key.append(s, len);
        } else {
            message->headers_.emplace_back(s, len);
            message->parser_state_.header_field = true;
        }
        message->header_bytes_ += len;
    }
    return 0;
}

int Message::ParserOnHeaderValue(llhttp_t *parser, const char *s, size_t len) {
    if (len > 0) {
        auto *message = reinterpret_cast<Message *>(parser->data);
        if (message->max_header_bytes_ > 0 &&
            message->header_bytes_ + len > message->max_header_bytes_) {
            message->size_exceeded_ = true;
            message->OnSizeExceeded();
            return 1;
        }
        message->headers_.back().value.append(s, len);
        message->parser_state_.header_field = false;
        message->header_bytes_ += len;
    }
    return 0;
}

int Message::ParserOnHeadersComplete(llhttp_t *parser) {
    auto request = reinterpret_cast<Request *>(parser->data);
    parse_url(request->url(), request->url_string().data());
    return 0;
}

int Message::ParserOnBody(llhttp_t *parser, const char *s, size_t len) {
    if (len > 0) {
        auto *request = reinterpret_cast<Request *>(parser->data);
        if (request->max_body_bytes_ > 0 &&
            request->body_bytes_ + len > request->max_body_bytes_) {
            request->size_exceeded_ = true;
            request->OnSizeExceeded();
            return 1;
        }
        request->body_.Write(s, len);
        request->body_bytes_ += len;
    }
    return 0;
}

int Message::ParserOnMessageComplete(llhttp_t *parser) {
    auto message = reinterpret_cast<Message *>(parser->data);
    message->parser_state_.complete = true;
    if (message->on_complete_) {
        message->on_complete_(message);
    }
    return 0;
}

llhttp_settings_t Message::parser_settings;

void Message::InitParserSettings() {
    auto settings = &parser_settings;
    llhttp_settings_init(settings);
    settings->on_url = ParserOnUrl;
    settings->on_header_field = ParserOnHeaderField;
    settings->on_header_value = ParserOnHeaderValue;
    settings->on_headers_complete = ParserOnHeadersComplete;
    settings->on_body = ParserOnBody;
    settings->on_message_complete = ParserOnMessageComplete;
}

llhttp_errno_t Message::Parse(const char *s, size_t len) {
    parser_state_.error = llhttp_execute(&parser_, s, len);
    return parser_state_.error;
}

void Message::Reset() {
    llhttp_init(&parser_, HTTP_BOTH, &parser_settings);
    parser_.data = this;
    parser_state_ = {false, HPE_OK, false};
    headers_.clear();
    body_.Clear();
    header_bytes_ = 0;
    body_bytes_ = 0;
    size_exceeded_ = false;
}

static const char *http_status_text(int status_code);

Request::Request(Server &server, std::shared_ptr<TcpClient> client)
    : server_(server), client_(std::move(client)) {
    response_.attach_client(client_);
    response_.set_max_response_bytes(server.max_response_bytes());
    SetLimits(server.max_header_bytes(), server.max_body_bytes());
}

void Request::Reset() {
    Message::Reset();
    url_string_.clear();
    url_ = {};
    response_.reset();
}

void Request::DetachClient() {
    response_.detach_client();
    client_.reset();
}

void Request::OnSizeExceeded() {
    if (client_) {
        client_->close();
    }
}

// llhttp_status_name

} // namespace incoming

void Response::set_header(const std::string &name, const std::string &value) {
    head_ << name << ": " << value << "\r\n";
}

namespace outgoing {

extern "C" const char *llhttp_method_name(llhttp_method_t method);

namespace {

bool starts_with_case_insensitive(const std::string &line, const char *prefix) {
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= line.size()) {
            return false;
        }
        char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(line[i])));
        char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

} // namespace

void Response::attach_client(const std::shared_ptr<TcpClient> &client) {
    client_ = client;
}

void Response::detach_client() { client_.reset(); }

void Response::flush_queue() {
    auto client = client_.lock();
    if (!client) {
        return;
    }
    for (auto &payload : write_queue_) {
        client->write(payload);
    }
    write_queue_.clear();
}

void Response::append_filtered_headers(std::string &out) const {
    if (head_.size() == 0) {
        return;
    }
    std::string raw(head_.data(), head_.size());
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos) {
            end = raw.size();
        }
        if (end > pos) {
            std::string line = raw.substr(pos, end - pos);
            if (!starts_with_case_insensitive(line, "content-length:") &&
                !starts_with_case_insensitive(line, "transfer-encoding:")) {
                out.append(line);
                out.append("\r\n");
            }
        }
        pos = end + 2;
    }
}

void Response::write_body(const char *data, size_t len) {
    if (ended_ || chunked_started_ || !data || len == 0) {
        return;
    }
    if (max_response_bytes_ > 0 &&
        (body_.size() + queued_body_bytes_ + len) > max_response_bytes_) {
        if (auto client = client_.lock()) {
            client->close();
        }
        ended_ = true;
        return;
    }
    auto chunk = std::make_shared<std::string>(data, len);
    queued_body_bytes_ += len;
    write_queue_.push_back(std::move(chunk));
}

void Response::end(int status) {
    if (ended_) {
        return;
    }
    if (chunked_started_) {
        end_chunk_encoding();
        return;
    }
    auto client = client_.lock();
    if (!client) {
        return;
    }

    if (status > 0) {
        status_ = status;
    }

    size_t body_len = body_.size() + queued_body_bytes_;
    if (max_response_bytes_ > 0 && body_len > max_response_bytes_) {
        client->close();
        ended_ = true;
        return;
    }

    std::string payload;
    payload.reserve(128 + head_.size() + body_len);
    payload.append("HTTP/1.1 ");
    payload.append(std::to_string(status_));
    payload.push_back(' ');
    payload.append(llhttp_status_name((llhttp_status)status_));
    payload.append("\r\n");
    if (head_.size() > 0) {
        append_filtered_headers(payload);
    }
    payload.append("Connection: Keep-Alive\r\n");
    payload.append("Content-Length: ");
    payload.append(std::to_string(body_len));
    payload.append("\r\n\r\n");

    if (body_.size() > 0) {
        payload.append(body_.data(), body_.size());
    }
    for (auto &chunk : write_queue_) {
        payload.append(*chunk);
    }

    write_queue_.clear();
    queued_body_bytes_ = 0;
    ended_ = true;

    client->write(std::make_shared<std::string>(std::move(payload)));
}

void Response::begin_chunked_encoding() {
    if (ended_ || chunked_started_) {
        return;
    }
    if (body_.size() > 0 || queued_body_bytes_ > 0 || !write_queue_.empty()) {
        log_error(
            "http: cannot start chunked encoding after body data is queued");
        return;
    }
    auto client = client_.lock();
    if (!client) {
        return;
    }

    std::string header;
    header.reserve(64 + head_.size());
    header.append("HTTP/1.1 ");
    header.append(std::to_string(status_));
    header.push_back(' ');
    header.append(llhttp_status_name((llhttp_status)status_));
    header.append("\r\n");
    append_filtered_headers(header);
    header.append("Connection: Keep-Alive\r\n");
    header.append("Transfer-Encoding: chunked\r\n\r\n");

    chunked_started_ = true;
    client->write(std::make_shared<std::string>(std::move(header)));
}

void Response::write_chunk(const std::string &chunk) {
    write_chunk(chunk.data(), chunk.size());
}

void Response::write_chunk(const char *data, size_t size) {
    if (ended_ || !chunked_started_ || chunked_ended_) {
        return;
    }
    if (!data || size == 0) {
        return;
    }
    if (max_response_bytes_ > 0 &&
        response_bytes_ + size > max_response_bytes_) {
        if (auto client = client_.lock()) {
            client->close();
        }
        ended_ = true;
        chunked_ended_ = true;
        return;
    }
    char prefix[32];
    int prefix_len = snprintf(prefix, sizeof prefix, "%zx\r\n", size);
    if (prefix_len <= 0) {
        log_error("http: failed to format chunk size");
        return;
    }

    auto payload = std::make_shared<std::string>();
    payload->reserve(static_cast<size_t>(prefix_len) + size + 2);
    payload->append(prefix, static_cast<size_t>(prefix_len));
    payload->append(data, size);
    payload->append("\r\n", 2);

    if (auto client = client_.lock()) {
        client->write(std::move(payload));
    }
    response_bytes_ += size;
}

void Response::write_trailer(const std::string &header) {
    if (!chunked_started_ || chunked_ended_ || header.empty()) {
        return;
    }
    trailers_.append(header);
    if (trailers_.size() < 2 || trailers_[trailers_.size() - 2] != '\r' ||
        trailers_[trailers_.size() - 1] != '\n') {
        trailers_.append("\r\n");
    }
}

void Response::write_last_chunk() {
    if (!chunked_started_ || chunked_ended_) {
        return;
    }
    std::string payload = "0\r\n";
    if (!trailers_.empty()) {
        payload.append(trailers_);
    }
    payload.append("\r\n");

    if (auto client = client_.lock()) {
        client->write(std::make_shared<std::string>(std::move(payload)));
    }
    trailers_.clear();
    chunked_ended_ = true;
    ended_ = true;
}

void Response::end_chunk_encoding() { write_last_chunk(); }

void Response::reset() {
    status_ = 501;
    head_.Clear();
    body_.Clear();
    write_queue_.clear();
    queued_body_bytes_ = 0;
    ended_ = false;
    chunked_started_ = false;
    chunked_ended_ = false;
    trailers_.clear();
    response_bytes_ = 0;
}

} // namespace outgoing
} // namespace http
} // namespace nexer
