#include <assert.h>

#include "nexer/core/timer.h"

namespace nexer {
namespace test {

void TestTimer() {
    EventLoop loop;

    int tick1 = 0;
    bool reached = false;
    bool timed_out = false;

    auto timer1 = Timer::create(loop, 30);
    auto timer2 = Timer::create(loop, 400);

    timer1->on_tick([&]() {
        tick1++;
        if (tick1 >= 2 && !reached) {
            reached = true;
            timer1->close();
            timer2->close();
        }
    });

    timer2->on_tick([&]() {
        timed_out = true;
        timer1->close();
        timer2->close();
    });

    timer1->start();
    timer2->start(false);

    loop.run();

    assert(reached);
    assert(!timed_out);
}

} // namespace test
} // namespace nexer
