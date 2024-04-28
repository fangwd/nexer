/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_PROCESS_MANAGER_H_
#define NEXER_PROCESS_MANAGER_H_

#include "process.h"
#include "config.h"
#include <map>

namespace nexer {

typedef std::function<void(Process*, int)> AfterProcessCheck;
class ProcessManager {
    struct App {
        const config::App *config;
        Process *process;
        std::vector<AfterProcessCheck> callbacks;
        bool restart;
        int pending_preamble;
        int error_preamble;
        uint64_t require_start_time;
        Timer *checker_timer;
        bool checking;
    };

  private:
    EventLoop& loop_;
    std::map<const config::App*, App> app_map_;
    std::map<const void*, std::shared_ptr<std::string>> str_map_;

    FunctionList<void, Process*> on_process_start_;
    FunctionList<void, Process*, int> on_process_error_;
    FunctionList<void, Process*, int, const char *, size_t> on_process_data_;
    FunctionList<void, Process*, int64_t, int> on_process_exit_;

    App& GetApp(const config::App& config);

    void Start(App& app);
    void CheckPreamble(App& app, Then<int> then);
    void Check(const config::App& app, Then<int> then);
    void ClearCallbacks(App&, int error);

    const char *str(const config::Command&);
    const char *str(const config::App&);
    const char *str(const App&);

  public:
    ProcessManager(EventLoop& loop);
    ~ProcessManager();

    void Require(const config::App& config, AfterProcessCheck then);

    inline auto OnProcessStart(std::function<void(const Process*)> fn) {
        return on_process_start_.Add(fn);
    }

    inline auto OnProcessError(std::function<void(const Process*, int)> fn) {
        return on_process_error_.Add(fn);
    }

    inline auto OnProcessData(std::function<void(const Process*, int, const char*, size_t)> fn) {
        return on_process_data_.Add(fn);
    }

    inline auto OnProcessExit(std::function<void(const Process*, int64_t status, int signal)> fn) {
        return on_process_exit_.Add(fn);
    }
};

}  // namespace nexer

#endif  // NEXER_PROCESS_MANAGER_H_
