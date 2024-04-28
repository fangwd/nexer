/* Copyright (c) Weidong Fang. All rights reserved. */

#include <assert.h>

#include "timer.h"

namespace nexer {
namespace test {

void TestTimer() {
    EventLoop loop;

    int tick1 = 0, tick2 = 0;

    Timer& timer1 = Timer::Create(loop, 1000);
    Timer& timer2 = Timer::Create(loop, 2100);

    timer1.OnTick([&]() {
        tick1++;
    });

    timer2.OnTick([&]() {
        tick2++;
        timer1.Close();
        timer2.Close();
    });

    timer1.Start();
    timer2.Start();

    loop.Run();

    assert(tick1 == 2);
    assert(tick2 == 1);
}

}  // namespace test
}  // namespace nexer
