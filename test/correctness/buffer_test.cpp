// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>
#include <buffer>

#include <stdint.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(BufferTests)
    {
    };
#pragma GCC diagnostic pop

    class failing_resource : public minstd::pmr::memory_resource
    {
    public:
        size_t calls = 0;

    private:
        void *do_allocate(size_t, size_t) override
        {
            calls++;
            return nullptr;
        }

        void do_deallocate(void *, size_t, size_t) override {}

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override { return this == &other; }
    };

    TEST(BufferTests, StackBufferAppendCopiesWholeElements)
    {
        minstd::stack_buffer<uint32_t, 8> buffer;

        for (size_t i = 0; i < 8; i++)
        {
            buffer[i] = 0;
        }

        const uint32_t values[] = {0x11111111u, 0x22222222u, 0x33333333u};

        CHECK_EQUAL(3u, buffer.append(values, 3));
        CHECK_EQUAL(3u, buffer.size());
        CHECK_EQUAL(0x11111111u, buffer[0]); //  before: 0x00111111 (only 3 bytes copied)
        CHECK_EQUAL(0x22222222u, buffer[1]);
        CHECK_EQUAL(0x33333333u, buffer[2]);

        CHECK_EQUAL(1u, buffer.append(0x44444444u));
        CHECK_EQUAL(0x44444444u, buffer[3]);
    }

    TEST(BufferTests, HeapBufferWithFailedAllocationIsEmptyAndFull)
    {
        failing_resource resource;
        minstd::heap_buffer<uint32_t> buffer(resource, 16);

        CHECK_EQUAL(0u, buffer.buffer_size()); //  before: 16, with a null buffer
        CHECK_EQUAL(0u, buffer.space_remaining());
        CHECK_EQUAL(0u, buffer.append(42u)); //  before (if reached): memcpy into nullptr
        CHECK_EQUAL(0u, buffer.size());
    }

    TEST(BufferTests, HeapBufferRejectsOverflowingSizes)
    {
        failing_resource resource;
        minstd::heap_buffer<uint32_t> buffer(resource, (SIZE_MAX / sizeof(uint32_t)) + 2); //  * 4 wraps to 4

        CHECK_EQUAL(0u, resource.calls); //  before: a 4-byte request for 2^62 elements
        CHECK_EQUAL(0u, buffer.buffer_size());
    }
}
