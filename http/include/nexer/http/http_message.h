#ifndef NEXER_HTTP_H_
#define NEXER_HTTP_H_

#include <deque>
#include <memory>
#include <string>

#include "llhttp.h"
#include "nexer/core/non_copyable.h"
#include "nexer/core/string_buffer.h"
#include "nexer/core/tcp_client.h"
#include "nexer/core/url.h"

namespace nexer {

namespace http {

struct Header {
    std::string key;
    std::string value;

    Header(const char *s, size_t len) { key.append(s, len); }
};

class Response {
  protected:
    int status_;

    StringBuffer head_;
    StringBuffer body_;

    void Reset();

  public:
    Response() : status_(501) {}

    int status() const { return status_; }

    StringBuffer &head() { return head_; }

    StringBuffer &body() { return body_; }

    inline void set_status(int status) { status_ = status; }

    void set_header(const std::string &name, const std::string &value);
};

class Server;

namespace incoming {
class Request;
}

namespace outgoing {

class Response : public nexer::http::Response {
  private:
    std::weak_ptr<TcpClient> client_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    size_t queued_body_bytes_ = 0;
    bool ended_ = false;
    bool chunked_started_ = false;
    bool chunked_ended_ = false;
    std::string trailers_;
    size_t max_response_bytes_ = 0;
    size_t response_bytes_ = 0;

    void flush_queue();
    void append_filtered_headers(std::string &out) const;

    void reset();
    void attach_client(const std::shared_ptr<TcpClient> &client);
    void detach_client();

    friend class incoming::Request;

  public:
    void write_body(const char *data, size_t len);
    void end(int status = 0);
    void begin_chunked_encoding();
    void write_chunk(const std::string &chunk);
    void write_chunk(const char *data, size_t size);
    void write_last_chunk();
    void write_trailer(const std::string &header);
    void end_chunk_encoding();

    void set_max_response_bytes(size_t max_bytes) {
        max_response_bytes_ = max_bytes;
    }
};

} // namespace outgoing

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
    size_t header_bytes_ = 0;
    size_t body_bytes_ = 0;
    size_t max_header_bytes_ = 0;
    size_t max_body_bytes_ = 0;
    bool size_exceeded_ = false;

    void (*on_complete_)(Message *);

    static int ParserOnUrl(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnHeaderField(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnHeaderValue(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnHeadersComplete(llhttp_t *parser);
    static int ParserOnBody(llhttp_t *parser, const char *s, size_t len);
    static int ParserOnMessageComplete(llhttp_t *parser);

    void Reset();
    virtual void OnSizeExceeded() {}

  public:
    Message() : on_complete_(nullptr) { Reset(); }

    virtual ~Message() {}

    static llhttp_settings_t parser_settings;
    static void InitParserSettings();

    llhttp_errno_t Parse(const char *, size_t);

    inline void OnComplete(void (*fn)(Message *)) { on_complete_ = fn; }

    inline bool IsComplete() { return parser_state_.complete; }

    inline const std::vector<Header> &headers() const { return headers_; }

    inline const StringBuffer &body() const { return body_; }

    inline StringBuffer &body() { return body_; }

    inline void SetLimits(size_t max_header_bytes, size_t max_body_bytes) {
        max_header_bytes_ = max_header_bytes;
        max_body_bytes_ = max_body_bytes;
    }
};

class Request : public Message, public std::enable_shared_from_this<Request> {
  private:
    std::string url_string_;
    Url url_;

    outgoing::Response response_;

    // The server that receives this request
    Server &server_;
    std::shared_ptr<TcpClient> client_;

    void Reset();
    void DetachClient();

    friend class http::Server;

  public:
    Request(Server &, std::shared_ptr<TcpClient>);
    void OnSizeExceeded() override;

    uint8_t method() { return parser_.method; }

    Url &url() { return url_; }

    std::string &url_string() { return url_string_; }

    Server &server() { return server_; }

    outgoing::Response &response() { return response_; }

    std::shared_ptr<TcpClient> client() { return client_; }
};

} // namespace incoming
} // namespace http
} // namespace nexer

#endif // NEXER_HTTP_H_
