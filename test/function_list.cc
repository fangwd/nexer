#include "function_list.h"

#include <assert.h>

namespace nexer {
namespace test {

void TestFunctionList() {
    FunctionList<void, int, int> fn_list;
    int totalx = 0, totaly = 0;

    std::function<void(int,int)> fn1 = [&](int x, int y) -> void {
        totalx += x;
        totaly += y;
    };

    std::function<void(int, int)> fn2 = [&](int x, int y) {
        totalx += x;
        totaly += y;
    };

    auto remove1 = fn_list.Add(fn1);
    fn_list.Invoke(1, 2);

    assert(totalx == 1);
    assert(totaly == 2);

    remove1();

    fn_list.Invoke(1, 2);

    assert(totalx == 1);
    assert(totaly == 2);

    remove1 = fn_list.Add(fn1);

    auto remove2 = fn_list.Add(fn2);

    fn_list.Invoke(1, 2);

    assert(totalx == 3);
    assert(totaly == 6);

    remove2();

    fn_list.Invoke(1, 2);
    assert(totalx == 4);
    assert(totaly == 8);
}

}  // namespace test
}  // namespace nexer

int main() {
    nexer::test::TestFunctionList();
}
