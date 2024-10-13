#ifndef NEXER_HTTP_CLIENT_H_
#define NEXER_HTTP_CLIENT_H_

#include <curl/curl.h>

#include "string_buffer.h"
#include "http_message.h"

namespace nexer {

class HttpClient {
  private:
    CURL *curl_;
    curl_slist *slist_;
    char error_buffer_[CURL_ERROR_SIZE];

    void Reset();

    int Execute();

  public:
    HttpClient();
    ~HttpClient();

    void SetFollow(bool);

    std::unique_ptr<http::Response> Get(const std::string &url);
};

}  // namespace nexer

#endif  // NEXER_HTTP_CLIENT_H_