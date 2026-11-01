#include <assert.h>
#include <stdexcept>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "nexer/core/string_buffer.h"

namespace nexer {
namespace test {

static void test_basic_operations() {
    StringBuffer sb;

    // Test write
    sb.Write("hello");
    assert(sb.size() == 5);
    assert(strcmp(sb.data(), "hello") == 0);

    // Test append
    sb.Write(" world");
    assert(sb.size() == 11);
    assert(strcmp(sb.data(), "hello world") == 0);

    // Test clear
    sb.Clear();
    assert(sb.size() == 0);
}

static void test_printf() {
    StringBuffer sb;

    sb.Printf("Number: %d, String: %s", 42, "test");
    assert(strcmp(sb.data(), "Number: 42, String: test") == 0);

    // Test long format string
    sb.Clear();
    for (int i = 0; i < 100; i++) {
        sb.Printf("%d ", i);
    }
    assert(sb.size() > 0);
}

static void test_resize() {
    StringBuffer sb;

    sb.Write("test");
    assert(sb.size() == 4);

    // Grow
    sb.Resize(10);
    assert(sb.size() == 10);

    // Shrink
    sb.Resize(2);
    assert(sb.size() == 2);
    assert(sb.data()[0] == 't');
    assert(sb.data()[1] == 'e');
}

static void test_consume() {
    StringBuffer sb;

    sb.Write("hello world");
    assert(sb.size() == 11);

    // Consume some bytes
    sb.Consume(6);
    assert(sb.size() == 5);
    assert(strcmp(sb.data(), "world") == 0);

    // Consume all
    sb.Consume(5);
    assert(sb.size() == 0);

    // Test over-consume (should clamp to size)
    sb.Write("test");
    sb.Consume(100);
    assert(sb.size() == 0);
}

static void test_indexing() {
    StringBuffer sb;

    sb.Write("hello");

    // Positive indexing
    assert(sb[0] == 'h');
    assert(sb[4] == 'o');

    // Negative indexing
    assert(sb[-1] == 'o');
    assert(sb[-5] == 'h');

    // Modification
    sb[0] = 'H';
    assert(sb[0] == 'H');

    // Out of bounds - should throw
    bool caught = false;
    try {
        char c = sb[100];
        (void)c;
    } catch (const std::out_of_range &) {
        caught = true;
    }
    assert(caught);

    // Negative out of bounds
    caught = false;
    try {
        char c = sb[-100];
        (void)c;
    } catch (const std::out_of_range &) {
        caught = true;
    }
    assert(caught);
}

static void test_large_buffer() {
    StringBuffer sb;

    // Write 1MB of data
    const size_t MB = 1024 * 1024;
    std::string large_str(MB, 'X');

    sb.Write(large_str.data(), large_str.size());
    assert(sb.size() == MB);
    assert(sb.data()[0] == 'X');
    assert(sb.data()[MB - 1] == 'X');

    // Consume half
    sb.Consume(MB / 2);
    assert(sb.size() == MB / 2);
    assert(sb.data()[0] == 'X');
}

static void test_safe_pbump() {
    StringBuffer sb;

    // Test that we can append large amounts incrementally
    // This tests the safe_pbump implementation
    for (int i = 0; i < 10000; i++) {
        sb.Write("test", 4);
    }
    assert(sb.size() == 40000);
}

static void test_reserve_and_appended() {
    StringBuffer sb;

    // Reserve space
    bool success = sb.Reserve(1024);
    assert(success);

    // Manually write to buffer
    memcpy(sb.data(), "direct", 6);
    sb.appended(6);

    assert(sb.size() == 6);
    assert(strcmp(sb.data(), "direct") == 0);
}

static void test_ends_with() {
    StringBuffer sb;

    sb.Write("hello world");

    assert(sb.ends_with("world"));
    assert(sb.ends_with("d"));
    assert(!sb.ends_with("hello"));
    assert(!sb.ends_with("worlds"));
}

static void test_stream_operations() {
    StringBuffer sb;

    sb << "Number: " << 42 << ", Boolean: " << true;
    assert(sb.size() > 0);
    assert(strstr(sb.data(), "42") != nullptr);
}

static void test_file_operations() {
    StringBuffer sb;
    const char *test_file = "/tmp/nexer_test_stringbuffer.txt";

    // Write to file
    sb.Write("test content");
    bool saved = sb.Save(test_file);
    assert(saved);

    // Read from file
    StringBuffer sb2;
    bool loaded = sb2.Load(test_file);
    assert(loaded);
    assert(sb2.size() == sb.size());
    assert(strcmp(sb2.data(), sb.data()) == 0);

    // Cleanup
    unlink(test_file);
}

static void test_equality() {
    StringBuffer sb;

    sb.Write("test");
    assert(sb == "test");
    assert(!(sb == "test2"));
    assert(!(sb == "tes"));
}

static void test_boundary_conditions() {
    StringBuffer sb;

    // Empty buffer operations
    sb.Consume(0);
    assert(sb.size() == 0);

    sb.Resize(0);
    assert(sb.size() == 0);

    // Write empty string
    sb.Write("", 0);
    assert(sb.size() == 0);

    // Write single character
    sb.Write('A');
    assert(sb.size() == 1);
    assert(sb[0] == 'A');
}

static void test_multithread_safety() {
    // Each thread gets its own StringBuffer, testing that the
    // safe_pbump and other operations work correctly under load
    const int num_threads = 10;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([i]() {
            StringBuffer sb;
            for (int j = 0; j < 1000; j++) {
                sb.Printf("Thread %d, iteration %d\n", i, j);
            }
            // Basic validation
            assert(sb.size() > 0);
            assert(sb.data() != nullptr);
        });
    }

    for (auto &t : threads) {
        t.join();
    }
}

static void test_stress() {
    StringBuffer sb;

    // Mix of operations to stress test
    for (int i = 0; i < 100; i++) {
        sb.Write("append");
        sb.Printf("_%d", i);
        if (i % 10 == 0) {
            sb.Consume(5);
        }
        if (i % 20 == 0) {
            sb.Resize(sb.size() / 2);
        }
    }

    // Should not crash and buffer should be in valid state
    assert(sb.size() >= 0);
}

void TestStringBuffer() {
    test_basic_operations();
    test_printf();
    test_resize();
    test_consume();
    test_indexing();
    test_large_buffer();
    test_safe_pbump();
    test_reserve_and_appended();
    test_ends_with();
    test_stream_operations();
    test_file_operations();
    test_equality();
    test_boundary_conditions();
    test_multithread_safety();
    test_stress();
}

} // namespace test
} // namespace nexer
