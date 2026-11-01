#include <assert.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "nexer/core/url.h"

namespace nexer {
namespace test {

static void TestDecodeUrlComponent() {
    {
        char dst[64];
        size_t len = decode_url_component("abc", dst);
        assert(len == 3);
        assert(strcmp(dst, "abc") == 0);
    }
    {
        char dst[64];
        size_t len = decode_url_component("a+b+c", dst);
        assert(len == 5);
        assert(strcmp(dst, "a b c") == 0);
    }
    {
        char dst[64];
        size_t len = decode_url_component("%41%7a%4A", dst);
        assert(len == 3);
        assert(strcmp(dst, "AzJ") == 0);
    }
    {
        char dst[64];
        size_t len = decode_url_component("%ZZ%1", dst);
        assert(len == 5);
        assert(strcmp(dst, "%ZZ%1") == 0);
    }
    {
        char dst[8];
        size_t len = decode_url_component("%00X", dst);
        assert(len == 2);
        assert(dst[0] == '\0');
        assert(dst[1] == 'X');
        assert(dst[2] == '\0');
    }
}

static void TestEncodeUrlComponent() {
    {
        char dst[64];
        size_t len = encode_url_component("abc", dst);
        assert(len == 3);
        assert(strcmp(dst, "abc") == 0);
    }
    {
        char dst[64];
        size_t len = encode_url_component("a b c", dst);
        assert(len == 5);
        assert(strcmp(dst, "a+b+c") == 0);
    }
    {
        char dst[64];
        size_t len = encode_url_component("-_.~", dst);
        assert(len == 4);
        assert(strcmp(dst, "-_.~") == 0);
    }
    {
        char dst[64];
        size_t len = encode_url_component("A+Z/", dst);
        assert(len == 8);
        assert(strcmp(dst, "A%2BZ%2F") == 0);
    }
}

static void TestParseUrl() {
    {
        Url url;
        parse_url(url, "http://example.com:8080/path/to?query=1#frag");
        assert(url.scheme == "http");
        assert(url.host == "example.com");
        assert(url.port == "8080");
        assert(url.path == "/path/to");
        assert(url.query == "query=1");
        assert(url.fragment == "frag");
    }
    {
        Url url;
        parse_url(url, "http://example.com");
        assert(url.scheme == "http");
        assert(url.host == "example.com");
        assert(url.port.empty());
        assert(url.path.empty());
        assert(url.query.empty());
        assert(url.fragment.empty());
    }
    {
        Url url;
        parse_url(url, "http://example.com?x=1");
        assert(url.scheme == "http");
        assert(url.host == "example.com");
        assert(url.path == "/");
        assert(url.query == "x=1");
    }
    {
        Url url;
        parse_url(url, "/only/path");
        assert(url.scheme.empty());
        assert(url.host.empty());
        assert(url.port.empty());
        assert(url.path == "/only/path");
        assert(url.query.empty());
        assert(url.fragment.empty());
    }
    {
        Url url;
        parse_url(url, "example.com/path");
        assert(url.scheme.empty());
        assert(url.host.empty());
        assert(url.port.empty());
        assert(url.path == "/example.com/path");
    }
    {
        Url url;
        parse_url(url, "");
        assert(url.scheme.empty());
        assert(url.host.empty());
        assert(url.port.empty());
        assert(url.path.empty());
        assert(url.query.empty());
        assert(url.fragment.empty());
    }
    {
        Url url;
        parse_url(url, "http://example.com#frag?x=1");
        assert(url.scheme == "http");
        assert(url.host == "example.com");
        assert(url.path == "/");
        assert(url.query.empty());
        assert(url.fragment == "frag?x=1");
    }
}

static void ParseQuery(const std::string &qs,
                       std::vector<std::pair<std::string, std::string>> &out) {
    parse_query_string(qs,
                       [&](const std::string &key, const std::string &value) {
                           out.emplace_back(key, value);
                           return 0;
                       });
}

static void TestParseQueryString() {
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("a=1&b=2", out);
        assert(out.size() == 2);
        assert(out[0].first == "a");
        assert(out[0].second == "1");
        assert(out[1].first == "b");
        assert(out[1].second == "2");
    }
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("a=&=b&c", out);
        assert(out.size() == 3);
        assert(out[0].first == "a");
        assert(out[0].second.empty());
        assert(out[1].first.empty());
        assert(out[1].second == "b");
        assert(out[2].first == "c");
        assert(out[2].second.empty());
    }
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("a=1&", out);
        assert(out.size() == 1);
        assert(out[0].first == "a");
        assert(out[0].second == "1");
    }
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("&&", out);
        assert(out.empty());
    }
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("a", out);
        assert(out.size() == 1);
        assert(out[0].first == "a");
        assert(out[0].second.empty());
    }
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("a=b=c", out);
        assert(out.size() == 1);
        assert(out[0].first == "a");
        assert(out[0].second == "b=c");
    }
    {
        std::vector<std::pair<std::string, std::string>> out;
        ParseQuery("", out);
        assert(out.empty());
    }
}

void TestUrl() {
    TestDecodeUrlComponent();
    TestEncodeUrlComponent();
    TestParseUrl();
    TestParseQueryString();
}

} // namespace test
} // namespace nexer
