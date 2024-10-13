#include "http_client.h"
#include "string_buffer.h"

namespace nexer {

static size_t http_content_callback(char *data, size_t size, size_t nmesb, void *p) {
    StringBuffer *sb = (StringBuffer *)p;
    size_t n = size * nmesb;
    sb->Write(data, n);
    return n;
}

static size_t http_header_callback(char *data, size_t size, size_t nmesb, void *p) {
    StringBuffer *sb = (StringBuffer *)p;
    size_t n = size * nmesb;

    if (n > 4 && data[0] == 'H' && data[1] == 'T' && data[2] == 'T' && data[3] == 'P' && data[4] == '/') {
        sb->clear();
    }

    // The empty line that separates header and body will be appended too
    sb->Write(data, n);

    return n;
}

void HttpClient::Reset() {
    curl_ = curl_easy_init();

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

    slist_ = curl_slist_append(slist_, "Accept-Language: en-US,en;q=0.5");
    slist_ = curl_slist_append(slist_, "Cache-Control: max-age=0");
}

void HttpClient::SetFollow(bool follow) {
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
}

HttpClient::HttpClient() : curl_(nullptr), slist_(nullptr) {
    Reset();
}

HttpClient::~HttpClient() {
    curl_easy_cleanup(curl_);
    curl_slist_free_all(slist_);
}

int HttpClient::Execute() {
    auto res = curl_easy_perform(curl_);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }

    return (int)res;
}

std::unique_ptr<http::Response> HttpClient::Get(const std::string &url) {
    auto res = std::make_unique<http::Response>();

    curl_easy_setopt(curl_, CURLOPT_URL, url.data());
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &res->body());
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &res->head());

    int err = Execute();
    if (err == 0) {
        long rc;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &rc);
        res->SetStatus((int)rc);
    } else {
        res->SetStatus(err);
    }

    return res;
}

}  // namespace nexer
