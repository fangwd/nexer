#include "nexer/core/function_list.h"
#include "runner.h"

#include <assert.h>

namespace nexer {
namespace test {

void TestFunctionList() {
    FunctionList<void, int, int> fn_list;
    auto guard = std::make_shared<int>(0);
    fn_list.set_owner(guard);
    int totalx = 0, totaly = 0;

    std::function<void(int, int)> fn1 = [&](int x, int y) -> void {
        totalx += x;
        totaly += y;
    };

    std::function<void(int, int)> fn2 = [&](int x, int y) {
        totalx += x;
        totaly += y;
    };

    auto remove1 = fn_list.add(fn1);
    fn_list.invoke(1, 2);

    assert(totalx == 1);
    assert(totaly == 2);

    remove1();

    fn_list.invoke(1, 2);

    assert(totalx == 1);
    assert(totaly == 2);

    remove1 = fn_list.add(fn1);

    auto remove2 = fn_list.add(fn2);

    fn_list.invoke(1, 2);

    assert(totalx == 3);
    assert(totaly == 6);

    remove2();

    fn_list.invoke(1, 2);
    assert(totalx == 4);
    assert(totaly == 8);

    bool cleared = false;
    fn_list.add([&](int x, int y) {
        totalx += x;
        totaly += y;
        fn_list.clear();
        cleared = true;
    });

    fn_list.add(fn2);

    fn_list.invoke(1, 2);
    assert(cleared);
    assert(totalx == 6);
    assert(totaly == 12);

    fn_list.invoke(1, 2);
    assert(totalx == 6);
    assert(totaly == 12);
}

} // namespace test
} // namespace nexer
