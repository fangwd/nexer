/* Copyright (c) Weidong Fang. All rights reserved. */

#include "url.h"
#include <ctype.h>

size_t DecodeUrlComponent(const char *src, char *dst) {
    size_t len = 0;
    for (; *src; src++, dst++, len++) {
        if (*src == '%' && isxdigit(src[1]) && isxdigit(src[2])) {
            char x = src[1] < 'A' ? src[1] - '0' : toupper(src[1]) - 'A' + 10;
            char y = src[2] < 'A' ? src[2] - '0' : toupper(src[2]) - 'A' + 10;
            *dst = (x << 4) + y;
            src += 2;
        } else if (*src == '+') {
            *dst = ' ';
        } else {
            *dst = *src;
        }
    }
    *dst = '\0';
    return len;
}

static void clear(Url &url) {
    url.scheme.clear();
    url.host.clear();
    url.port.clear();
    url.path.clear();
    url.query.clear();
    url.fragment.clear();
}

void ParseUrl(Url &url, const char *s) {
    clear(url);

    const char *p = strstr(s, "://");
    if (p) {
        url.scheme.append(s, p - s);

        s = p + 3;

        while (*s && *s != ':' && *s != '/' && *s != '?' && *s != '#') {
            url.host.push_back(*s++);
        }

        if (*s == ':') {
            s++;
            while (*s && *s != '/' && *s != '?' && *s != '#') {
                url.port.push_back(*s++);
            }
        }
    }

    if (*s && *s != '/') {
        url.path.push_back('/');
    }

    while (*s && *s != '?' && *s != '#') {
        url.path.push_back(*s++);
    }

    if (*s == '?') {
        s++;
        while (*s && *s != '#') {
            url.query.push_back(*s++);
        }
    }

    if (*s == '#') {
        s++;
        while (*s) {
            url.fragment.push_back(*s++);
        }
    }
}

bool operator==(const Url &a, const Url &b) {
    return a.scheme == b.scheme && a.host == b.host && a.port == b.port && a.path == b.path && a.query == b.query &&
           a.fragment == b.fragment;
}

void ParseQueryString(const std::string &qs, std::function<int(const std::string&, const std::string&)> cb) {
    const char *p = qs.data();
    const char *q = qs.data() + qs.size();
    std::string key, value;

    auto run_cb = [&]() {
        if (key.size() > 0 || value.size() > 0) {
            cb(key, value);
        }
        key.clear();
        value.clear();
    };

    // 0: key, 1: value
    int state = 0;

    while (p < q) {
        char c = *p++;
        if (state == 0) {
            if (c == '=') {
                state = 1;
            } else if (c == '&') {
                run_cb();
            } else {
                key.push_back(c);
            }
        } else {
            if (c == '&') {
                run_cb();
                state = 0;
            } else {
                value.push_back(c);
            }
        }
    }

    run_cb();
}