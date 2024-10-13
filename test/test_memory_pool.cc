#include "memory_pool.h"
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
        auto data = pool.Allocate(size);
        assert(data);
        assert(size > 0);
        assert(size < 512);
    }
    {
        FixedSizeMemoryPool pool(512);
        size_t size = 0;
        auto data = pool.Allocate(size);
        pool.Free(data);
    }
    {
        FixedSizeMemoryPool pool(512);
        size_t size = 0;
        auto data1 = pool.Allocate(size);
        auto data2 = pool.Allocate(size);
        assert(data1);
        assert(data2);
        pool.Free(data1);
        auto data3 = pool.Allocate(size);
        assert(data3 == data1);
        pool.Free(data3);
        pool.Free(data2);
        auto data4 = pool.Allocate(size);
        assert(data4 == data2);
    }
}

void TestMemoryPool() {
    TestFixedSizeMemoryPool();
}
}
}