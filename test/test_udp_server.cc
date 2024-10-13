#include <assert.h>

#include <thread>

#include "string_buffer.h"
#include "udp_server.h"

namespace nexer {
namespace test {

#define TEST_PORT 19001

static void OnSend(uv_udp_send_t* req, int status) {
    auto server = (UdpServer*)req->handle->data;
    server->Close();
    free(req->data);
    free(req);
}

static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

static void OnClose(uv_handle_t* handle) {
    assert(uv_is_closing(handle));
}

static void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
    if (nread == 0) {
        assert(addr == nullptr);
        return;
    }
    assert(nread > 0);
    assert(!memcmp("PING", buf->base, nread));
    free((char*)buf->base);
    uv_close((uv_handle_t*)handle, OnClose);
}

static void OnClientSend(uv_udp_send_t* req, int status) {
    int err = uv_udp_recv_start(req->handle, OnAlloc, OnRecv);
    assert(err == 0);
    free(req->data);
}

void TestUdpServer() {
    EventLoop loop;
    UdpServer& server = UdpServer::Create(loop);

    server.OnRecv([&server](const char* s, size_t len, const struct sockaddr* addr) {
        auto req = (uv_udp_send_t*)malloc(sizeof(uv_udp_send_t));
        auto copy = (char*)malloc(len);
        memcpy(copy, s, len);
        auto buf = uv_buf_init(copy, len);
        req->data = copy;
        uv_udp_send(req, (uv_udp_t*)server.handle(), &buf, 1, addr, OnSend);
    });

    server.Listen(TEST_PORT);

    struct sockaddr_in addr;
    assert(uv_ip4_addr("127.0.0.1", TEST_PORT, &addr) == 0);

    uv_udp_t client;
    assert(uv_udp_init((uv_loop_t*)loop, &client) == 0);

    char *data = (char*)malloc(4);
    memcpy(data, "PING", 4);

    auto buf = uv_buf_init(data, 4);
    uv_udp_send_t req;

    req.data = data;

    int err = uv_udp_send(&req, &client, &buf, 1, (const struct sockaddr*)&addr, OnClientSend);

    assert(err == 0);

    loop.Run();
}

}  // namespace test
}  // namespace nexer
