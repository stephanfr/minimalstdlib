// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../shared/lockfree_skiplist_test_helpers.h"
#include "../shared/process_isolation.h"

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (SkiplistTests)
    {
    };
#pragma GCC diagnostic pop

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (SkiplistWriteCorrectnessTests)
    {
    };
#pragma GCC diagnostic pop

    TEST(SkiplistTests, BlockGarbageCollection)
    {
        // 1024 slots per block, so shift is 10.
        minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS, 16, 10, minstd::skiplist_extensions::skiplist_statistics> list;

        list.reset_slot_high_water_mark();

        for (uint32_t cycle = 0; cycle < 10; ++cycle)
        {
            for (uint32_t i = 1; i <= 15000; ++i)
            {
                CHECK_TRUE(list.insert(i, i));
            }

            CHECK_EQUAL(15000u, list.size());

            CHECK_TRUE(list.active_blocks() >= 14);

            for (uint32_t i = 1; i <= 14500; ++i)
            {
                CHECK_TRUE(list.remove(i));
            }

            CHECK_EQUAL(500u, list.size());

            for (uint32_t i = 0; i < 50; ++i)
            {
                list.try_advance_epoch(0);
                list.find(0xFFFFFFFF);
            }

            CHECK_TRUE(list.active_blocks() <= 5);

            for (uint32_t i = 14501; i <= 15000; ++i)
            {
                CHECK_TRUE(list.remove(i));
            }

            for (uint32_t i = 0; i < 50; ++i)
            {
                list.find(0xFFFFFFFF);
            }
        }
    }

    TEST(SkiplistTests, BasicFunctionality)
    {
        minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS> list;

        for (uint32_t i = 0; i < 100; i++)
        {
            CHECK_TRUE(list.insert(i, i));
        }

        CHECK_EQUAL(100u, list.size());

        for (uint32_t i = 0; i < 100; i++)
        {
            CHECK_TRUE(list.find(i) != list.end());
            CHECK_EQUAL(i, list.find(i)->second);
        }

        CHECK_TRUE(list.find(101) == list.end());

        for (uint32_t i = 0; i < 10; i++)
        {
            CHECK_TRUE(list.remove(i * 10));
            CHECK_TRUE(list.remove((i * 10) + 1));
        }

        CHECK_EQUAL(80u, list.size());

        for (uint32_t i = 0; i < 100; i++)
        {
            if ((i % 10 == 0) || ((i > 0) && ((i - 1) % 10 == 0)))
            {
                CHECK_TRUE(list.find(i) == list.end());
            }
            else
            {
                CHECK_TRUE(list.find(i) != list.end());
                CHECK_EQUAL(i, list.find(i)->second);
            }
        }

        CHECK_FALSE(list.insert(14, 114));
        CHECK_EQUAL(14u, list.find(14)->second);

        CHECK_FALSE(list.insert(37, 137));
        CHECK_EQUAL(37u, list.find(37)->second);

        CHECK_TRUE(list.insert(100, 100));

        CHECK_EQUAL(81u, list.size());

        for (uint32_t i = 0; i <= 100; i++)
        {
            if (((i % 10 == 0) || ((i > 0) && ((i - 1) % 10 == 0))) && (i != 100))
            {
                CHECK_TRUE(list.find(i) == list.end());
            }
            else
            {
                CHECK_TRUE(list.find(i) != list.end());
                CHECK_EQUAL(i, list.find(i)->second);
            }
        }

        CHECK_EQUAL(81u, list.size());

        for (uint32_t i = 0; i <= 100; i++)
        {
            if (((i % 10 == 0) || ((i > 0) && ((i - 1) % 10 == 0))) && (i != 100))
            {
                CHECK_TRUE(list.find(i) == list.end());
            }
            else
            {
                CHECK_TRUE(list.find(i) != list.end());
                CHECK_EQUAL(i, list.find(i)->second);
            }
        }

        CHECK_TRUE(list.insert(10, 10));
        CHECK_TRUE(list.insert(31, 31));
        CHECK_TRUE(list.insert(71, 71));

        CHECK_EQUAL(84u, list.size());

        for (uint32_t i = 0; i <= 100; i++)
        {
            if (((i % 10 == 0) || ((i > 0) && ((i - 1) % 10 == 0))) &&
                (i != 10) && (i != 31) && (i != 71) && (i != 100))
            {
                CHECK_TRUE(list.find(i) == list.end());
            }
            else
            {
                CHECK_TRUE(list.find(i) != list.end());
                CHECK_EQUAL(i, list.find(i)->second);
            }
        }
    }

    TEST(SkiplistTests, ConstructorUsesProvidedMemoryResource)
    {
        class counting_memory_resource : public minstd::pmr::memory_resource
        {
        public:
            explicit counting_memory_resource(minstd::pmr::memory_resource *upstream)
                : upstream_(upstream)
            {
            }

            size_t allocations() const
            {
                return allocations_;
            }

            size_t deallocations() const
            {
                return deallocations_;
            }

        private:
            void *do_allocate(size_t bytes, size_t alignment) override
            {
                ++allocations_;
                return upstream_->allocate(bytes, alignment);
            }

            void do_deallocate(void *ptr, size_t bytes, size_t alignment) override
            {
                ++deallocations_;
                upstream_->deallocate(ptr, bytes, alignment);
            }

            bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
            {
                return this == &other;
            }

            minstd::pmr::memory_resource *upstream_;
            size_t allocations_ = 0;
            size_t deallocations_ = 0;
        };

        static unsigned char test_resource_buffer[2 * 1024 * 1024];
        minstd::pmr::lockfree_composite_single_arena_resource<> upstream_resource(test_resource_buffer, sizeof(test_resource_buffer), 2);
        counting_memory_resource counting_resource(&upstream_resource);

        const size_t allocations_before = counting_resource.allocations();
        const size_t deallocations_before = counting_resource.deallocations();

        {
            using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS>;

            list_type list(&counting_resource);

            for (uint32_t i = 0; i < 64; ++i)
            {
                CHECK_TRUE(list.insert(i, i + 1));
            }

            for (uint32_t i = 0; i < 64; ++i)
            {
                auto item = list.find(i);
                CHECK_TRUE(item != list.end());
                CHECK_EQUAL(i + 1, item->second);
            }

            for (uint32_t i = 0; i < 32; ++i)
            {
                CHECK_TRUE(list.remove(i));
            }

            CHECK_TRUE(counting_resource.allocations() > allocations_before);
        }

        CHECK_TRUE(counting_resource.deallocations() > deallocations_before);
    }

    TEST(SkiplistTests, DefaultConstructorFallbackMatchesResourceConstructorBehavior)
    {
        //  The two lists must NOT coexist: they are the same type (same Tag), so they share one slot-block
        //      memory resource, and the default list's is nullptr.  Lists that need different resources at the
        //      same time use different Tags (see TaggedListsUseIndependentMemoryResources).
        //  Run them in non-overlapping scopes and record results for cross-comparison.

        using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS>;

        bool   default_found[128]{};
        uint32_t default_values[128]{};
        uint32_t default_size = 0;

        {
            list_type default_list;

            for (uint32_t i = 0; i < 128; ++i)
            {
                CHECK_TRUE(default_list.insert(i, i * 3));
            }

            for (uint32_t i = 0; i < 128; ++i)
            {
                auto item = default_list.find(i);
                CHECK_TRUE(item != default_list.end());
                CHECK_EQUAL(i * 3, item->second);
            }

            for (uint32_t i = 0; i < 64; ++i)
            {
                CHECK_TRUE(default_list.remove(i * 2));
            }

            default_size = default_list.size();

            for (uint32_t i = 0; i < 128; ++i)
            {
                auto item = default_list.find(i);
                default_found[i] = (item != default_list.end());
                if (default_found[i])
                {
                    default_values[i] = item->second;
                }
            }

            CHECK_TRUE(default_list.validate_ordering());
        }

        {
            static unsigned char resource_buffer[2 * 1024 * 1024];
            minstd::pmr::lockfree_composite_single_arena_resource<> resource(resource_buffer, sizeof(resource_buffer), 2);
            list_type resource_list(&resource);

            for (uint32_t i = 0; i < 128; ++i)
            {
                CHECK_TRUE(resource_list.insert(i, i * 3));
            }

            for (uint32_t i = 0; i < 128; ++i)
            {
                auto item = resource_list.find(i);
                CHECK_TRUE(item != resource_list.end());
                CHECK_EQUAL(i * 3, item->second);
            }

            for (uint32_t i = 0; i < 64; ++i)
            {
                CHECK_TRUE(resource_list.remove(i * 2));
            }

            CHECK_EQUAL(default_size, resource_list.size());

            for (uint32_t i = 0; i < 128; ++i)
            {
                auto resource_item = resource_list.find(i);
                const bool in_resource = (resource_item != resource_list.end());

                CHECK_EQUAL(default_found[i], in_resource);

                if (default_found[i] && in_resource)
                {
                    CHECK_EQUAL(default_values[i], resource_item->second);
                }
            }

            CHECK_TRUE(resource_list.validate_ordering());
        }
    }

    TEST(SkiplistTests, TemplateInstantiationWithCustomMaxLevels)
    {
        minstd::skip_list<uint32_t, uint64_t, SKIPLIST_STRESS_MAX_THREADS, 8> list;

        for (uint32_t i = 0; i < 256; ++i)
        {
            CHECK_TRUE(list.insert(i, static_cast<uint64_t>(i) * 10ull));
        }

        CHECK_EQUAL(256u, list.size());

        for (uint32_t i = 0; i < 256; ++i)
        {
            auto value = list.find(i);
            CHECK_TRUE(value != list.end());
            CHECK_EQUAL(static_cast<uint64_t>(i) * 10ull, value->second);
        }

        for (uint32_t i = 0; i < 128; ++i)
        {
            CHECK_TRUE(list.remove(i));
        }

        for (uint32_t i = 0; i < 128; ++i)
        {
            CHECK_TRUE(list.find(i) == list.end());
        }

        for (uint32_t i = 128; i < 256; ++i)
        {
            auto value = list.find(i);
            CHECK_TRUE(value != list.end());
            CHECK_EQUAL(static_cast<uint64_t>(i) * 10ull, value->second);
        }
    }

    TEST(SkiplistTests, MultiThreadedStressInsertFindRemove)
    {
        using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS>;

        list_type list;

        for (uint32_t i = 0; i < SKIPLIST_STRESS_KEY_SPACE; ++i)
        {
            CHECK_TRUE(list.insert(i, i));
        }

        pthread_t workers[SKIPLIST_STRESS_NUM_THREADS]{};
        skiplist_stress_thread_args<list_type> thread_args[SKIPLIST_STRESS_NUM_THREADS]{};

        minstd::atomic<bool> start{false};
        minstd::atomic<size_t> ready_count{0};
        minstd::atomic<size_t> validation_failures{0};

        for (size_t i = 0; i < SKIPLIST_STRESS_NUM_THREADS; ++i)
        {
            thread_args[i].list_ = &list;
            thread_args[i].start_ = &start;
            thread_args[i].ready_count_ = &ready_count;
            thread_args[i].validation_failures_ = &validation_failures;
            thread_args[i].thread_id_ = static_cast<uint32_t>(i);
            thread_args[i].iterations_ = SKIPLIST_STRESS_ITERATIONS_PER_THREAD;
            thread_args[i].key_space_ = SKIPLIST_STRESS_KEY_SPACE;
            thread_args[i].operations_completed_ = 0;

            CHECK_EQUAL(0, pthread_create(&workers[i], nullptr, skiplist_stress_worker<list_type>, &thread_args[i]));
        }

        while (ready_count.load(minstd::memory_order_acquire) < SKIPLIST_STRESS_NUM_THREADS)
        {
            sched_yield();
        }

        start.store(true, minstd::memory_order_release);

        size_t total_operations = 0;

        for (size_t i = 0; i < SKIPLIST_STRESS_NUM_THREADS; ++i)
        {
            CHECK_EQUAL(0, pthread_join(workers[i], nullptr));
            total_operations += thread_args[i].operations_completed_;
        }

        const size_t expected_operations = SKIPLIST_STRESS_NUM_THREADS * SKIPLIST_STRESS_ITERATIONS_PER_THREAD;

        CHECK_EQUAL(expected_operations, total_operations);
        CHECK_EQUAL(static_cast<size_t>(0), validation_failures.load(minstd::memory_order_acquire));

        CHECK_EQUAL(SKIPLIST_STRESS_KEY_SPACE, list.size());
    }

    TEST(SkiplistTests, MultiThreadedStressThreadScaling)
    {
        using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS>;
        // Correctness sweep across 1..MAX thread counts. Use the standard correctness
        // iteration budget rather than the perf-scaled multiplier (which is reserved
        // for the performance suite). The MultiThreadedStressInsertFindRemove and
        // MultiThreadedStressOrderingAndContentCorrectness siblings use the same
        // SKIPLIST_STRESS_ITERATIONS_PER_THREAD baseline.
        const size_t iterations_per_thread = SKIPLIST_STRESS_ITERATIONS_PER_THREAD;

        for (size_t num_threads = 1; num_threads <= SKIPLIST_STRESS_MAX_THREADS; ++num_threads)
        {
            if ((num_threads == 9) || (num_threads == 11) || (num_threads == 13) || (num_threads == 15))
            {
                continue;
            }

            list_type list;

            for (uint32_t key = 0; key < SKIPLIST_STRESS_KEY_SPACE; ++key)
            {
                CHECK_TRUE(list.insert(key, key));
            }

            pthread_t workers[SKIPLIST_STRESS_MAX_THREADS]{};
            skiplist_stress_thread_args<list_type> thread_args[SKIPLIST_STRESS_MAX_THREADS]{};

            minstd::atomic<bool> start{false};
            minstd::atomic<size_t> ready_count{0};
            minstd::atomic<size_t> validation_failures{0};

            for (size_t i = 0; i < num_threads; ++i)
            {
                thread_args[i].list_ = &list;
                thread_args[i].start_ = &start;
                thread_args[i].ready_count_ = &ready_count;
                thread_args[i].validation_failures_ = &validation_failures;
                thread_args[i].thread_id_ = static_cast<uint32_t>(i);
                thread_args[i].iterations_ = iterations_per_thread;
                thread_args[i].key_space_ = SKIPLIST_STRESS_KEY_SPACE;
                thread_args[i].operations_completed_ = 0;

                CHECK_EQUAL(0, pthread_create(&workers[i], nullptr, skiplist_stress_worker<list_type>, &thread_args[i]));
            }

            while (ready_count.load(minstd::memory_order_acquire) < num_threads)
            {
                sched_yield();
            }

            start.store(true, minstd::memory_order_release);

            size_t total_operations = 0;

            for (size_t i = 0; i < num_threads; ++i)
            {
                CHECK_EQUAL(0, pthread_join(workers[i], nullptr));
                total_operations += thread_args[i].operations_completed_;
            }

            const size_t expected_operations = num_threads * iterations_per_thread;

            CHECK_EQUAL(expected_operations, total_operations);
            CHECK_EQUAL(static_cast<size_t>(0), validation_failures.load(minstd::memory_order_acquire));

            CHECK_EQUAL(SKIPLIST_STRESS_KEY_SPACE, list.size());

            CHECK_TRUE(list.validate_ordering());
        }
    }

    TEST(SkiplistTests, MultiThreadedStressOrderingAndContentCorrectness)
    {
        using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS>;

        list_type list;

        for (uint32_t key = 0; key < SKIPLIST_STRESS_KEY_SPACE; ++key)
        {
            CHECK_TRUE(list.insert(key, key));
        }

        pthread_t workers[SKIPLIST_STRESS_NUM_THREADS]{};
        skiplist_stress_thread_args<list_type> thread_args[SKIPLIST_STRESS_NUM_THREADS]{};

        minstd::atomic<bool> start{false};
        minstd::atomic<size_t> ready_count{0};
        minstd::atomic<size_t> validation_failures{0};

        for (size_t i = 0; i < SKIPLIST_STRESS_NUM_THREADS; ++i)
        {
            thread_args[i].list_ = &list;
            thread_args[i].start_ = &start;
            thread_args[i].ready_count_ = &ready_count;
            thread_args[i].validation_failures_ = &validation_failures;
            thread_args[i].thread_id_ = static_cast<uint32_t>(i);
            thread_args[i].iterations_ = SKIPLIST_STRESS_ITERATIONS_PER_THREAD * 2;
            thread_args[i].key_space_ = SKIPLIST_STRESS_KEY_SPACE;
            thread_args[i].operations_completed_ = 0;

            CHECK_EQUAL(0, pthread_create(&workers[i], nullptr, skiplist_stress_worker<list_type>, &thread_args[i]));
        }

        while (ready_count.load(minstd::memory_order_acquire) < SKIPLIST_STRESS_NUM_THREADS)
        {
            sched_yield();
        }

        start.store(true, minstd::memory_order_release);

        size_t total_operations = 0;

        for (size_t i = 0; i < SKIPLIST_STRESS_NUM_THREADS; ++i)
        {
            CHECK_EQUAL(0, pthread_join(workers[i], nullptr));
            total_operations += thread_args[i].operations_completed_;
        }

        CHECK_EQUAL(SKIPLIST_STRESS_NUM_THREADS * (SKIPLIST_STRESS_ITERATIONS_PER_THREAD * 2), total_operations);
        CHECK_EQUAL(static_cast<size_t>(0), validation_failures.load(minstd::memory_order_acquire));

        for (uint32_t key = 0; key < SKIPLIST_STRESS_KEY_SPACE; ++key)
        {
            auto value = list.find(key);
            CHECK_TRUE(value != list.end());
            CHECK_EQUAL(key, value->second);
        }

        CHECK_EQUAL(SKIPLIST_STRESS_KEY_SPACE, list.size());

        CHECK_TRUE(list.validate_ordering());
    }

    TEST(SkiplistTests, InterruptNestedReadSectionDepthCorrectness)
    {
        static constexpr uint32_t KEY_COUNT = 64;

        intr_test_list_t list;
        for (uint32_t k = 0; k < KEY_COUNT; ++k)
        {
            CHECK_TRUE(list.insert(k, k));
        }

        s_intr_list = &list;
        s_intr_signal_count = 0;
        s_intr_nested_count = 0;

        struct sigaction sa
        {
        };
        struct sigaction sa_old
        {
        };
        sa.sa_handler = sigusr1_nested_read_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        CHECK_EQUAL(0, sigaction(SIGUSR1, &sa, &sa_old));

        minstd::atomic<bool> bomber_stop{false};
        intr_test_bomber_args bargs{pthread_self(), &bomber_stop};
        pthread_t bomber;
        CHECK_EQUAL(0, pthread_create(&bomber, nullptr, intr_test_bomber_fn, &bargs));

        for (size_t i = 0; s_intr_signal_count < 100; ++i)
        {
            const uint32_t key = static_cast<uint32_t>(i % KEY_COUNT);
            auto value = list.find(key);
            CHECK_TRUE(value != list.end());
            CHECK_EQUAL(key, value->second);
        }

        bomber_stop.store(true, minstd::memory_order_release);
        pthread_join(bomber, nullptr);

        CHECK_TRUE(s_intr_signal_count > 0);

        for (size_t cycle = 0; cycle < 5; ++cycle)
        {
            for (uint32_t k = 0; k < KEY_COUNT; ++k)
            {
                list.remove(k);
            }
            for (uint32_t k = 0; k < KEY_COUNT; ++k)
            {
                CHECK_TRUE(list.insert(k, k + static_cast<uint32_t>(cycle) + 1u));
            }
        }

        CHECK_TRUE(list.validate_ordering());

        s_intr_list = nullptr;
        sigaction(SIGUSR1, &sa_old, nullptr);
    }

    TEST(SkiplistWriteCorrectnessTests, ConcurrentInsertContentOrderingThenSequentialRemove)
    {
        memory_leak_overload_scope_guard memory_leak_overload_guard;

        using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS>;

        static constexpr size_t WRITE_TEST_NUM_THREADS = 8;
        static constexpr size_t WRITE_TEST_ITERATIONS_PER_THREAD = 4096;
        static constexpr uint32_t WRITE_TEST_KEY_SPACE = 2048;

        list_type list;

        bool expected_present[WRITE_TEST_KEY_SPACE]{};

        pthread_t workers[WRITE_TEST_NUM_THREADS]{};
        skiplist_write_correctness_thread_args<list_type> thread_args[WRITE_TEST_NUM_THREADS]{};

        minstd::atomic<bool> start{false};
        minstd::atomic<size_t> ready_count{0};
        minstd::atomic<size_t> validation_failures{0};

        for (size_t i = 0; i < WRITE_TEST_NUM_THREADS; ++i)
        {
            thread_args[i].list_ = &list;
            thread_args[i].start_ = &start;
            thread_args[i].ready_count_ = &ready_count;
            thread_args[i].validation_failures_ = &validation_failures;
            thread_args[i].expected_present_ = expected_present;
            thread_args[i].thread_id_ = static_cast<uint32_t>(i);
            thread_args[i].num_threads_ = static_cast<uint32_t>(WRITE_TEST_NUM_THREADS);
            thread_args[i].iterations_ = WRITE_TEST_ITERATIONS_PER_THREAD;
            thread_args[i].key_space_ = WRITE_TEST_KEY_SPACE;
            thread_args[i].operations_completed_ = 0;

            CHECK_EQUAL(0, pthread_create(&workers[i], nullptr, skiplist_write_correctness_worker<list_type>, &thread_args[i]));
        }

        while (ready_count.load(minstd::memory_order_acquire) < WRITE_TEST_NUM_THREADS)
        {
            sched_yield();
        }

        start.store(true, minstd::memory_order_release);

        size_t total_operations = 0;

        for (size_t i = 0; i < WRITE_TEST_NUM_THREADS; ++i)
        {
            CHECK_EQUAL(0, pthread_join(workers[i], nullptr));
            total_operations += thread_args[i].operations_completed_;
        }

        CHECK_EQUAL(WRITE_TEST_NUM_THREADS * WRITE_TEST_ITERATIONS_PER_THREAD, total_operations);
        CHECK_EQUAL(0u, validation_failures.load(minstd::memory_order_relaxed));

        uint32_t expected_size = 0;

        for (uint32_t key = 0; key < WRITE_TEST_KEY_SPACE; ++key)
        {
            auto value = list.find(key);

            if (expected_present[key])
            {
                CHECK_TRUE(value != list.end());
                CHECK_EQUAL(key, value->second);
                expected_size++;
            }
            else
            {
                CHECK_TRUE(value == list.end());
            }
        }

        CHECK_EQUAL(expected_size, list.size());

        CHECK_TRUE(list.validate_ordering());

        for (uint32_t key = 0; key < WRITE_TEST_KEY_SPACE; ++key)
        {
            if (expected_present[key])
            {
                CHECK_TRUE(list.remove(key));
            }
            else
            {
                CHECK_FALSE(list.remove(key));
            }

            CHECK_TRUE(list.find(key) == list.end());
        }

        CHECK_EQUAL(0u, list.size());

        CHECK_TRUE(list.validate_ordering());
    }

    TEST(SkiplistTests, IteratorBasicTest)
    {
        using skip_list_type = minstd::skip_list<uint32_t, uint32_t, 16, 16>;
        skip_list_type list;

        size_t count = 0;
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            count++;
        }
        CHECK_EQUAL(0, count);

        for (uint32_t i = 10; i < 20; ++i)
        {
            list.insert(i, i * 10);
        }

        count = 0;
        uint32_t last_key = 0;
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            count++;
            CHECK_TRUE(it->first > last_key);
            CHECK_EQUAL(it->first * 10, it->second);
            last_key = it->first;
        }
        CHECK_EQUAL(10, count);
        CHECK_EQUAL(19, last_key);

        list.remove(15);
        list.remove(10);
        list.remove(19);

        count = 0;
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            count++;
        }
        CHECK_EQUAL(7, count);
    }

    TEST(SkiplistTests, ConcurrentInsertRemoveOfSameKeysReclaimsEveryNode)
    {
        //  Threads insert and remove the same few keys.  Once the list is emptied and every level has been
        //      helped, every node that was successfully inserted must have been reclaimed.  A node that
        //      insert() published at an upper level after remove() retired it, or that was skipped by a stale
        //      upper-level successor, is never unlinked at level 0 and is missing from nodes_reclaimed().

        using list_type = minstd::skip_list<uint32_t, uint32_t, SKIPLIST_STRESS_MAX_THREADS, 16, 14, minstd::skiplist_extensions::skiplist_statistics>;

        constexpr uint32_t NUM_THREADS = 8;
        constexpr uint32_t NUM_KEYS = 16;
        constexpr uint32_t OPS_PER_THREAD = 100000;

        list_type list;
        list.reset_statistics();

        struct args_type
        {
            list_type *list;
            minstd::atomic<bool> *go;
            minstd::atomic<uint32_t> *inserted;
            uint64_t seed;
        };

        auto worker = [](void *arg) -> void *
        {
            auto *a = static_cast<args_type *>(arg);

            while (!a->go->load(minstd::memory_order_acquire))
            {
            }

            uint64_t rng = a->seed;

            for (uint32_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                const uint32_t key = static_cast<uint32_t>((rng >> 33) % NUM_KEYS) + 1;

                if (((rng >> 62) & 1) != 0)
                {
                    if (a->list->insert(key, key))
                    {
                        a->inserted->fetch_add(1);
                    }
                }
                else
                {
                    a->list->remove(key);
                }
            }

            return nullptr;
        };

        minstd::atomic<bool> go{false};
        minstd::atomic<uint32_t> inserted{0};
        args_type args[NUM_THREADS];
        pthread_t threads[NUM_THREADS];

        for (uint32_t t = 0; t < NUM_THREADS; ++t)
        {
            args[t] = {&list, &go, &inserted, 0x9E3779B97F4A7C15ULL * (t + 1)};
            CHECK_EQUAL(0, pthread_create(&threads[t], nullptr, worker, &args[t]));
        }

        go.store(true, minstd::memory_order_release);

        for (uint32_t t = 0; t < NUM_THREADS; ++t)
        {
            pthread_join(threads[t], nullptr);
        }

        for (uint32_t key = 1; key <= NUM_KEYS; ++key)
        {
            list.remove(key);
        }

        //  Help every level unlink, and let every CPU slot reclaim what it retired.
        for (uint32_t round = 0; round < 64; ++round)
        {
            for (uint32_t key = 1; key <= NUM_KEYS; ++key)
            {
                list.find(key);
            }

            for (size_t slot = 0; slot < SKIPLIST_STRESS_MAX_THREADS; ++slot)
            {
                list.advance_and_reclaim(slot);
            }
        }

        CHECK_EQUAL(0u, list.size());
        CHECK_EQUAL(inserted.load(), list.nodes_reclaimed()); //  before: fewer (stranded nodes)
    }

    TEST(SkiplistTests, SharedSlotReclaimThrottleIsRaceFree)
    {
        //  Several threads drive the throttle of ONE slot, as threads on one CPU (or an interrupt handler and
        //      the code it interrupted) do through skip_list::remove().
        using state_type = minstd::cpu_slot_policy_state<int, 16, 4>;

        static state_type state;
        state.initialize(0, 1);

        constexpr size_t NUM_THREADS = 4;
        constexpr size_t ITERATIONS = 100000;

        auto worker = [](void *arg) -> void *
        {
            const bool freeing = (reinterpret_cast<uintptr_t>(arg) & 1) != 0;

            for (size_t i = 0; i < ITERATIONS; ++i)
            {
                state.increment_reclaim_throttle();

                if (state.reclaim_throttle_triggered())
                {
                    state.adapt_reclaim_throttle(freeing ? 1 : 0, freeing); //  before: TSan data race on the period
                }
            }

            return nullptr;
        };

        pthread_t threads[NUM_THREADS];
        for (uintptr_t t = 0; t < NUM_THREADS; ++t)
        {
            CHECK_EQUAL(0, pthread_create(&threads[t], nullptr, worker, reinterpret_cast<void *>(t)));
        }
        for (auto &thread : threads)
        {
            pthread_join(thread, nullptr);
        }

        const uint32_t period = state.reclaim_throttle_period();
        CHECK(period >= 4 && period <= 16);
    }

    //  ---- Slot allocator tests (K2/K3) ----

    namespace
    {
        //  atomic_forward_link reads these two fields from the pointer it is given.  Each test uses its own node
        //      type because the slot tables are static per instantiation.
        struct aba_node
        {
            uint32_t internal_slot_ = 0;
            uint32_t internal_generation_ = 0;
        };

        struct churn_node
        {
            uint32_t internal_slot_ = 0;
            uint32_t internal_generation_ = 0;
        };

        struct exhaust_node
        {
            uint32_t internal_slot_ = 0;
            uint32_t internal_generation_ = 0;
        };

        struct oom_node
        {
            uint32_t internal_slot_ = 0;
            uint32_t internal_generation_ = 0;
        };

        class failing_block_resource : public minstd::pmr::memory_resource
        {
            void *do_allocate(size_t, size_t) override { return nullptr; }
            void do_deallocate(void *, size_t, size_t) override {}
            bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override { return this == &other; }
        };

        template <typename link_type>
        void release_all_slot_blocks()
        {
            for (auto &entry : link_type::blocks_)
            {
                link_type::delete_block(entry.exchange(nullptr)); //  delete_block(nullptr) is a no-op
            }

            for (auto *block = link_type::extract_unlinked_blocks(); block != nullptr;)
            {
                auto *next = block->retired_next;
                link_type::delete_block(block);
                block = next;
            }
        }
    }

    TEST(SkiplistTests, SlotAllocatorNeverHandsOutTheSameSlotTwice)
    {
        using link_type = minstd::skiplist_internal::atomic_forward_link<aba_node>;

        constexpr size_t NUM_THREADS = 8;
        constexpr size_t ITERATIONS = 200000;
        constexpr size_t TRACKED_SLOTS = 1u << 14; //  block 0

        static aba_node nodes[NUM_THREADS];
        static minstd::atomic<uint32_t> holders[TRACKED_SLOTS];

        struct args_type
        {
            aba_node *node;
            minstd::atomic<bool> *go;
            minstd::atomic<uint32_t> *violations;
        };

        auto worker = [](void *arg) -> void *
        {
            auto *a = static_cast<args_type *>(arg);

            //  Keep one slot for the whole run so the block never drains and retires.
            const auto pin = link_type::allocate_slot(a->node);

            while (!a->go->load(minstd::memory_order_acquire))
            {
            }

            for (size_t i = 0; i < ITERATIONS; ++i)
            {
                const auto handle = link_type::allocate_slot(a->node);

                if ((handle.slot == 0) || (handle.slot >= TRACKED_SLOTS))
                {
                    continue;
                }

                if (holders[handle.slot].fetch_add(1) != 0) //  someone else holds this slot too
                {
                    a->violations->fetch_add(1);
                }

                holders[handle.slot].fetch_sub(1);
                link_type::free_slot(handle.slot);
            }

            link_type::free_slot(pin.slot);
            return nullptr;
        };

        minstd::atomic<bool> go{false};
        minstd::atomic<uint32_t> violations{0};
        args_type args[NUM_THREADS];
        pthread_t threads[NUM_THREADS];

        for (size_t t = 0; t < NUM_THREADS; ++t)
        {
            args[t] = {&nodes[t], &go, &violations};
            CHECK_EQUAL(0, pthread_create(&threads[t], nullptr, worker, &args[t]));
        }

        go.store(true, minstd::memory_order_release);

        for (size_t t = 0; t < NUM_THREADS; ++t)
        {
            pthread_join(threads[t], nullptr);
        }

        release_all_slot_blocks<link_type>();

        CHECK_EQUAL(0u, violations.load()); //  before: > 0 (probabilistic)
    }

    TEST(SkiplistTests, SlotAllocateFreeCycleReusesOneSlotAndOneBlock)
    {
        using link_type = minstd::skiplist_internal::atomic_forward_link<churn_node, 14, minstd::skiplist_extensions::skiplist_slot_statistics>;

        churn_node node;

        for (int i = 0; i < 100; ++i)
        {
            const auto handle = link_type::allocate_slot(&node);
            CHECK(handle.slot != 0);
            link_type::free_slot(handle.slot);
        }

        CHECK(link_type::slot_high_water_mark() <= 1);              //  before: 100
        CHECK_EQUAL(1u, link_type::slot_stats_.blocks_allocated()); //  before: 100

        release_all_slot_blocks<link_type>();
    }

    TEST(SkiplistTests, SlotRangesAreReusedOnceTheCursorIsExhausted)
    {
        using link_type = minstd::skiplist_internal::atomic_forward_link<exhaust_node, 6>; //  64 slots per block

        static exhaust_node node;
        static uint32_t slots[link_type::MAX_SLOTS];
        size_t allocated = 0;

        //  Walk the cursor through the whole slot space (slot 0 is the null handle).
        while (true)
        {
            const auto handle = link_type::allocate_slot(&node);
            if (handle.slot == 0)
            {
                break;
            }
            slots[allocated++] = handle.slot;
        }

        CHECK_EQUAL(link_type::MAX_SLOTS - 1, allocated);

        for (size_t i = 0; i < allocated; ++i) //  every block drains and retires
        {
            link_type::free_slot(slots[i]);
        }

        const auto handle = link_type::allocate_slot(&node);
        CHECK(handle.slot != 0); //  before: 0 forever - retired ranges were never reused

        link_type::free_slot(handle.slot);
        release_all_slot_blocks<link_type>();
    }

    TEST(SkiplistTests, FailedSlotBlockAllocationReturnsNoSlot)
    {
        CHECK(minstd::pmr::test::runs_to_completion([]
                                                    {
                                                        using link_type = minstd::skiplist_internal::atomic_forward_link<oom_node>;
                                                        failing_block_resource resource;
                                                        link_type::set_block_memory_resource(&resource);
                                                        oom_node node;
                                                        const auto handle = link_type::allocate_slot(&node);
                                                        if (handle.slot != 0) _exit(1); })); //  before: the child segfaults
    }


    namespace
    {
        //  Counts allocations; honours alignment (skip_node and slot_block are alignas(64)).
        class aligned_counting_resource : public minstd::pmr::memory_resource
        {
        public:
            size_t allocations() const { return allocations_; }
            size_t outstanding() const { return allocations_ - deallocations_; }

        private:
            size_t allocations_ = 0;
            size_t deallocations_ = 0;

            void *do_allocate(size_t bytes, size_t alignment) override
            {
                allocations_++;
                return aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
            }

            void do_deallocate(void *ptr, size_t, size_t) override
            {
                deallocations_++;
                free(ptr);
            }

            bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override { return this == &other; }
        };

        struct first_list_tag {};
        struct second_list_tag {};
    }

    TEST(SkiplistTests, TaggedListsUseIndependentMemoryResources)
    {
        //  Same key, value and parameters, different tags: each list has its own slot tables, so the two can be
        //      alive at the same time on different memory resources.
        using first_list_type = minstd::skip_list<uint32_t, uint32_t, 4, 16, 6, minstd::skiplist_extensions::null_skiplist_statistics, first_list_tag>;
        using second_list_type = minstd::skip_list<uint32_t, uint32_t, 4, 16, 6, minstd::skiplist_extensions::null_skiplist_statistics, second_list_tag>;

        aligned_counting_resource first_resource;
        aligned_counting_resource second_resource;

        {
            first_list_type first(&first_resource);
            second_list_type second(&second_resource);

            const size_t second_allocations = second_resource.allocations();

            for (uint32_t key = 1; key <= 200; ++key) //  more than one 64-slot block
            {
                CHECK_TRUE(first.insert(key, key * 10));
            }

            CHECK_EQUAL(second_allocations, second_resource.allocations()); //  nothing of first's came from second's resource

            for (uint32_t key = 1; key <= 100; ++key)
            {
                CHECK_TRUE(second.insert(key, key * 20));
            }

            CHECK_EQUAL(200u, first.size());
            CHECK_EQUAL(100u, second.size());

            for (uint32_t key = 1; key <= 100; ++key)
            {
                CHECK_EQUAL(key * 10, first.find(key)->second);
                CHECK_EQUAL(key * 20, second.find(key)->second);
            }
        } //  second is destroyed first: it must not redirect first's teardown to second's resource

        CHECK_EQUAL(0u, first_resource.outstanding());
        CHECK_EQUAL(0u, second_resource.outstanding());
    }

}
