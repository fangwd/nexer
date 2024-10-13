#ifndef NEXER_TEST_RUNNER_H_
#define NEXER_TEST_RUNNER_H_

#include "uv.h"
#include "logger.h"

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
void Run();

}  // namespace test
}  // namespace nexer

#endif  // NEXER_TEST_RUNNER_H_
