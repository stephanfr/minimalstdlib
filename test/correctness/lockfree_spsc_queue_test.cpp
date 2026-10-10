// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <lockfree/spsc_queue>

#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include <pthread.h>

#define TEST_BUFFER_SIZE 65536
#define MAX_QUEUE_ELEMENTS 128

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (SingleProducerSingleConsumerLockfreeQueueTests)
    {
    };
#pragma GCC diagnostic pop

    static char buffer[TEST_BUFFER_SIZE];

    class test_element
    {
    public:
        explicit test_element(uint32_t value)
            : value_(value)
        {
        }

        test_element(const test_element &) = default;

        uint32_t value() const
        {
            return value_;
        }

    private:
        uint32_t value_ = 0;
        char empty_space_[18];
    };

    using test_element_queue = minstd::spsc_queue<test_element>;

    using queue_allocator = minstd::allocator<test_element_queue::value_type>;
    using queue_static_heap_allocator = minstd::pmr::polymorphic_allocator<test_element_queue::value_type>;

    void *produce(void *arguments)
    {
        test_element_queue *queue = static_cast<test_element_queue *>(arguments);

        for (int i = 0; i < 1000; i++)
        {

            test_element element(i);

            while (!queue->push_back(i))
            {
            }
        }

        return nullptr;
    }

    void *consume(void *arguments)
    {
        test_element_queue *queue = static_cast<test_element_queue *>(arguments);

        test_element element(65536);

        for (uint32_t i = 0; i < 1000; i++)
        {
            while (!queue->pop_front(element))
            {
            }

            CHECK_EQUAL(i, element.value());
        }

        return nullptr;
    }

    TEST(SingleProducerSingleConsumerLockfreeQueueTests, BasicTest)
    {
        minstd::pmr::monotonic_buffer_resource heap_allocator_resource(buffer, TEST_BUFFER_SIZE, nullptr);
        queue_static_heap_allocator heap_allocator(&heap_allocator_resource);

        test_element_queue queue(heap_allocator, MAX_QUEUE_ELEMENTS);

        CHECK(queue.empty());
        CHECK(queue.capacity() == MAX_QUEUE_ELEMENTS - 1);

        for (uint32_t i = 0; i < MAX_QUEUE_ELEMENTS - 2; i++)
        {
            CHECK(queue.push_back(i));
            CHECK(!queue.empty());
            CHECK(queue.size_estimate() == i + 1);
        }

        test_element front(65536);

        for (uint32_t i = 0; i < MAX_QUEUE_ELEMENTS - 2; i++)
        {
            CHECK(queue.pop_front(front));
            CHECK_EQUAL(i, front.value());
            CHECK(queue.size_estimate() == MAX_QUEUE_ELEMENTS - i - 3);
        }
    }

    TEST(SingleProducerSingleConsumerLockfreeQueueTests, MultithreadedTest)
    {
        minstd::pmr::monotonic_buffer_resource heap_allocator_resource(buffer, TEST_BUFFER_SIZE, nullptr);
        queue_static_heap_allocator heap_allocator(&heap_allocator_resource);

        test_element_queue queue(heap_allocator, MAX_QUEUE_ELEMENTS);

        //  Test a producer and consumer in different threads, 1000 times

        for (int i = 0; i < 1000; i++)
        {
            CHECK(queue.empty());
            pthread_t producer;
            pthread_t consumer;

            CHECK(pthread_create(&producer, NULL, produce, (void *)&queue) == 0);
            CHECK(pthread_create(&consumer, NULL, consume, (void *)&queue) == 0);

            CHECK(pthread_join(producer, NULL) == 0);
            CHECK(pthread_join(consumer, NULL) == 0);

            CHECK(queue.empty());
        }
    }

    TEST(SingleProducerSingleConsumerLockfreeQueueTests, PopFrontOnEmptyQueueIsANoOp)
    {
        minstd::pmr::monotonic_buffer_resource heap_allocator_resource(buffer, TEST_BUFFER_SIZE, nullptr);
        queue_static_heap_allocator heap_allocator(&heap_allocator_resource);

        test_element_queue queue(heap_allocator, 4);

        queue.popFront(); //  nothing to drop

        CHECK_TRUE(queue.empty()); //  before: false - read_index_ ran past write_index_
        CHECK_EQUAL(0u, queue.size_estimate());
        CHECK_TRUE(queue.push_back(7u));

        test_element element(0);
        CHECK_TRUE(queue.pop_front(element));
        CHECK_EQUAL(7u, element.value());
        CHECK_TRUE(queue.empty());
    }

    struct counted_element
    {
        static inline int destroyed = 0;

        int value = 0;

        counted_element() = default;
        counted_element(int v) : value(v) {}
        counted_element(const counted_element &) = default;
        counted_element &operator=(const counted_element &) = default;
        ~counted_element() { destroyed++; }
    };

    TEST(SingleProducerSingleConsumerLockfreeQueueTests, DestructorDestroysQueuedElements)
    {
        minstd::pmr::monotonic_buffer_resource heap_allocator_resource(buffer, TEST_BUFFER_SIZE, nullptr);
        minstd::pmr::polymorphic_allocator<counted_element> allocator(&heap_allocator_resource);

        {
            minstd::spsc_queue<counted_element> queue(allocator, 4);

            CHECK_TRUE(queue.push_back(1));
            CHECK_TRUE(queue.push_back(2));
            CHECK_TRUE(queue.push_back(3));

            counted_element front;
            CHECK_TRUE(queue.pop_front(front)); //  wrap the read index past the start on the way out
            CHECK_TRUE(queue.push_back(4));

            counted_element::destroyed = 0;
        }

        CHECK_EQUAL(3 + 1, counted_element::destroyed); //  three queued elements and `front`; before: 1
    }

    TEST(SingleProducerSingleConsumerLockfreeQueueTests, ZeroCapacityQueueHoldsNothing)
    {
        minstd::pmr::monotonic_buffer_resource heap_allocator_resource(buffer, TEST_BUFFER_SIZE, nullptr);
        queue_static_heap_allocator heap_allocator(&heap_allocator_resource);

        test_element_queue queue(heap_allocator, 0);

        CHECK_EQUAL(0u, queue.capacity()); //  before: SIZE_MAX
        CHECK_FALSE(queue.push_back(1u));  //  before: constructs into a 0-element buffer
        CHECK_TRUE(queue.empty());
    }
}
