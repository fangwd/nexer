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

void ParseUrl(Url&, const char *);

void ParseQueryString(const std::string&, std::function<int(const std::string&, const std::string&)>);

size_t DecodeUrlComponent(const char *src, char *dst);

#endif
