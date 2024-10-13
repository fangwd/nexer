#ifndef NEXER_HTTP_H_
#define NEXER_HTTP_H_

#include <memory>
#include <string>

#include "llhttp.h"
#include "non_copyable.h"
#include "string_buffer.h"
#include "url.h"
#include "tcp_client.h"

namespace nexer {

namespace http {

struct Header {
    std::string key;
    std::string value;

    Header(const char *s, size_t len) {
        key.append(s, len);
    }
};

class Response {
  protected:
    int status_;

    StringBuffer head_;
    StringBuffer body_;

    void Reset();

  public:
    Response() : status_(501) {}

    int status() const {
        return status_;
    }

    StringBuffer &head() {
        return head_;
    }

    StringBuffer &body() {
        return body_;
    }

    inline void SetStatus(int status) {
        status_ = status;
    }

    void SetHeader(const std::string &name, const std::string &value);
};

class Server;

namespace incoming {
class Request;
}

namespace outgoing {

class Response : public nexer::http::Response {
  private:
    StringBuffer start_line_;
    TcpClient* client_;
    uv_buf_t buf_[3];

    void Reset();

    friend class incoming::Request;

  public:
    void End(int status=0);
};

}  // namespace outgoing

namespace incoming {

class Message : NonCopyable {
  protected:
    llhttp_t parser_;
    struct {
        bool header_field;
        llhttp_errno_t error;
        bool complete;
    } parser_state_;

    std::vector<Header> headers_;
    StringBuffer body_;

    void (*on_complete_)(Message *);

    static int ParserOnUrl(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnHeaderField(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnHeaderValue(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnHeadersComplete(llhttp_t *parser);
    static int ParserOnBody(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnMessageComplete(llhttp_t *parser);

    void Reset();

  public:
    Message(): on_complete_(nullptr) {
        Reset();
    }

    virtual ~Message() {}

    static llhttp_settings_t parser_settings;
    static void InitParserSettings();

    llhttp_errno_t Parse(const char *, size_t);

    inline void OnComplete(void (*fn)(Message *)) {
        on_complete_ = fn;
    }

    inline bool IsComplete() {
        return parser_state_.complete;
    }
};

class Request : public Message {
  private:
    std::string url_string_;
    Url url_;

    outgoing::Response response_;

    // The server that receives this request
    Server &server_;
    TcpClient &client_;

    void Reset();

    friend class http::Server;

  public:
    Request(Server&, TcpClient&);

    uint8_t method() {
        return parser_.method;
    }

    Url &url() {
        return url_;
    }

    std::string &url_string() {
        return url_string_;
    }

    Server &server() {
        return server_;
    }
};

}  // namespace incoming
}  // namespace http
}  // namespace nexer

#endif  // NEXER_HTTP_H_
