/* Copyright (c) Weidong Fang. All rights reserved. */

#include "http_message.h"
#include "http_server.h"

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
        if (message->parser_state_.header_field) {
            message->headers_.back().key.append(s, len);
        } else {
            message->headers_.emplace_back(s, len);
            message->parser_state_.header_field = true;
        }
    }
    return 0;
}

int Message::ParserOnHeaderValue(llhttp_t *parser, const char *s, size_t len) {
    if (len > 0) {
        auto *message = reinterpret_cast<Message *>(parser->data);
        message->headers_.back().value.append(s, len);
        message->parser_state_.header_field = false;
    }
    return 0;
}

int Message::ParserOnHeadersComplete(llhttp_t *parser) {
    auto request = reinterpret_cast<Request *>(parser->data);
    ParseUrl(request->url(), request->url_string().data());
    return 0;
}

int Message::ParserOnBody(llhttp_t *parser, const char *s, size_t len) {
    if (len > 0) {
        auto *request = reinterpret_cast<Request *>(parser->data);
        request->body_.Write(s, len);
    }
    return 0;
}

int Message::ParserOnMessageComplete(llhttp_t *parser) {
    auto message = reinterpret_cast<Message*>(parser->data);
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
}

static const char *http_status_text(int status_code);

Request::Request(Server &server, TcpClient& client) : server_(server), client_(client) {
    response_.client_ = &client;
}

void Request::Reset() {
    Message::Reset();
    url_string_.clear();
    url_ = {};
    response_.Reset();
}

// llhttp_status_name

}  // namespace incoming

void Response::SetHeader(const std::string &name, const std::string &value) {
    head_ << name << ": " << value << "\r\n";
}

namespace outgoing {

extern "C" const char* llhttp_method_name(llhttp_method_t method);

void Response::End(int status) {
    if (status > 0) {
        status_ = status;
    }

    StringBuffer &sb = start_line_;

    sb << "HTTP/1.1 " << status_ << " " << llhttp_status_name((llhttp_status)status_) << "\r\n";

    head_ << "Connection: Keep-Alive\r\n";
    head_ << "Content-Length: " << body_.size() << "\r\n\r\n";

    buf_[0] = uv_buf_init(sb.data(), sb.size());
    buf_[1] = uv_buf_init(head_.data(), head_.size());
    buf_[2] = uv_buf_init(body_.data(), body_.size());

    client_->Write(buf_, 3);
}

void Response::Reset() {
    status_ = 501;
    start_line_.Clear();
    head_.Clear();
    body_.Clear();
}

}  // namespace outgoing
}  // namespace http
}  // namespace nexer
