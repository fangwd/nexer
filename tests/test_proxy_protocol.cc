#include <assert.h>
#include <memory>
#include <string>

#include "nexer/runtime/proxy_protocol.h"

namespace nexer {
namespace test {

using Kind = proxy::Request::Kind;
using Parser = proxy::RequestParser;
using Result = Parser::Result;

static proxy::Request ParseRequest(const std::string &data,
                                   size_t max_head_bytes) {
    return Parser().parse(data, max_head_bytes);
}

static void TestClassify() {
    assert(ParseRequest("", 1024).kind == Kind::Incomplete);
    assert(ParseRequest(std::string("\x05\x01\x00", 3), 1024).kind ==
           Kind::Socks);
    assert(ParseRequest(std::string("\x04\x01", 2), 1024).kind == Kind::Socks);
    assert(ParseRequest("CONNECT a:1 HTTP/1.1\r\n", 1024).kind ==
           Kind::Incomplete);
    assert(ParseRequest(std::string(2000, 'x'), 1024).kind == Kind::Invalid);
}

static void TestConnect() {
    std::string head = "CONNECT example.com:443 HTTP/1.1\r\n"
                       "Host: example.com:443\r\n\r\n";
    auto request = ParseRequest(head + "early", 1024);
    assert(request.kind == Kind::Connect);
    assert(request.host == "example.com");
    assert(request.port == 443);
    assert(request.head_size == head.size());

    request = ParseRequest("CONNECT [::1]:8443 HTTP/1.1\r\n\r\n", 1024);
    assert(request.kind == Kind::Connect);
    assert(request.host == "::1");
    assert(request.port == 8443);

    // CONNECT requires a port.
    assert(ParseRequest("CONNECT example.com HTTP/1.1\r\n\r\n", 1024).kind ==
           Kind::Invalid);
    assert(ParseRequest("CONNECT a:70000 HTTP/1.1\r\n\r\n", 1024).kind ==
           Kind::Invalid);
}

static void TestForward() {
    std::string head = "GET http://example.com:8080/a?b=1 HTTP/1.1\r\n"
                       "Host: example.com:8080\r\n"
                       "Proxy-Connection: keep-alive\r\n"
                       "proxy-authorization: Basic eA==\r\n"
                       "Connection: keep-alive\r\n"
                       "Keep-Alive: timeout=5\r\n"
                       "Accept: */*\r\n\r\n";
    auto request = ParseRequest(head + "body", 4096);
    assert(request.kind == Kind::Forward);
    assert(request.host == "example.com");
    assert(request.port == 8080);
    assert(request.head_size == head.size());
    assert(request.head == "GET /a?b=1 HTTP/1.1\r\n"
                           "Host: example.com:8080\r\n"
                           "Accept: */*\r\n"
                           "Connection: close\r\n\r\n");

    request = ParseRequest("GET http://example.com HTTP/1.0\r\n\r\n", 1024);
    assert(request.kind == Kind::Forward);
    assert(request.port == 80);
    assert(request.head == "GET / HTTP/1.0\r\n"
                           "Host: example.com\r\n"
                           "Connection: close\r\n\r\n");

    request = ParseRequest("GET http://[::1]?q HTTP/1.1\r\n\r\n", 1024);
    assert(request.kind == Kind::Forward);
    assert(request.host == "::1");
    assert(request.head.compare(0, 18, "GET /?q HTTP/1.1\r\n") == 0);

    // Origin-form and non-http targets are not proxy requests.
    assert(ParseRequest("GET / HTTP/1.1\r\n\r\n", 1024).kind == Kind::Invalid);
    assert(ParseRequest("GET https://a/ HTTP/1.1\r\n\r\n", 1024).kind ==
           Kind::Invalid);
    assert(ParseRequest("GET http://a/ FOO\r\n\r\n", 1024).kind ==
           Kind::Invalid);
    assert(
        ParseRequest("GET http://a/ HTTP/1.1\r\nno-colon\r\n\r\n", 1024).kind ==
        Kind::Invalid);
}

// Host comes from the target, and fields named in Connection are dropped.
static void TestForwardHopByHop() {
    auto request = ParseRequest("GET http://a.test/ HTTP/1.1\r\n"
                                "Host: b.test\r\n"
                                "Connection: X-Secret , keep-alive\r\n"
                                "x-secret: 1\r\n"
                                "X-Empty:\r\n"
                                "X-Kept: 2 \t\r\n\r\n",
                                1024);
    assert(request.kind == Kind::Forward);
    assert(request.head == "GET / HTTP/1.1\r\n"
                           "Host: a.test\r\n"
                           "X-Empty: \r\n"
                           "X-Kept: 2\r\n"
                           "Connection: close\r\n\r\n");
}

static bool Invalid(const std::string &head) {
    return ParseRequest(head, 1024).kind == Kind::Invalid;
}

// llhttp never sees the method, so it must be a token; llhttp checks the
// target, which must not carry a fragment, and the version must be HTTP/1.x.
static void TestRequestLineValidation() {
    assert(Invalid(" http://a/ HTTP/1.1\r\n\r\n"));
    assert(Invalid("BAD(METHOD http://a/ HTTP/1.1\r\n\r\n"));
    const char nul[] = "GE\0T http://a/ HTTP/1.1\r\n\r\n";
    assert(Invalid(std::string(nul, sizeof(nul) - 1)));
    assert(Invalid("GET http://a/\x01x HTTP/1.1\r\n\r\n"));
    assert(Invalid("GET http://a/\tx HTTP/1.1\r\n\r\n"));
    assert(Invalid("GET http://a/\x7f HTTP/1.1\r\n\r\n"));
    assert(Invalid("GET http://a/\xc3\xa9 HTTP/1.1\r\n\r\n"));
    assert(Invalid("GET http://a/x\rGET /y HTTP/1.1\r\n\r\n"));
    assert(Invalid("CONNECT a\x01"
                   "b:443 HTTP/1.1\r\n\r\n"));
    assert(Invalid("CONNECT a:443 HTTP/\r\n\r\n"));
    assert(Invalid("CONNECT a:443 HTTP/1\r\n\r\n"));
    assert(Invalid("CONNECT a:443 HTTP/1.1x\r\n\r\n"));
    assert(Invalid("GET http://a/ HTTP/\r\n\r\n"));
    assert(Invalid("GET http://a/ HTTP/2.0\r\n\r\n"));
    assert(Invalid("GET http://a/ HTTP/0.9\r\n\r\n"));
    assert(Invalid("GET http://a/x#f HTTP/1.1\r\n\r\n"));
    assert(Invalid("GET\r\n\r\n"));
    assert(ParseRequest("GE", 1024).kind == Kind::Incomplete);
    assert(ParseRequest("CONNECT a:443 HTTP/1.0\r\n\r\n", 1024).kind ==
           Kind::Connect);
    assert(ParseRequest("M-SEARCH!#$%&'*+.^_`|~ http://a/%20 HTTP/1.1\r\n\r\n",
                        1024)
               .kind == Kind::Forward);
}

static void TestForwardRejectsFramingOptions() {
    assert(ParseRequest("POST http://a/ HTTP/1.1\r\n"
                        "Connection: close, Content-Length\r\n"
                        "Content-Length: 3\r\n\r\nabc",
                        1024)
               .kind == Kind::Invalid);
    assert(ParseRequest("POST http://a/ HTTP/1.1\r\n"
                        "Connection: transfer-encoding\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n",
                        1024)
               .kind == Kind::Invalid);
}

// A head that arrives in pieces parses to the same request.
static void TestParseInPieces() {
    for (std::string head : {"CONNECT example.com:443 HTTP/1.1\r\n"
                             "Host: example.com:443\r\n\r\n",
                             "VERSION-CONTROL http://a.test:81/x HTTP/1.1\r\n"
                             "Host: b.test\r\n"
                             "X-Kept: 1\r\n\r\n"}) {
        auto whole = ParseRequest(head, 1024);
        Parser parser;
        for (size_t size = 0; size < head.size(); size++) {
            assert(parser.parse(head.substr(0, size), 1024).kind ==
                   Kind::Incomplete);
        }
        auto request = parser.parse(head, 1024);
        assert(request.kind == whole.kind);
        assert(request.kind != Kind::Invalid);
        assert(request.host == whole.host);
        assert(request.port == whole.port);
        assert(request.head_size == head.size());
        assert(request.head == whole.head);
    }
}

// The parser that read `head`, ready for what follows it.
static std::shared_ptr<Parser> AfterHead(const std::string &head) {
    auto parser = std::make_shared<Parser>();
    auto request = parser->parse(head, 4096);
    assert(request.kind == Kind::Forward);
    assert(request.head_size == head.size());
    return parser;
}

static Result Feed(const std::shared_ptr<Parser> &parser,
                   const std::string &data, size_t &used) {
    return parser->feed(data.data(), data.size(), used);
}

static void TestFraming() {
    size_t used = 0;
    {
        auto parser = AfterHead("GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n");
        assert(Feed(parser, "", used) == Result::Complete);
        assert(used == 0);
        assert(Feed(parser, "x", used) == Result::Complete);
        assert(used == 0);
    }
    {
        auto parser =
            AfterHead("POST http://a/ HTTP/1.1\r\nContent-Length: 5\r\n\r\n");
        assert(Feed(parser, "ab", used) == Result::More);
        assert(used == 2);
        assert(Feed(parser, "cdeGET", used) == Result::Complete);
        assert(used == 3);
    }
    {
        auto parser = AfterHead("POST http://a/ HTTP/1.1\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
        std::string body = "3\r\nabc\r\n0\r\n\r\n";
        auto pipelined = "GET http://b/ HTTP/1.1\r\n\r\n";
        assert(Feed(parser, body + pipelined, used) == Result::Complete);
        assert(used == body.size());
    }
    {
        auto parser = AfterHead("POST http://a/ HTTP/1.1\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
        std::string body = "3\r\nabc\r\n0\r\nX-Trailer: 1\r\n\r\n";
        assert(Feed(parser, body.substr(0, 20), used) == Result::More);
        assert(used == 20);
        assert(Feed(parser, body.substr(20) + "GET", used) == Result::Complete);
        assert(used == body.size() - 20);
    }
    {
        auto parser = AfterHead("POST http://a/ HTTP/1.1\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
        assert(Feed(parser, "zz\r\n", used) == Result::Error);
    }
    assert(Invalid("POST http://a/ HTTP/1.1\r\n"
                   "Content-Length: 1\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n"));
}

// Methods outside llhttp's list (RFC 3253 VERSION-CONTROL) parse and frame
// like any other request.
static void TestFramingExtensionMethod() {
    auto parser = AfterHead("VERSION-CONTROL http://a/doc HTTP/1.1\r\n"
                            "Content-Length: 2\r\n\r\n");
    size_t used = 0;
    assert(Feed(parser, "ok", used) == Result::Complete);
    assert(used == 2);
}

static void TestSocks5() {
    assert(proxy::Socks5Greeting() == std::string("\x05\x01\x00", 3));
    assert(proxy::Socks5Connect("ab", 0x1bb) ==
           std::string("\x05\x01\x00\x03\x02"
                       "ab\x01\xbb",
                       9));

    assert(proxy::Socks5GreetingReply(std::string("\x05", 1)) == 0);
    assert(proxy::Socks5GreetingReply(std::string("\x05\x00", 2)) == 2);
    assert(proxy::Socks5GreetingReply(std::string("\x05\xff", 2)) == -1);

    std::string ipv4("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
    assert(proxy::Socks5ConnectReply(ipv4.substr(0, 9)) == 0);
    assert(proxy::Socks5ConnectReply(ipv4) == 10);
    assert(proxy::Socks5ConnectReply(ipv4 + "more") == 10);
    std::string domain("\x05\x00\x00\x03\x01x\x00\x00", 8);
    assert(proxy::Socks5ConnectReply(domain) == 8);
    std::string refused("\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00", 10);
    assert(proxy::Socks5ConnectReply(refused) == -1);
}

void TestProxyProtocol() {
    TestClassify();
    TestConnect();
    TestForward();
    TestForwardHopByHop();
    TestRequestLineValidation();
    TestForwardRejectsFramingOptions();
    TestParseInPieces();
    TestFraming();
    TestFramingExtensionMethod();
    TestSocks5();
}

} // namespace test
} // namespace nexer
