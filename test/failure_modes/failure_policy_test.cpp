// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <lockfree/spsc_queue>

#include <stddef.h>
#include <stdint.h>

namespace
{
    struct failure_event_record
    {
        const char *reason = nullptr;
        const char *message = nullptr;
        const char *file = nullptr;
        int line = 0;
        int call_count = 0;
    };

    failure_event_record g_failure_event;

    void reset_failure_event()
    {
        g_failure_event.reason = nullptr;
        g_failure_event.message = nullptr;
        g_failure_event.file = nullptr;
        g_failure_event.line = 0;
        g_failure_event.call_count = 0;
    }

    void record_failure_event(const char *reason,
                              const char *message,
                              const char *file,
                              int line)
    {
        g_failure_event.reason = reason;
        g_failure_event.message = message;
        g_failure_event.file = file;
        g_failure_event.line = line;
        g_failure_event.call_count++;
    }

    bool contains_text(const char *text, const char *needle)
    {
        if (text == nullptr || needle == nullptr)
        {
            return false;
        }

        if (*needle == '\0')
        {
            return true;
        }

        for (const char *start = text; *start != '\0'; ++start)
        {
            const char *candidate = start;
            const char *pattern = needle;

            while (*candidate != '\0' && *pattern != '\0' && *candidate == *pattern)
            {
                ++candidate;
                ++pattern;
            }

            if (*pattern == '\0')
            {
                return true;
            }
        }

        return false;
    }

    template <typename T>
    class failing_allocator_type : public minstd::allocator<T>
    {
    public:
        size_t max_size() const noexcept override
        {
            return __SIZE_MAX__ / sizeof(T);
        }

        T *allocate(size_t num_elements) override
        {
            (void)num_elements;
            return nullptr;
        }

        void deallocate(T *ptr, size_t num_elements) override
        {
            (void)ptr;
            (void)num_elements;
        }
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(FailurePolicyTests)
    {
        minstd::failure_hook_type previous_hook_ = nullptr;

        void setup() override
        {
            reset_failure_event();
            previous_hook_ = minstd::set_failure_hook(record_failure_event);
        }

        void teardown() override
        {
            minstd::set_failure_hook(previous_hook_);
        }
    };
#pragma GCC diagnostic pop

    TEST(FailurePolicyTests, ContractViolationLogsWithoutTrap)
    {
        MINIMAL_STD_ASSERT(false);

        CHECK_EQUAL(1, g_failure_event.call_count);
        CHECK(g_failure_event.reason != nullptr);
        CHECK(g_failure_event.message != nullptr);
        CHECK(g_failure_event.file != nullptr);
        CHECK(g_failure_event.line > 0);
        STRCMP_EQUAL("contract_violation", g_failure_event.reason);
        STRCMP_EQUAL("false", g_failure_event.message);
    }

    TEST(FailurePolicyTests, OutOfMemoryLogsWithoutTrap)
    {
        const int expected_line = __LINE__ + 1;
        MINIMAL_STD_OUT_OF_MEMORY(64u, alignof(uint32_t));

        CHECK_EQUAL(1, g_failure_event.call_count);
        CHECK(g_failure_event.reason != nullptr);
        CHECK(g_failure_event.message != nullptr);
        CHECK(g_failure_event.file != nullptr);
        CHECK(g_failure_event.line > 0);
        STRCMP_EQUAL("allocation_failure", g_failure_event.reason);
        STRCMP_EQUAL("MINIMAL_STD_OUT_OF_MEMORY", g_failure_event.message);
        STRCMP_EQUAL(__FILE__, g_failure_event.file);
        CHECK_EQUAL(expected_line, g_failure_event.line);
    }

    TEST(FailurePolicyTests, InvalidOptionalAccessLogsWithoutTrap)
    {
        const int expected_line = __LINE__ + 1;
        minstd::invalid_optional_access(__FILE__, __LINE__);

        CHECK_EQUAL(1, g_failure_event.call_count);
        CHECK(g_failure_event.reason != nullptr);
        CHECK(g_failure_event.message != nullptr);
        CHECK(g_failure_event.file != nullptr);
        CHECK(g_failure_event.line > 0);
        STRCMP_EQUAL("invalid_optional_access", g_failure_event.reason);
        STRCMP_EQUAL("invalid_optional_access", g_failure_event.message);
        STRCMP_EQUAL(__FILE__, g_failure_event.file);
        CHECK_EQUAL(expected_line, g_failure_event.line);
    }

    TEST(FailurePolicyTests, SpscQueueConstructorOomLogsWithoutTrap)
    {
        static constexpr size_t QUEUE_CAPACITY = 8;

        failing_allocator_type<uint32_t> allocator;
        minstd::spsc_queue<uint32_t> queue(allocator, QUEUE_CAPACITY);

        CHECK(queue.empty());
        CHECK_EQUAL(QUEUE_CAPACITY - 1, queue.capacity());

        CHECK_EQUAL(1, g_failure_event.call_count);
        CHECK(g_failure_event.reason != nullptr);
        CHECK(g_failure_event.message != nullptr);
        CHECK(g_failure_event.file != nullptr);
        CHECK(g_failure_event.line > 0);
        STRCMP_EQUAL("allocation_failure", g_failure_event.reason);
        STRCMP_EQUAL("MINIMAL_STD_OUT_OF_MEMORY", g_failure_event.message);
        CHECK(contains_text(g_failure_event.file, "include/lockfree/spsc_queue"));
    }
}
