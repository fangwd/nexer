#include "nexer/runtime/nexer.h"
#include "nexer/core/tcp_forwarder.h"
#include "nexer/core/udp_server.h"
#include "nexer/runtime/admin_server.h"
#include <algorithm>
#include <assert.h>
#include <utility>

namespace nexer {

static constexpr uint64_t kDrainPollMs = 50;

std::shared_ptr<Nexer::DummyServer>
Nexer::DummyServer::CreateTcp(EventLoop &loop, int port, bool echo) {
    auto server = TcpServer::Create(loop);
    auto dummy = std::make_shared<DummyServer>();
    std::weak_ptr<DummyServer> weak_dummy = dummy;
    server->OnConnection([weak_dummy, echo](std::shared_ptr<TcpClient> client) {
        if (auto dummy = weak_dummy.lock()) {
            dummy->Track(std::move(client), echo);
        } else {
            client->close();
        }
    });
    if (!server->Listen(port)) {
        server->close();
        return nullptr;
    }
    dummy->handle_ = std::static_pointer_cast<Handle>(server);
    return dummy;
}

std::shared_ptr<Nexer::DummyServer>
Nexer::DummyServer::CreateUdp(EventLoop &loop, int port) {
    auto server = UdpServer::create(loop);
    std::weak_ptr<UdpServer> weak_server = server;
    server->on_recv(
        [weak_server](const char *s, size_t len, const struct sockaddr *) {
            if (auto server = weak_server.lock()) {
                log_debug("DD: %.*s", (int)len, s);
            }
        });
    if (!server->listen(port)) {
        server->close();
        return nullptr;
    }
    auto dummy = std::make_shared<DummyServer>();
    dummy->handle_ = std::static_pointer_cast<Handle>(server);
    return dummy;
}

void Nexer::DummyServer::Track(std::shared_ptr<TcpClient> client, bool echo) {
    std::weak_ptr<DummyServer> weak_self = shared_from_this();
    if (echo) {
        auto forwarder = TcpForwarder::create(client, client);
        std::weak_ptr<TcpForwarder> weak_forwarder = forwarder;
        forwarder->on_close([weak_self, weak_forwarder] {
            auto self = weak_self.lock();
            auto forwarder = weak_forwarder.lock();
            if (self && forwarder) {
                self->forwarders_.erase(forwarder);
            }
        });
        forwarders_.insert(std::move(forwarder));
        return;
    }
    std::weak_ptr<TcpClient> weak_client = client;
    client->on_close([weak_self, weak_client] {
        auto self = weak_self.lock();
        auto client = weak_client.lock();
        if (self && client) {
            self->clients_.erase(client);
        }
    });
    clients_.insert(std::move(client));
}

void Nexer::DummyServer::Close() {
    if (handle_) {
        handle_->close();
        handle_.reset();
    }
    auto forwarders = std::move(forwarders_);
    forwarders_.clear();
    for (auto &forwarder : forwarders) {
        forwarder->shutdown();
    }
    auto clients = std::move(clients_);
    clients_.clear();
    for (auto &client : clients) {
        if (!client->is_closing()) {
            client->close();
        }
    }
}

Nexer::Nexer(Config &config) : config_(config) {
    process_manager_ = new ProcessManager(loop_);
    process_manager_->set_force_after_ms(
        config_.admin().shutdown.force_after_ms);
    process_manager_->OnProcessData(
        [](std::shared_ptr<Process>, int, const char *s, size_t len) {
            printf("%.*s", (int)len, s);
            fflush(stdout);
        });
    admin_server_ =
        std::make_unique<AdminServer>(loop_, config_, *process_manager_);
    admin_server_->set_runtime(this);
}

Nexer::~Nexer() {
    close();
    delete process_manager_;
}

int Nexer::run() {
    if (!loop_.is_initialised()) {
        log_error("nexer: event loop initialization failed");
        close();
        return 1;
    }

    if (admin_server_ && !admin_server_->start()) {
        close();
        return 1;
    }

    for (auto &config : config_.proxies()) {
        auto proxy = TcpProxy::create(
            loop_, config, process_manager_,
            static_cast<size_t>(config_.forwarder().max_buffer_bytes));
        if (!proxy->Listen(config.bind, config.port)) {
            proxy->close();
            close();
            return 1;
        }
        proxies_.push_back(proxy);
    }
    for (auto &config : config_.dummies()) {
        if (!start_dummy_server(config)) {
            close();
            return 1;
        }
    }

    if (!install_signal_handlers()) {
        close();
        return 1;
    }

    collect_required_apps();
    start_apps();

    loop_running_ = true;
    loop_.run();
    loop_running_ = false;
    close();
    return exit_code_;
}

bool Nexer::install_signal_handlers() {
    int status = uv_signal_init(loop_, &sigint_);
    if (status == 0) {
        status = uv_signal_init(loop_, &sigterm_);
    }
    if (status != 0) {
        log_error("nexer: uv_signal_init: %s", uv_strerror(status));
        return false;
    }
    sigint_.data = this;
    sigterm_.data = this;
    signals_installed_ = true;
    if ((status = uv_signal_start(&sigint_, on_signal, SIGINT)) != 0 ||
        (status = uv_signal_start(&sigterm_, on_signal, SIGTERM)) != 0) {
        log_error("nexer: uv_signal_start: %s", uv_strerror(status));
        return false;
    }
    return true;
}

void Nexer::on_signal(uv_signal_t *handle, int signum) {
    auto *self = static_cast<Nexer *>(handle->data);
    const char *name = signum == SIGINT ? "SIGINT" : "SIGTERM";
    ShutdownOptions options;
    options.grace_ms = self->config_.admin().shutdown.default_grace_ms;
    options.force_after_ms = self->config_.admin().shutdown.force_after_ms;
    options.reason = name;
    if (!self->request_shutdown(std::move(options))) {
        log_info("nexer: %s received, shutdown already in progress", name);
    }
}

void Nexer::close_signal_handlers() {
    if (!signals_installed_) {
        return;
    }
    signals_installed_ = false;
    for (auto *handle : {&sigint_, &sigterm_}) {
        if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(handle))) {
            uv_signal_stop(handle);
            uv_close(reinterpret_cast<uv_handle_t *>(handle), nullptr);
        }
    }
}

// Required apps are the auto_start roots plus every transitive dependency.
void Nexer::collect_required_apps() {
    std::vector<const config::App *> stack;
    for (auto *app : config_.apps()) {
        if (app && app->auto_start) {
            stack.push_back(app);
        }
    }
    while (!stack.empty()) {
        auto *app = stack.back();
        stack.pop_back();
        if (std::find(required_apps_.begin(), required_apps_.end(), app) !=
            required_apps_.end()) {
            continue;
        }
        required_apps_.push_back(app);
        for (auto *dependency : app->preamble) {
            stack.push_back(dependency);
        }
    }
}

void Nexer::start_apps() {
    struct Startup {
        size_t pending = 0;
        uint64_t started_at_ms = Timer::now();
        bool finished = false;
    };
    auto startup = std::make_shared<Startup>();

    std::vector<const config::App *> roots;
    for (auto *app : config_.apps()) {
        if (app && app->auto_start) {
            roots.push_back(app);
        }
    }
    startup->pending = roots.size();
    if (roots.empty()) {
        finish_startup(0);
        return;
    }

    auto timeout_ms = config_.startup_timeout_ms();
    if (timeout_ms > 0) {
        startup_timer_ = Timer::create(loop_, timeout_ms);
        std::weak_ptr<Timer> weak_timer = startup_timer_;
        startup_timer_->on_tick([this, startup, weak_timer] {
            if (auto timer = weak_timer.lock()) {
                timer->close();
            }
            startup_timer_.reset();
            if (startup->finished) {
                return;
            }
            startup->finished = true;
            std::string pending;
            for (auto *app : required_apps_) {
                auto snapshot = process_manager_->snapshot(*app);
                if (snapshot.observed != ObservedState::Ready) {
                    if (!pending.empty()) {
                        pending += ", ";
                    }
                    pending += app->name.empty() ? "<unnamed>" : app->name;
                    pending += " (";
                    pending += to_string(snapshot.observed);
                    pending += ")";
                }
            }
            log_error("nexer: startup timed out after %llums; not ready: %s",
                      (unsigned long long)config_.startup_timeout_ms(),
                      pending.c_str());
            fail_startup("startup timeout");
        });
        startup_timer_->start(false);
    }

    // The first terminal failure fails startup; waiting for unrelated roots
    // could otherwise take forever, and later roots must not be launched.
    for (auto *app : roots) {
        if (startup->finished) {
            break;
        }
        process_manager_->Require(
            *app, [this, app, startup](std::shared_ptr<Process>, int error) {
                if (startup->finished) {
                    return;
                }
                const char *label =
                    app->name.empty() ? "<unnamed>" : app->name.c_str();
                if (error != 0) {
                    startup->finished = true;
                    if (startup_timer_) {
                        startup_timer_->close();
                        startup_timer_.reset();
                    }
                    log_error("nexer: auto-start failed for %s (%s)", label,
                              error_string(error).c_str());
                    log_error("nexer: startup failed");
                    fail_startup("startup failure");
                    return;
                }
                if (--startup->pending == 0) {
                    startup->finished = true;
                    if (startup_timer_) {
                        startup_timer_->close();
                        startup_timer_.reset();
                    }
                    finish_startup(Timer::now() - startup->started_at_ms);
                }
            });
    }
}

void Nexer::finish_startup(uint64_t elapsed_ms) {
    if (shutdown_requested_) {
        return;
    }
    startup_complete_ = true;
    if (required_apps_.empty()) {
        log_info("nexer: ready (no auto_start apps)");
    } else {
        log_info("nexer: ready (%zu apps in %llums)", required_apps_.size(),
                 (unsigned long long)elapsed_ms);
    }
    if (on_ready_) {
        on_ready_();
    }
}

void Nexer::fail_startup(const char *reason) {
    startup_failed_ = true;
    exit_code_ = 1;
    ShutdownOptions options;
    options.force_after_ms = config_.admin().shutdown.force_after_ms;
    options.reason = reason;
    request_shutdown(std::move(options));
}

bool Nexer::request_shutdown(ShutdownOptions options) {
    if (shutdown_requested_) {
        return false;
    }
    shutdown_requested_ = true;
    log_info("nexer: shutdown requested (%s, delay %llums, grace %llums, "
             "force after %llums)",
             options.reason.empty() ? "unspecified" : options.reason.c_str(),
             (unsigned long long)options.delay_ms,
             (unsigned long long)options.grace_ms,
             (unsigned long long)options.force_after_ms);

    // Always defer at least one loop iteration so an HTTP shutdown response
    // is flushed before listeners close.
    shutdown_timer_ = Timer::create(loop_, options.delay_ms);
    std::weak_ptr<Timer> weak_timer = shutdown_timer_;
    shutdown_timer_->on_tick([this, options, weak_timer] {
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        shutdown_timer_.reset();
        begin_shutdown(options);
    });
    shutdown_timer_->start(false);
    return true;
}

void Nexer::begin_shutdown(ShutdownOptions options) {
    shutting_down_ = true;
    if (startup_timer_) {
        startup_timer_->close();
        startup_timer_.reset();
    }
    // Stop accepting proxy traffic; existing connections drain below.
    for (auto &proxy : proxies_) {
        proxy->close();
    }
    drain_proxies(std::move(options));
}

void Nexer::drain_proxies(ShutdownOptions options) {
    auto active = [this] {
        size_t count = 0;
        for (auto &proxy : proxies_) {
            count += proxy->connections();
        }
        return count;
    };
    if (options.grace_ms == 0 || active() == 0) {
        stop_processes(std::move(options));
        return;
    }
    log_info("nexer: draining %zu proxy connections (up to %llums)", active(),
             (unsigned long long)options.grace_ms);
    auto deadline = Timer::now() + options.grace_ms;
    drain_timer_ = Timer::create(loop_, kDrainPollMs);
    std::weak_ptr<Timer> weak_timer = drain_timer_;
    drain_timer_->on_tick([this, options, active, deadline, weak_timer] {
        if (active() > 0 && Timer::now() < deadline) {
            return;
        }
        if (auto timer = weak_timer.lock()) {
            timer->close();
        }
        drain_timer_.reset();
        stop_processes(options);
    });
    drain_timer_->start();
}

void Nexer::stop_processes(ShutdownOptions options) {
    for (auto &proxy : proxies_) {
        proxy->shutdown();
    }
    process_manager_->set_force_after_ms(options.force_after_ms);
    process_manager_->Shutdown([this](bool clean) { finish_shutdown(clean); });
}

void Nexer::finish_shutdown(bool clean) {
    if (!clean) {
        log_error("nexer: owned child processes remain after shutdown");
        exit_code_ = 1;
    }
    close_listeners();
    close_signal_handlers();
    if (exit_code_ == 0) {
        log_info("nexer: shutdown complete");
    } else {
        log_error("nexer: shutdown complete with errors (exit %d)", exit_code_);
    }
    loop_.stop();
}

void Nexer::close_listeners() {
    if (admin_server_) {
        admin_server_->stop();
    }
    for (auto it = dummy_servers_.rbegin(); it != dummy_servers_.rend(); ++it) {
        (*it)->Close();
    }
    dummy_servers_.clear();
    for (auto it = proxies_.rbegin(); it != proxies_.rend(); ++it) {
        (*it)->shutdown();
        (*it)->close();
    }
    proxies_.clear();
}

Readiness Nexer::readiness() {
    Readiness readiness;
    readiness.startup_complete = startup_complete_;
    readiness.shutting_down = shutdown_requested_;
    for (auto *app : required_apps_) {
        auto snapshot = process_manager_->snapshot(*app);
        bool completed_task = app->type == config::App::Type::Task &&
                              snapshot.observed == ObservedState::Completed;
        bool satisfied =
            snapshot.desired == DesiredState::Running &&
            (snapshot.observed == ObservedState::Ready || completed_task);
        if (!satisfied) {
            readiness.blocking.push_back(snapshot);
        }
    }
    readiness.ready =
        startup_complete_ && !shutdown_requested_ && readiness.blocking.empty();
    return readiness;
}

// `close()` can safely be called multiple times; each cleanup path
// guards its resources, so later invocations become no-ops.
void Nexer::close() {
    for (auto *timer : {&startup_timer_, &shutdown_timer_, &drain_timer_}) {
        if (*timer) {
            (*timer)->close();
            timer->reset();
        }
    }
    close_listeners();
    close_signal_handlers();
    if (process_manager_ && !loop_running_) {
        process_manager_->Shutdown();
    }
}

bool Nexer::start_dummy_server(config::Dummy &config) {
    std::shared_ptr<DummyServer> server;
    switch (config.type) {
    case config::Dummy::Type::Udp:
        server = DummyServer::CreateUdp(loop_, config.port);
        break;
    case config::Dummy::Type::Tcp:
        server = DummyServer::CreateTcp(loop_, config.port, config.echo);
        break;
    default:
        log_error("nexer: unsupported dummy server type");
        return false;
    }
    if (!server) {
        return false;
    }
    dummy_servers_.push_back(std::move(server));
    return true;
}

} // namespace nexer
