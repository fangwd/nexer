#include <assert.h>

#include <thread>

#include "http_client.h"
#include "http_server.h"
#include "logger.h"
#include "string_buffer.h"

namespace nexer {
namespace test {

#define PORT 19001

static void test_keep_alive() {
    EventLoop loop;
    http::Server& server = http::Server::Create(loop);

    server.OnRequest([&server](http::incoming::Request& req, http::outgoing::Response& res) {
        res.body() << "Hello";
        res.End();

        if (req.url().path == "/shutdown") {
            server.Close();
        }
    });

    server.Listen(PORT);

    loop.Run();
}

const char* url_of(const char* path) {
    static char url[1024];
    snprintf(url, sizeof url, "http://localhost:%d%s", PORT, path);
    return url;
}

static void make_keep_alive_requests() {
    HttpClient client;
    auto res = client.Get(url_of("/hello"));
    assert(res->body() == "Hello");

    res = client.Get(url_of("/shutdown"));
    assert(res->body() == "Hello");
}

void TestHttpServer() {
    std::thread t1(test_keep_alive);
    make_keep_alive_requests();
    t1.join();
}

}  // namespace test
}  // namespace nexer
