#ifndef NEXER_PROXY_PROTOCOL_H_
#define NEXER_PROXY_PROTOCOL_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "llhttp.h"

namespace nexer {
namespace proxy {

// What an http+socks client sent before its tunnel exists.
struct Request {
    enum class Kind {
        Incomplete,
        Socks,   // SOCKS4/5 greeting, passed through to a SOCKS upstream
        Connect, // HTTP CONNECT host:port
        Forward, // HTTP request with an absolute http:// target
        Invalid,
    };
    Kind kind = Kind::Incomplete;
    std::string host;
    int port = 0;
    // Size of the client's HTTP head (Connect and Forward).
    size_t head_size = 0;
    // Forward only: the head rewritten for the origin server.
    std::string head;
};

// llhttp over one client request, so a forwarded connection carries exactly
// one request to its origin. llhttp knows only a fixed list of methods and
// framing does not depend on the method (CONNECT aside), so it parses the
// request with a stand-in method.
class RequestParser {
  public:
    enum class Result { More, Complete, Error };

    RequestParser();
    RequestParser(const RequestParser &) = delete;
    RequestParser &operator=(const RequestParser &) = delete;

    // Reads everything the client has sent so far. While the result is
    // Incomplete, call again with the same bytes plus what arrived since;
    // only the new ones are parsed.
    Request parse(const std::string &data, size_t max_head_bytes);

    // After a Forward: feeds the client bytes after the head. On Complete,
    // `used` is how many of them belong to the request; once complete, every
    // later feed uses none.
    Result feed(const char *data, size_t len, size_t &used);

  private:
    llhttp_t parser_;
    size_t parsed_ = 0;
    size_t method_size_ = 0;
    bool connect_ = false;
    std::string url_;
    std::string field_;
    std::string value_;
    std::vector<std::pair<std::string, std::string>> headers_;
    // Header fields after the head are trailers, which are not kept.
    bool head_done_ = false;
    bool complete_ = false;
    bool error_ = false;
};

std::string Socks5Greeting();
std::string Socks5Connect(const std::string &host, int port);
// Both return the reply size once complete, 0 while incomplete and -1 when
// the server refuses.
int Socks5GreetingReply(const std::string &data);
int Socks5ConnectReply(const std::string &data);

} // namespace proxy
} // namespace nexer

#endif // NEXER_PROXY_PROTOCOL_H_
