#ifndef _NEXER_URL_H
#define _NEXER_URL_H

#include <functional>
#include <string>

struct Url {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    std::string fragment;
};

void parse_url(Url &, const char *);

void parse_query_string(
    const std::string &,
    std::function<int(const std::string &, const std::string &)>);

size_t decode_url_component(const char *src, char *dst);
size_t encode_url_component(const char *src, char *dst);

#endif
