#include <assert.h>

#include "async_work.h"
#include "logger.h"

namespace nexer {
namespace test {

void TestAsyncWork() {
    EventLoop loop;

    int executed = 0;
    int finished = 0;
    int error = 0;

    AsyncWork work(loop);

    work.OnEnd([&](int status) {
        finished++;
        error = status;
    });

    work.Start([&] {
        executed++;
        uv_sleep(1000);
    });

    loop.Run();

    assert(executed == 1);
    assert(finished == 1);
    assert(error == 0);
}

}  // namespace test
}  // namespace nexer
