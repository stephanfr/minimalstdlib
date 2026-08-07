// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <lockfree/mp_sc_growable_ring_queue>
#include <lockfree/spsc_queue>

#include <__memory_resource/memory_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include "../shared/perf_report.h"
#include "../shared/perf_test_config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <utility>

//  Two capacity regimes are exercised here, not just one, because they stress
//  genuinely different parts of the implementation and neither predicts the
//  other's result:
//
//  - "Fixed, sized so no regrowth occurs" isolates steady-state per-operation
//    cost (the working set is one large, never-touched-again buffer).
//  - "Small initial capacity, left to grow continuously" exercises the
//    drain-then-regrow path on essentially every call, and -- because the
//    active buffer stays small for most of the run -- tends to stay
//    cache/TLB-resident in a way the large fixed buffer cannot, which has
//    previously produced HIGHER throughput than the fixed-capacity case.
//    That is a real, measured effect (not a mistake), so both regimes are
//    tracked here rather than assuming the fixed-capacity number is always
//    representative.
//
//  Thread counts are chosen to bracket the actual deployment shape this
//  queue was built for: mostly 1 producer/1 consumer, with occasional bursts
//  of up to 4-5 producers against the single consumer.
namespace
{
    struct perf_item
    {
        uint64_t seq_;
    };

    double now_seconds()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

    //  monotonic_buffer_resource is deliberately not thread-safe (matches
    //  std::pmr::monotonic_buffer_resource), so every multithreaded scenario
    //  below needs a resource backed directly by global new/delete instead.
    class new_delete_test_resource : public minstd::pmr::memory_resource
    {
    private:
        void *do_allocate(size_t bytes, size_t align) override
        {
            return ::operator new(bytes, static_cast<std::align_val_t>(align));
        }

        void do_deallocate(void *ptr, size_t, size_t align) override
        {
            ::operator delete(ptr, static_cast<std::align_val_t>(align));
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
        {
            return this == &other;
        }
    };

    constexpr size_t DEFAULT_TOTAL_ITEMS = 6'000'000;

    //  ---- original spsc_queue: fixed baseline, unaffected by anything below ----
    double run_spsc_baseline(size_t total_items, size_t capacity)
    {
        using queue_t = minstd::spsc_queue<perf_item>;

        new_delete_test_resource resource;
        minstd::pmr::polymorphic_allocator<perf_item> alloc(&resource);
        queue_t q(alloc, capacity);

        struct thread_args
        {
            queue_t *q;
            size_t total;
        } args{&q, total_items};

        pthread_t producer, consumer;

        double start = now_seconds();

        pthread_create(
            &producer, nullptr, [](void *arg) -> void *
            {
            auto *a = static_cast<thread_args *>(arg);
            for (size_t i = 0; i < a->total; ++i)
            {
                perf_item it{i};
                while (!a->q->push_back(it)) {}
            }
            return nullptr; },
            &args);

        pthread_create(
            &consumer, nullptr, [](void *arg) -> void *
            {
            auto *a = static_cast<thread_args *>(arg);
            size_t received = 0;
            uint64_t checksum = 0;
            while (received < a->total)
            {
                perf_item it{0};
                if (a->q->pop_front(it)) { checksum += it.seq_; ++received; }
            }
            if (checksum == 0xFFFFFFFFFFFFFFFFull) printf("impossible\n");
            return nullptr; },
            &args);

        pthread_join(producer, nullptr);
        pthread_join(consumer, nullptr);

        return static_cast<double>(total_items) / (now_seconds() - start);
    }

    //  ---- ring queue: configurable producer count and capacity regime ----
    template <size_t SegmentCount, size_t NumProducers, size_t NumConsumers>
    struct ring_perf_runner
    {
        using queue_t = minstd::mp_sc_growable_ring_queue<perf_item, SegmentCount>;
        using alloc_t = minstd::pmr::polymorphic_allocator<typename queue_t::allocator_type::value_type>;

        queue_t *queue_ = nullptr;
        size_t items_per_producer_ = 0;
        minstd::atomic<uint64_t> popped_{0};
        minstd::atomic<uint64_t> finished_producers_{0};

        struct producer_args
        {
            ring_perf_runner *self;
        };

        static void *produce(void *arg)
        {
            auto *a = static_cast<producer_args *>(arg);
            ring_perf_runner *self = a->self;
            delete a;

            for (size_t i = 0; i < self->items_per_producer_; ++i)
            {
                perf_item it{i};
                while (!self->queue_->push_back(it))
                {
                }
            }

            self->finished_producers_.fetch_add(1, minstd::memory_order_acq_rel);
            return nullptr;
        }

        static void *consume(void *arg)
        {
            auto *self = static_cast<ring_perf_runner *>(arg);
            uint64_t checksum = 0;
            size_t total = self->items_per_producer_ * NumProducers;

            while (true)
            {
                perf_item it{0};

                if (self->queue_->pop_front(it))
                {
                    checksum += it.seq_;
                    self->popped_.fetch_add(1, minstd::memory_order_acq_rel);
                }
                else if (self->finished_producers_.load(minstd::memory_order_acquire) == NumProducers &&
                         self->popped_.load(minstd::memory_order_acquire) == total)
                {
                    break;
                }
            }

            if (checksum == 0xFFFFFFFFFFFFFFFFull)
            {
                printf("impossible\n");
            }

            return nullptr;
        }

        //  Returns {ops_per_sec, final_capacity_estimate}.
        minstd::pair<double, size_t> run(size_t total_items, size_t initial_capacity,
                                          size_t growth_numerator = 5, size_t growth_denominator = 4)
        {
            items_per_producer_ = total_items / NumProducers;
            popped_.store(0, minstd::memory_order_relaxed);
            finished_producers_.store(0, minstd::memory_order_relaxed);

            new_delete_test_resource resource;
            alloc_t alloc(&resource);
            queue_t q(alloc, initial_capacity, growth_numerator, growth_denominator);
            queue_ = &q;

            pthread_t producers[NumProducers];
            pthread_t consumers[NumConsumers];

            double start = now_seconds();

            for (size_t i = 0; i < NumProducers; ++i)
            {
                pthread_create(&producers[i], nullptr, produce, new producer_args{this});
            }

            for (size_t i = 0; i < NumConsumers; ++i)
            {
                pthread_create(&consumers[i], nullptr, consume, this);
            }

            for (size_t i = 0; i < NumProducers; ++i)
            {
                pthread_join(producers[i], nullptr);
            }

            for (size_t i = 0; i < NumConsumers; ++i)
            {
                pthread_join(consumers[i], nullptr);
            }

            double elapsed = now_seconds() - start;
            size_t actual_total = items_per_producer_ * NumProducers;

            return {static_cast<double>(actual_total) / elapsed, q.capacity_estimate()};
        }
    };
}

TEST_GROUP (MpScGrowableRingQueuePerformanceTests)
{
};

//  Fixed capacity, sized well above the total item count so no regrowth ever
//  occurs -- isolates steady-state per-push/per-pop cost.
TEST(MpScGrowableRingQueuePerformanceTests, FixedLargeCapacityNoRegrow)
{
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t CAPACITY = TOTAL_ITEMS + 1024;

    perf_report report("MpScGrowableRingQueuePerformanceTests", "FixedLargeCapacityNoRegrow");

    printf("\n=== Fixed capacity (%zu), no regrowth: %zu items ===\n", CAPACITY, TOTAL_ITEMS);
    printf("%-46s : %14s\n", "Scenario", "Items/sec");

    double spsc = run_spsc_baseline(TOTAL_ITEMS, CAPACITY);
    printf("%-46s : %14.0f\n", "spsc_queue (1P, 1C) [baseline]", spsc);
    report.record("spsc_queue baseline (1P,1C)", spsc, 2, TOTAL_ITEMS);

    {
        ring_perf_runner<2, 1, 1> r;
        auto [ops, cap] = r.run(TOTAL_ITEMS, CAPACITY);
        printf("%-46s : %14.0f\n", "ring (1P, 1C)", ops);
        report.record("ring (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }
    {
        ring_perf_runner<2, 5, 1> r;
        auto [ops, cap] = r.run(TOTAL_ITEMS, CAPACITY);
        printf("%-46s : %14.0f\n", "ring (5P, 1C)", ops);
        report.record("ring (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }

    report.finalize();
}

//  Small initial capacity (64), default 1.25x growth: forces many real
//  drain-then-regrow cycles across the run instead of none. Deliberately
//  compared side by side with the fixed-capacity scenario above rather than
//  assumed to be slower -- see the file-level comment for why.
TEST(MpScGrowableRingQueuePerformanceTests, SmallInitialCapacityContinuousGrowth)
{
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t INITIAL_CAPACITY = 64;

    perf_report report("MpScGrowableRingQueuePerformanceTests", "SmallInitialCapacityContinuousGrowth");

    printf("\n=== Small initial capacity (%zu), continuous growth: %zu items ===\n", INITIAL_CAPACITY, TOTAL_ITEMS);
    printf("%-46s : %14s   %s\n", "Scenario", "Items/sec", "Final capacity");

    {
        ring_perf_runner<2, 1, 1> r;
        auto [ops, cap] = r.run(TOTAL_ITEMS, INITIAL_CAPACITY);
        printf("%-46s : %14.0f   %zu\n", "ring (1P, 1C)", ops, cap);
        report.record("ring growing (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * INITIAL_CAPACITY);
    }
    {
        ring_perf_runner<2, 5, 1> r;
        auto [ops, cap] = r.run(TOTAL_ITEMS, INITIAL_CAPACITY);
        printf("%-46s : %14.0f   %zu\n", "ring (5P, 1C)", ops, cap);
        report.record("ring growing (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * INITIAL_CAPACITY);
    }

    report.finalize();
}
