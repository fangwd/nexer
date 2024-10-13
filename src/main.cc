#include <assert.h>

#include <string>
#include <thread>

#include "nexer.h"
#include "logger.h"
#include "process.h"
#include "timer.h"
#include "url.h"
#include "tcp_proxy.h"

int main(int argc, char** argv) {
    char config_file[1024];
    snprintf(config_file, sizeof config_file, "%s/.nexer/nexer.conf", getenv("HOME"));

    nexer::Config config;
    if (!nexer::Config::ParseFile(config, config_file)) {
        log_fatal("Failed to parse %s", config_file);
        return 1;
    }

    default_logger.open(config.logger().file.c_str(), config.logger().level);

    nexer::Nexer nexer(config);
    nexer.Start();

    return 0;
}
