/**
 * Complete example of an asynchronous HTTP server using nexer_http.
 * This example demonstrates:
 * 1. Safe lifetime management using std::shared_ptr and std::weak_ptr.
 * 2. Header checking and HTTP error handling.
 * 3. Offloading CPU-intensive or blocking tasks using nexer::AsyncWork.
 * 4. Responding asynchronously from a background thread.
 */

#include "nexer/core/async_work.h"
#include "nexer/core/logger.h"
#include "nexer/http/http_server.h"
#include <algorithm>
#include <iostream>

using namespace nexer;

int main() {
    // 1. Create the event loop
    EventLoop loop;

    // 2. Create the HTTP server instance
    // Note: Create returns a std::shared_ptr<http::Server>
    auto server = http::Server::create(loop);

    // 3. Register the request handler
    // It takes a shared_ptr to the incoming request and a reference to the
    // response object.
    server->on_request([](std::shared_ptr<http::incoming::Request> req,
                          http::outgoing::Response &res) {
        log_info("Received request: %s", req->url().path.c_str());

        // --- Example A: Basic Header Checking & Early Error Return ---
        bool has_auth = false;
        for (const auto &header : req->headers()) {
            // Case-insensitive check for Authorization header
            std::string key = header.key;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);

            if (key == "authorization") {
                has_auth = true;
                log_debug("Auth found: %s", header.value.c_str());
                break;
            }
        }

        // Simulating a requirement for an Auth header on /private
        if (req->url().path == "/private" && !has_auth) {
            res.body() << "Unauthorized access to private area.";
            res.end(401); // Returns HTTP 401 Unauthorized
            return;
        }

        // --- Example B: Asynchronous Background Work ---
        // If the path is /compute, we simulate a heavy task using AsyncWork.
        if (req->url().path == "/compute") {
            // It is CRITICAL to capture a weak_ptr to the request.
            // If the client disconnects before the background work finishes,
            // the shared_ptr refcount would otherwise keep the connection alive
            // unnecessarily, or we might try to write to a closed socket.
            std::weak_ptr<http::incoming::Request> weak_req = req;

            // Create an AsyncWork object managed by shared_ptr
            auto work = AsyncWork::Create(req->server().loop());

            // The 'Start' callback runs in a background thread from the libuv
            // thread pool.
            work->Start([weak_req]() {
                // [Background Thread]
                // Simulate heavy computation (e.g., image processing, complex
                // DB query)
                uv_sleep(2000);

                log_debug("Computation finished in background thread");
            });

            // The 'OnEnd' callback runs back on the main EventLoop thread.
            work->OnEnd([weak_req](int status) {
                // [Main Loop Thread]
                // 1. Check if the request object is still alive
                if (auto req_locked = weak_req.lock()) {
                    // 2. Safety: Ensure the client connection hasn't been
                    // closed
                    req_locked->response().body()
                        << "Computation result: 42 (status: " << status << ")";
                    req_locked->response().end(200);
                    log_info("Sent async response for /compute");
                } else {
                    log_info("Request gone, discarding compute result");
                }
            });

            // Return from the lambda now. The response will be sent later in
            // OnEnd.
            return;
        }

        // --- Example C: Simple Synchronous Response ---
        if (req->url().path == "/") {
            res.set_header("Content-Type", "text/plain");
            res.body() << "Welcome to the Nexer HTTP Server!\n";
            res.body()
                << "Try /compute for async work, /private for auth check, "
                   "/stream for queued writes, or /chunked for chunked "
                   "transfer.";
            res.end(200);
        } else if (req->url().path == "/stream") {
            // Demonstrate queued writes. End() sends headers plus queued chunks
            // safely.
            res.set_header("Content-Type", "text/plain");
            res.write_body("chunk-", 6);
            res.write_body("one-", 4);
            res.write_body("two\n", 4);
            res.end(200);
        } else if (req->url().path == "/chunked") {
            // Demonstrate true chunked transfer with a trailer.
            res.set_header("Content-Type", "text/plain");
            res.begin_chunked_encoding();
            res.write_chunk("alpha");
            res.write_chunk("-");
            res.write_chunk("beta");
            res.write_trailer("X-Chunked: yes");
            res.end_chunk_encoding();
        } else {
            // Default 404 Not Found
            res.body() << "Path not found: " << req->url().path;
            res.end(404);
        }
    });

    // 4. Start listening on port 8080
    int port = 8080;
    if (server->Listen(port)) {
        std::cout << "HTTP Server listening on http://localhost:" << port
                  << std::endl;
        // 5. Run the event loop
        loop.run();
    } else {
        std::cerr << "Failed to listen on port " << port << std::endl;
        return 1;
    }

    return 0;
}
