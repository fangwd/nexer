#include "nexer/http/http_client.h"
#include "nexer/core/string_buffer.h"

#include <cctype>
#include <cstring>

namespace nexer {

namespace {

bool EqualsIgnoreCase(const std::string &lhs, const char *rhs) {
    if (!rhs) {
        return lhs.empty();
    }
    size_t rhs_len = std::strlen(rhs);
    if (lhs.size() != rhs_len) {
        return false;
    }
    for (size_t i = 0; i < rhs_len; ++i) {
        auto a = static_cast<unsigned char>(lhs[i]);
        auto b = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

static size_t http_content_callback(char *data, size_t size, size_t nmesb,
                                    void *p) {
    StringBuffer *sb = (StringBuffer *)p;
    size_t n = size * nmesb;
    sb->Write(data, n);
    return n;
}

static size_t http_header_callback(char *data, size_t size, size_t nmesb,
                                   void *p) {
    StringBuffer *sb = (StringBuffer *)p;
    size_t n = size * nmesb;

    if (n > 4 && data[0] == 'H' && data[1] == 'T' && data[2] == 'T' &&
        data[3] == 'P' && data[4] == '/') {
        sb->clear();
    }

    // The empty line that separates header and body will be appended too
    sb->Write(data, n);

    return n;
}

void HttpClient::reset() {
    curl_ = curl_easy_init();
    if (!curl_) {
        fprintf(stderr, "curl_easy_init() failed. Ensure curl_global_init has "
                        "been called before using HttpClient.\n");
        return;
    }

    curl_easy_setopt(curl_, CURLOPT_USERAGENT, "Nexer/1.0");

    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, http_content_callback);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, http_header_callback);

    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl_, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error_buffer_);

    curl_easy_setopt(curl_, CURLOPT_PRIVATE, this);
    curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 120L);
    set_default_headers();
}

void HttpClient::set_follow(bool follow) {
    if (!curl_) {
        return;
    }
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
}

HttpClient::HttpClient() : curl_(nullptr), slist_(nullptr) { reset(); }

HttpClient::~HttpClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
    curl_slist_free_all(slist_);
}

int HttpClient::execute() {
    if (!curl_) {
        return static_cast<int>(CURLE_FAILED_INIT);
    }
    auto res = curl_easy_perform(curl_);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(res));
    }

    return (int)res;
}

std::unique_ptr<http::Response> HttpClient::request(const std::string &method,
                                                    const std::string &url,
                                                    const std::string &body) {
    auto res = std::make_unique<http::Response>();

    if (!curl_) {
        res->set_status(static_cast<int>(CURLE_FAILED_INIT));
        return res;
    }

    custom_method_ = method;
    if (custom_method_.empty()) {
        custom_method_ = "GET";
    }

    if (body.empty()) {
        request_body_.clear();
    } else {
        request_body_ = body;
    }

    bool is_get = EqualsIgnoreCase(custom_method_, "GET");

    curl_easy_setopt(curl_, CURLOPT_URL, url.data());
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &res->body());
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &res->head());
    curl_easy_setopt(curl_, CURLOPT_HTTPGET, is_get ? 1L : 0L);
    curl_easy_setopt(curl_, CURLOPT_POST, 0L);
    curl_easy_setopt(curl_, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, custom_method_.c_str());

    if (!request_body_.empty()) {
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS,
                         const_cast<char *>(request_body_.data()));
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, request_body_.size());
    } else {
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0L);
    }

    int err = execute();
    if (err == 0) {
        long rc;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &rc);
        res->set_status((int)rc);
    } else {
        res->set_status(err);
    }

    return res;
}

std::unique_ptr<http::Response> HttpClient::get(const std::string &url) {
    return request("GET", url);
}

void HttpClient::set_default_headers() {
    if (!curl_) {
        return;
    }
    if (slist_) {
        curl_slist_free_all(slist_);
        slist_ = nullptr;
    }
    slist_ = curl_slist_append(slist_, "Accept-Language: en-US,en;q=0.5");
    slist_ = curl_slist_append(slist_, "Cache-Control: max-age=0");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, slist_);
}

void HttpClient::clear_headers() { set_default_headers(); }

void HttpClient::add_header(const std::string &header) {
    if (!curl_) {
        return;
    }
    slist_ = curl_slist_append(slist_, header.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, slist_);
}

} // namespace nexer
