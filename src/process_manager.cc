#include "process_manager.h"
#include <sstream>

namespace nexer {

ProcessManager::ProcessManager(EventLoop& loop) : loop_(loop) {}

ProcessManager::~ProcessManager() {}

static inline const char *ToString(int64_t status, int signal) {
    static char buf[128];
    if (signal != 0) {
        snprintf(buf, sizeof buf, "%lld (%s)", status, strsignal(signal));
    } else {
        snprintf(buf, sizeof buf, "%lld", status);
    }
    return buf;
}

void ProcessManager::Start(App& app) {
    Then<int> then([&](int error) {
        if (error != 0) {
            log_debug("Failed to start %s (preamble check failure)", str(app));
            ClearCallbacks(app, error);
            return;
        }

        auto& process = Process::Create(loop_, app.config->command);

        process.OnData([&](int fd, const char* s, size_t len) {
            on_process_data_.Invoke(&process, fd, s, len);
        });

        process.OnError([&](int error) {
            log_debug("Failed to run %s (%d)", str(app), error);
            app.process = nullptr;
            on_process_error_.Invoke(&process, error);
            ClearCallbacks(app, error);
        });

        process.OnExit([&](int64_t status, int signal) {
            log_debug("Exited %s %s (restart: %d)", str(app), ToString(status, signal), (int) app.restart);
            status = status ? status : signal;
            on_process_exit_.Invoke(&process, status, signal);
            app.process = nullptr;
            if (status != 0) {
                if (app.restart) {
                    app.restart = false;
                    Start(app);
                } else if (!app.config->checker) {
                    ClearCallbacks(app, status);
                }
            }
        });

        auto& timer = Timer::Create(loop_, 100);

        Then<int> then([&](int error) {
            uint64_t start_time = Timer::Now() - app.require_start_time;
            bool timeout = app.config->max_start_time > 0 && start_time > app.config->max_start_time;
            log_debug("Checked %s after start (error %d, timeout %d)", str(app), error, (int) timeout);
            if (error == 0 || error == -ENOENT || timeout) {
                log_debug("Completed starting %s (error %d, time %zu)", str(app), error, start_time);
                timer.Close();
                app.checker_timer = nullptr;
                ClearCallbacks(app, error);
            }
            app.checking = false;
        });

        timer.OnTick([&, then] {
            if (app.callbacks.size() == 0) {
                log_debug("No need to check %s after start", str(app));
                timer.Close();
            } else if (!app.checking) {
                log_debug("Checking %s after start", str(app));
                Check(*app.config, then);
                app.checking = true;
            }
        });
        app.process = &process;
        app.checker_timer = &timer;
        app.checking = false;
        timer.Start();
        log_debug("Starting process %s", str(app));
        process.Start();
        on_process_start_.Invoke(&process);
    });

    CheckPreamble(app, then);
}

void ProcessManager::ClearCallbacks(App& app, int error) {
    for (auto then : app.callbacks) {
        then(app.process, error);
    }
    app.callbacks.clear();
}

void ProcessManager::Require(const config::App& config, AfterProcessCheck then) {
    auto& app = GetApp(config);

    app.callbacks.push_back(then);
    log_debug("Requiring %s (waiting %zu)", str(config), app.callbacks.size());
    if (app.callbacks.size() > 1) {
        return;
    }

    Check(config, Then<int>([&](int error) {
        if (error == 0) {
            if (config.checker || app.process != nullptr) {
                ClearCallbacks(app, 0);
            } else {
                log_debug("Starting %s as no checker/process found", str(config));
                app.require_start_time = Timer::Now();
                Start(app);
            }
        } else {
            if (app.process != nullptr) {
                log_debug("Killing %s with restart after check error %d", str(config), error);
                app.restart = true;
                app.process->Kill();
            } else {
                log_debug("Starting %s after check error %d", str(config), error);
                app.require_start_time = Timer::Now();
                Start(app);
            }
        }
    }));
}

void ProcessManager::CheckPreamble(App& app, Then<int> then) {
    auto& preamble = app.config->preamble;

    app.error_preamble = 0;

    log_debug("Requiring %zu preamble(s) for %s", preamble.size(), str(app));
    if ((app.pending_preamble = preamble.size()) == 0) {
        then(0);
        return;
    }

    for (auto& config: preamble) {
        log_debug("Requiring %s for %s", str(*config), str(app));
        Require(*config, [&, then](Process* process, int error) {
            app.pending_preamble--;
            log_debug("Required %s for %s (error %d, %d more)", str(*config), str(app), error, app.pending_preamble);
            if (error != 0) {
                app.error_preamble++;
            }
            if (app.pending_preamble == 0) {
                then(app.error_preamble);
            }
        });
    }
}

void ProcessManager::Check(const config::App& app, Then<int> then) {
    if (!app.checker) {
        then(0);
        return;
    }

    log_debug("Running checker for %s (%s)", str(app), str(*app.checker));

    auto& process = Process::Create(loop_, *app.checker);

    process.OnData([&](int fd, const char* s, size_t len) {
        on_process_data_.Invoke(&process, fd, s, len);
    });

    process.OnError([&](int error) {
        log_debug("Checker for %s (%s) failed (error %d)", str(app), str(*app.checker), error);
        on_process_error_.Invoke(&process, error);
        then(error);
    });

    process.OnExit([&, then](int64_t status, int signal) {
        log_debug("Checker for %s (%s) exited %s", str(app), str(*app.checker), ToString(status, signal));
        status = status ? status : signal;
        on_process_exit_.Invoke(&process, status, signal);
        then(int(status));
    });

    process.Start();

    on_process_start_.Invoke(&process);
}

ProcessManager::App& ProcessManager::GetApp(const config::App& config) {
    auto it = app_map_.find(&config);
    if (it != app_map_.end()) {
        return it->second;
    }

    app_map_.emplace(&config, App({
        .config = &config,
        .process = nullptr,
        .pending_preamble = 0,
    }));

    return app_map_[&config];
}

const char *ProcessManager::str(const config::Command& cmd) {
    auto it = str_map_.find(&cmd);
    if (it != str_map_.end()) {
        return it->second->c_str();
    }

    std::stringstream ss;
    size_t n = 0;

    for (auto p: cmd.env) {
        if (n++ > 0) {
            ss << ' ';
        }
        ss << p;
    }

    if (cmd.env.size() > 0) {
        ss << ' ';
    }

    ss << cmd.file;

    for (auto p: cmd.args) {
        ss << ' ';
        ss << p;
    }

    auto str_ptr = std::make_shared<std::string>(ss.str());
    str_map_.emplace(&cmd, str_ptr);

    return str_ptr->c_str();

}

const char *ProcessManager::str(const config::App& app) {
    if (app.name.size() > 0) {
        return app.name.c_str();
    }
    return str(app.command);
}

const char *ProcessManager::str(const App& app) {
    return str(*app.config);
}

}  // namespace nexer
