#ifndef NEXER_HTTP_CLIENT_H_
#define NEXER_HTTP_CLIENT_H_

#include <curl/curl.h>

#include <string>

#include "nexer/http/http_message.h"

namespace nexer {

class HttpClient {
  private:
    CURL *curl_;
    curl_slist *slist_;
    char error_buffer_[CURL_ERROR_SIZE];
    std::string custom_method_;
    std::string request_body_;

    void reset();
    void set_default_headers();

    int execute();

  public:
    HttpClient();
    ~HttpClient();

    void set_follow(bool);
    void clear_headers();
    void add_header(const std::string &header);

    std::unique_ptr<http::Response> request(const std::string &method,
                                            const std::string &url,
                                            const std::string &body = "");
    std::unique_ptr<http::Response> get(const std::string &url);
};

} // namespace nexer

#endif // NEXER_HTTP_CLIENT_H_
