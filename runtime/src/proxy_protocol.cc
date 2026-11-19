#include "nexer/runtime/proxy_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <strings.h>

namespace nexer {
namespace proxy {

namespace {

// Splits "host", "host:port" or "[v6]:port"; `default_port` 0 makes the port
// required.
bool SplitHostPort(const std::string &s, int default_port, std::string &host,
                   int &port) {
    std::string port_str;
    if (!s.empty() && s[0] == '[') {
        auto close = s.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = s.substr(1, close - 1);
        if (close + 1 < s.size()) {
            if (s[close + 1] != ':') {
                return false;
            }
            port_str = s.substr(close + 2);
        }
    } else {
        auto colon = s.find(':');
        if (colon != std::string::npos && s.find(':', colon + 1) != s.npos) {
            return false;
        }
        host = s.substr(0, colon);
        if (colon != std::string::npos) {
            port_str = s.substr(colon + 1);
        }
    }
    if (host.empty() || host.size() > 255) {
        return false;
    }
    if (port_str.empty()) {
        port = default_port;
        return port > 0;
    }
    port = 0;
    for (char c : port_str) {
        if (c < '0' || c > '9' || (port = port * 10 + (c - '0')) > 65535) {
            return false;
        }
    }
    return port > 0;
}

// RFC 9110 tchar.
bool IsTokenChar(char c) {
    static const char *kSymbols = "!#$%&'*+-.^_`|~";
    return isalnum(static_cast<unsigned char>(c)) ||
           (c != '\0' && strchr(kSymbols, c) != nullptr);
}

std::string Trim(const std::string &s) {
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

// Proxy-to-origin rewrite: origin-form target, Host taken from the target,
// hop-by-hop headers (including those listed in Connection) dropped and the
// connection closed after one response, so a kept-alive client connection
// can never be reused for a different host.
bool RewriteForward(
    const std::string &method, const std::string &target,
    const std::string &version,
    const std::vector<std::pair<std::string, std::string>> &headers,
    Request &request) {
    static const std::string kScheme = "http://";
    if (strncasecmp(target.data(), kScheme.data(), kScheme.size()) != 0) {
        return false;
    }
    auto rest = target.substr(kScheme.size());
    auto path_at = rest.find_first_of("/?");
    auto authority = rest.substr(0, path_at);
    std::string path =
        path_at == std::string::npos ? "/" : rest.substr(path_at);
    if (path[0] == '?') {
        path.insert(0, "/");
    }
    if (!SplitHostPort(authority, 80, request.host, request.port)) {
        return false;
    }

    std::vector<std::string> drop = {"Host", "Connection", "Keep-Alive",
                                     "Proxy-Connection", "Proxy-Authorization"};
    for (auto &header : headers) {
        if (strcasecmp(header.first.c_str(), "Connection") != 0) {
            continue;
        }
        auto &value = header.second;
        size_t at = 0;
        while (at <= value.size()) {
            auto comma = value.find(',', at);
            auto option = Trim(value.substr(at, comma - at));
            // Dropping a framing field would change where the body ends.
            if (strcasecmp(option.c_str(), "Content-Length") == 0 ||
                strcasecmp(option.c_str(), "Transfer-Encoding") == 0) {
                return false;
            }
            drop.push_back(option);
            if (comma == std::string::npos) {
                break;
            }
            at = comma + 1;
        }
    }

    std::string out = method + " " + path + " " + version + "\r\n" +
                      "Host: " + authority + "\r\n";
    for (auto &header : headers) {
        auto dropped = std::any_of(drop.begin(), drop.end(), [&](auto &d) {
            return strcasecmp(header.first.c_str(), d.c_str()) == 0;
        });
        if (!dropped) {
            out += header.first + ": " + Trim(header.second) + "\r\n";
        }
    }
    out += "Connection: close\r\n\r\n";
    request.head = std::move(out);
    return true;
}

} // namespace

Request RequestParser::parse(const std::string &data, size_t max_head_bytes) {
    using Kind = Request::Kind;
    Request request;
    if (data.empty()) {
        return request;
    }
    if (data[0] == 0x04 || data[0] == 0x05) {
        request.kind = Kind::Socks;
        return request;
    }

    if (method_size_ == 0) {
        auto sp = data.find(' ', parsed_);
        auto end = sp == std::string::npos ? data.size() : sp;
        if (sp == 0 || !std::all_of(data.begin() + parsed_, data.begin() + end,
                                    IsTokenChar)) {
            request.kind = Kind::Invalid;
            return request;
        }
        parsed_ = end;
        if (sp == std::string::npos) {
            if (data.size() > max_head_bytes) {
                request.kind = Kind::Invalid;
            }
            return request;
        }
        method_size_ = sp;
        connect_ = data.compare(0, sp, "CONNECT") == 0;
        const char *stand_in = connect_ ? "CONNECT" : "POST";
        llhttp_execute(&parser_, stand_in, strlen(stand_in));
    }

    auto error =
        llhttp_execute(&parser_, data.data() + parsed_, data.size() - parsed_);
    parsed_ = data.size();
    if (error == HPE_OK) {
        if (data.size() > max_head_bytes) {
            request.kind = Kind::Invalid;
        }
        return request;
    }
    request.kind = Kind::Invalid;
    if (error != HPE_PAUSED) {
        return request;
    }
    head_done_ = true;
    request.head_size =
        static_cast<size_t>(llhttp_get_error_pos(&parser_) - data.data());

    // Taken out, as the parser lives on to frame the body.
    auto target = std::move(url_);
    auto headers = std::move(headers_);
    auto major = llhttp_get_http_major(&parser_);
    auto minor = llhttp_get_http_minor(&parser_);
    if (request.head_size > max_head_bytes || major != 1 || minor > 1 ||
        target.find('#') != std::string::npos) {
        return request;
    }
    if (connect_) {
        if (SplitHostPort(target, 0, request.host, request.port)) {
            request.kind = Kind::Connect;
        }
    } else if (RewriteForward(data.substr(0, method_size_), target,
                              minor ? "HTTP/1.1" : "HTTP/1.0", headers,
                              request)) {
        request.kind = Kind::Forward;
    }
    return request;
}

RequestParser::RequestParser() {
    static const llhttp_settings_t settings = [] {
        llhttp_settings_t settings;
        llhttp_settings_init(&settings);
        settings.on_url = [](llhttp_t *p, const char *at, size_t len) {
            static_cast<RequestParser *>(p->data)->url_.append(at, len);
            return 0;
        };
        settings.on_header_field = [](llhttp_t *p, const char *at, size_t len) {
            auto *self = static_cast<RequestParser *>(p->data);
            if (!self->head_done_) {
                self->field_.append(at, len);
            }
            return 0;
        };
        settings.on_header_value = [](llhttp_t *p, const char *at, size_t len) {
            auto *self = static_cast<RequestParser *>(p->data);
            if (!self->head_done_) {
                self->value_.append(at, len);
            }
            return 0;
        };
        settings.on_header_value_complete = [](llhttp_t *p) {
            auto *self = static_cast<RequestParser *>(p->data);
            if (!self->head_done_) {
                self->headers_.emplace_back(std::move(self->field_),
                                            std::move(self->value_));
                self->field_.clear();
                self->value_.clear();
            }
            return 0;
        };
        settings.on_headers_complete = [](llhttp_t *) {
            return static_cast<int>(HPE_PAUSED);
        };
        settings.on_message_complete = [](llhttp_t *) {
            return static_cast<int>(HPE_PAUSED);
        };
        return settings;
    }();
    llhttp_init(&parser_, HTTP_REQUEST, &settings);
    parser_.data = this;
}

RequestParser::Result RequestParser::feed(const char *data, size_t len,
                                          size_t &used) {
    used = 0;
    if (error_) {
        return Result::Error;
    }
    if (complete_) {
        return Result::Complete;
    }
    // Paused after the head; does nothing on later feeds.
    llhttp_resume(&parser_);
    auto error = llhttp_execute(&parser_, data, len);
    if (error == HPE_OK) {
        used = len;
        return Result::More;
    }
    // An upgrade request pauses after its head; the rewrite drops Upgrade,
    // so the head is the whole request.
    if (error == HPE_PAUSED || error == HPE_PAUSED_UPGRADE) {
        complete_ = true;
        used = static_cast<size_t>(llhttp_get_error_pos(&parser_) - data);
        return Result::Complete;
    }
    error_ = true;
    return Result::Error;
}

std::string Socks5Greeting() { return std::string("\x05\x01\x00", 3); }

std::string Socks5Connect(const std::string &host, int port) {
    std::string out("\x05\x01\x00\x03", 4);
    out.push_back(static_cast<char>(host.size()));
    out += host;
    out.push_back(static_cast<char>((port >> 8) & 0xff));
    out.push_back(static_cast<char>(port & 0xff));
    return out;
}

int Socks5GreetingReply(const std::string &data) {
    if (data.size() < 2) {
        return 0;
    }
    return data[0] == 0x05 && data[1] == 0x00 ? 2 : -1;
}

int Socks5ConnectReply(const std::string &data) {
    if (data.size() < 5) {
        return 0;
    }
    if (data[0] != 0x05 || data[1] != 0x00) {
        return -1;
    }
    size_t addr;
    switch (data[3]) {
    case 0x01:
        addr = 4;
        break;
    case 0x03:
        addr = 1 + static_cast<unsigned char>(data[4]);
        break;
    case 0x04:
        addr = 16;
        break;
    default:
        return -1;
    }
    size_t size = 4 + addr + 2;
    return data.size() < size ? 0 : static_cast<int>(size);
}

} // namespace proxy
} // namespace nexer
