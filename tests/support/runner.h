#ifndef NEXER_TEST_RUNNER_H_
#define NEXER_TEST_RUNNER_H_

#include "nexer/core/logger.h"
#include "uv.h"

#include <assert.h>

namespace nexer {
namespace test {
struct Task {
    const char *name;
    void (*run)(void);
};

extern Task tasks[];
extern char exename[4096];
extern int echo_server_port;

void Run(const char *name);
// Runs every suite in a child process; returns the number of failed suites.
int Run();

} // namespace test
} // namespace nexer

#endif // NEXER_TEST_RUNNER_H_
