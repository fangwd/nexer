#include "nexer/core/memory_pool.h"
#include <assert.h>

namespace nexer {
namespace test {
static void TestFixedSizeMemoryPool() {
    {
        FixedSizeMemoryPool pool;
    }
    {
        FixedSizeMemoryPool pool(512);
        size_t size = 0;
        auto data = pool.allocate(size);
        assert(data);
        assert(size > 0);
        assert(size < 512);
    }
    {
        FixedSizeMemoryPool pool(512);
        size_t size = 0;
        auto data = pool.allocate(size);
        pool.reclaim(data);
    }
    {
        FixedSizeMemoryPool pool(512);
        size_t size = 0;
        auto data1 = pool.allocate(size);
        auto data2 = pool.allocate(size);
        assert(data1);
        assert(data2);
        pool.reclaim(data1);
        auto data3 = pool.allocate(size);
        assert(data3 == data1);
        pool.reclaim(data3);
        pool.reclaim(data2);
        auto data4 = pool.allocate(size);
        assert(data4 == data2);
    }
}

void TestMemoryPool() { TestFixedSizeMemoryPool(); }
} // namespace test
} // namespace nexer
