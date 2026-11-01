#include <assert.h>

#include "nexer/core/async_work.h"
#include "nexer/core/logger.h"

namespace nexer {
namespace test {

void TestAsyncWork() {
    EventLoop loop;

    int executed = 0;
    int finished = 0;
    int error = 0;

    auto work = AsyncWork::Create(loop);

    work->OnEnd([&](int status) {
        finished++;
        error = status;
    });

    work->Start([&] {
        executed++;
        uv_sleep(20);
    });

    loop.run();

    assert(executed == 1);
    assert(finished == 1);
    assert(error == 0);
}

} // namespace test
} // namespace nexer
