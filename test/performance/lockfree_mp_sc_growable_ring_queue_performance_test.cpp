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

#include "external/mp_sc_growable_ring_queue/jiffy_queue.h"
#include "external/mp_sc_growable_ring_queue/moodycamel_runner.h"

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

    constexpr size_t DEFAULT_WARMUP_ITEMS = 4'000'000;
    constexpr size_t DEFAULT_TOTAL_ITEMS = 40'000'000;

    //  ---- original spsc_queue: fixed baseline, unaffected by anything below ----
    double run_spsc_baseline(size_t warmup_items, size_t total_items, size_t capacity)
    {
        using queue_t = minstd::spsc_queue<perf_item>;

        new_delete_test_resource resource;
        minstd::pmr::polymorphic_allocator<perf_item> alloc(&resource);
        queue_t q(alloc, capacity);

        auto do_run = [&](size_t items) {
            if (items == 0) return 0.0;
            struct thread_args
            {
                queue_t *q;
                size_t total;
            } args{&q, items};

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

            return now_seconds() - start;
        };

        if (warmup_items > 0)
            do_run(warmup_items);

        double elapsed = do_run(total_items);
        return static_cast<double>(total_items) / elapsed;
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
        minstd::pair<double, size_t> run(queue_t& q, size_t total_items)
        {
            items_per_producer_ = total_items / NumProducers;
            popped_.store(0, minstd::memory_order_relaxed);
            finished_producers_.store(0, minstd::memory_order_relaxed);

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
    constexpr size_t WARMUP_ITEMS = DEFAULT_WARMUP_ITEMS;
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t CAPACITY = WARMUP_ITEMS + TOTAL_ITEMS + 1024;

    perf_report report("MpScGrowableRingQueuePerformanceTests", "FixedLargeCapacityNoRegrow");

    printf("\n=== Fixed capacity (%zu), no regrowth: %zu items (after %zu warmup) ===\n", CAPACITY, TOTAL_ITEMS, WARMUP_ITEMS);
    printf("%-46s : %14s\n", "Scenario", "Items/sec");

    double spsc = run_spsc_baseline(WARMUP_ITEMS, TOTAL_ITEMS, CAPACITY);
    printf("%-46s : %14.0f\n", "spsc_queue (1P, 1C) [baseline]", spsc);
    report.record("spsc_queue baseline (1P,1C)", spsc, 2, TOTAL_ITEMS);

    {
        ring_perf_runner<2, 1, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<2, 1, 1>::alloc_t alloc(&resource);
        ring_perf_runner<2, 1, 1>::queue_t q(alloc, CAPACITY);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        printf("%-46s : %14.0f\n", "ring (1P, 1C)", ops);
        report.record("ring (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }
    {
        ring_perf_runner<8, 5, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<8, 5, 1>::alloc_t alloc(&resource);
        ring_perf_runner<8, 5, 1>::queue_t q(alloc, CAPACITY / 4);  // Adjust capacity per segment
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
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
    constexpr size_t WARMUP_ITEMS = DEFAULT_WARMUP_ITEMS;
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t INITIAL_CAPACITY = 64;

    perf_report report("MpScGrowableRingQueuePerformanceTests", "SmallInitialCapacityContinuousGrowth");

    printf("\n=== Small initial capacity (%zu), continuous growth: %zu items (after %zu warmup) ===\n", INITIAL_CAPACITY, TOTAL_ITEMS, WARMUP_ITEMS);
    printf("%-46s : %14s   %s\n", "Scenario", "Items/sec", "Final capacity");

    {
        ring_perf_runner<2, 1, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<2, 1, 1>::alloc_t alloc(&resource);
        ring_perf_runner<2, 1, 1>::queue_t q(alloc, INITIAL_CAPACITY);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        printf("%-46s : %14.0f   %zu\n", "ring (1P, 1C)", ops, cap);
        report.record("ring growing (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * INITIAL_CAPACITY);
    }
    {
        ring_perf_runner<2, 5, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<2, 5, 1>::alloc_t alloc(&resource);
        ring_perf_runner<2, 5, 1>::queue_t q(alloc, INITIAL_CAPACITY);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        printf("%-46s : %14.0f   %zu\n", "ring (5P, 1C)", ops, cap);
        report.record("ring growing (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * INITIAL_CAPACITY);
    }

    report.finalize();
}


// ---------------------------------------------------------------------------
//  External comparison tests — all three MPSC implementations, same load
//
//  Reference implementations under external/mp_sc_growable_ring_queue/:
//    Jiffy      — DolevAdas/Jiffy, wait-free MPSC, fixed 1620-slot segments
//    Moodycamel — cameron314/concurrentqueue, lock-free MPMC used in MPSC
//                 mode with ProducerToken/ConsumerToken; runner compiled in a
//                 separate TU without -Iinclude (needs real std::atomic).
// ---------------------------------------------------------------------------
namespace
{
    template <size_t NumProducers>
    struct jiffy_perf_runner
    {
        using queue_t = JiffyMpScQueue<perf_item>;

        queue_t *queue_ = nullptr;
        size_t items_per_producer_ = 0;
        minstd::atomic<uint64_t> popped_{0};
        minstd::atomic<uint64_t> finished_producers_{0};

        struct producer_args { jiffy_perf_runner *self; };

        static void *produce(void *arg)
        {
            auto *a = static_cast<producer_args *>(arg);
            jiffy_perf_runner *self = a->self;
            delete a;

            for (size_t i = 0; i < self->items_per_producer_; ++i)
                self->queue_->enqueue(perf_item{i});

            self->finished_producers_.fetch_add(1, minstd::memory_order_acq_rel);
            return nullptr;
        }

        static void *consume(void *arg)
        {
            auto *self = static_cast<jiffy_perf_runner *>(arg);
            size_t total = self->items_per_producer_ * NumProducers;

            while (true)
            {
                perf_item it{0};
                if (self->queue_->dequeue(it))
                {
                    self->popped_.fetch_add(1, minstd::memory_order_acq_rel);
                }
                else if (self->finished_producers_.load(minstd::memory_order_acquire) == NumProducers &&
                         self->popped_.load(minstd::memory_order_acquire) == total)
                {
                    break;
                }
            }
            return nullptr;
        }

        double run(queue_t& q, size_t total_items)
        {
            items_per_producer_ = total_items / NumProducers;
            popped_.store(0, minstd::memory_order_relaxed);
            finished_producers_.store(0, minstd::memory_order_relaxed);

            queue_ = &q;

            pthread_t producers[NumProducers];
            pthread_t consumer;

            double start = now_seconds();

            for (size_t i = 0; i < NumProducers; ++i)
                pthread_create(&producers[i], nullptr, produce, new producer_args{this});

            pthread_create(&consumer, nullptr, consume, this);

            for (size_t i = 0; i < NumProducers; ++i)
                pthread_join(producers[i], nullptr);

            pthread_join(consumer, nullptr);

            size_t actual_total = items_per_producer_ * NumProducers;
            return static_cast<double>(actual_total) / (now_seconds() - start);
        }
    };
}

TEST_GROUP(MpScGrowableRingQueueExternalComparisonPerformanceTests){};

//  All three queues, fixed ring capacity (no regrowth), 1P/1C and 5P/1C.
TEST(MpScGrowableRingQueueExternalComparisonPerformanceTests, FixedLargeCapacityNoRegrow)
{
    constexpr size_t WARMUP_ITEMS = DEFAULT_WARMUP_ITEMS;
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t CAPACITY = WARMUP_ITEMS + TOTAL_ITEMS + 1024;

    perf_report report("MpScGrowableRingQueueExternalComparisonPerformanceTests", "FixedLargeCapacityNoRegrow");

    printf("\n=== Ring (fixed %zu) vs Jiffy vs Moodycamel: %zu items (after %zu warmup) ===\n", CAPACITY, TOTAL_ITEMS, WARMUP_ITEMS);
    printf("%-50s : %14s\n", "Scenario", "Items/sec");

    double ring_1p, ring_5p;

    {
        ring_perf_runner<2, 1, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<2, 1, 1>::alloc_t alloc(&resource);
        ring_perf_runner<2, 1, 1>::queue_t q(alloc, CAPACITY);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        ring_1p = ops;
        printf("%-50s : %14.0f\n", "ring fixed (1P, 1C)", ops);
        report.record("ring fixed (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }
    {
        ring_perf_runner<8, 5, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<8, 5, 1>::alloc_t alloc(&resource);
        ring_perf_runner<8, 5, 1>::queue_t q(alloc, CAPACITY / 4);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        ring_5p = ops;
        printf("%-50s : %14.0f\n", "ring fixed (5P, 1C)", ops);
        report.record("ring fixed (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }
    {
        jiffy_perf_runner<1> r;
        jiffy_perf_runner<1>::queue_t q;
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        double jiffy = r.run(q, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "jiffy (1P, 1C)", jiffy);
        double delta = 100.0 * (jiffy - ring_1p) / ring_1p;
        report.record_with_baseline("jiffy (1P,1C)", jiffy, ring_1p, delta,
                                    jiffy / ring_1p, 2, TOTAL_ITEMS);
        CHECK_TRUE(jiffy > 0);
    }
    {
        jiffy_perf_runner<5> r;
        jiffy_perf_runner<5>::queue_t q;
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        double jiffy = r.run(q, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "jiffy (5P, 1C)", jiffy);
        double delta = 100.0 * (jiffy - ring_5p) / ring_5p;
        report.record_with_baseline("jiffy (5P,1C)", jiffy, ring_5p, delta,
                                    jiffy / ring_5p, 6, TOTAL_ITEMS);
        CHECK_TRUE(jiffy > 0);
    }
    {
        double mc = run_moodycamel(1, WARMUP_ITEMS, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "moodycamel (1P, 1C)", mc);
        double delta = 100.0 * (mc - ring_1p) / ring_1p;
        report.record_with_baseline("moodycamel (1P,1C)", mc, ring_1p, delta,
                                    mc / ring_1p, 2, TOTAL_ITEMS);
        CHECK_TRUE(mc > 0);
    }
    {
        double mc = run_moodycamel(5, WARMUP_ITEMS, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "moodycamel (5P, 1C)", mc);
        double delta = 100.0 * (mc - ring_5p) / ring_5p;
        report.record_with_baseline("moodycamel (5P,1C)", mc, ring_5p, delta,
                                    mc / ring_5p, 6, TOTAL_ITEMS);
        CHECK_TRUE(mc > 0);
    }

    report.finalize();
}

//  All three queues, ring starting at capacity 64 with continuous regrowth.
TEST(MpScGrowableRingQueueExternalComparisonPerformanceTests, SmallInitialCapacityContinuousGrowth)
{
    constexpr size_t WARMUP_ITEMS = DEFAULT_WARMUP_ITEMS;
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t INITIAL_CAPACITY = 64;

    perf_report report("MpScGrowableRingQueueExternalComparisonPerformanceTests", "SmallInitialCapacityContinuousGrowth");

    printf("\n=== Ring (initial %zu, growing) vs Jiffy vs Moodycamel: %zu items (after %zu warmup) ===\n", INITIAL_CAPACITY, TOTAL_ITEMS, WARMUP_ITEMS);
    printf("%-50s : %14s\n", "Scenario", "Items/sec");

    double ring_1p, ring_5p;

    {
        ring_perf_runner<2, 1, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<2, 1, 1>::alloc_t alloc(&resource);
        ring_perf_runner<2, 1, 1>::queue_t q(alloc, INITIAL_CAPACITY);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        ring_1p = ops;
        printf("%-50s : %14.0f\n", "ring growing (1P, 1C)", ops);
        report.record("ring growing (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * INITIAL_CAPACITY);
    }
    {
        ring_perf_runner<8, 5, 1> r;
        new_delete_test_resource resource;
        ring_perf_runner<8, 5, 1>::alloc_t alloc(&resource);
        ring_perf_runner<8, 5, 1>::queue_t q(alloc, INITIAL_CAPACITY);
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        auto [ops, cap] = r.run(q, TOTAL_ITEMS);
        ring_5p = ops;
        printf("%-50s : %14.0f\n", "ring growing (5P, 1C)", ops);
        report.record("ring growing (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * INITIAL_CAPACITY);
    }
    {
        jiffy_perf_runner<1> r;
        jiffy_perf_runner<1>::queue_t q;
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        double jiffy = r.run(q, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "jiffy (1P, 1C)", jiffy);
        double delta = 100.0 * (jiffy - ring_1p) / ring_1p;
        report.record_with_baseline("jiffy (1P,1C)", jiffy, ring_1p, delta,
                                    jiffy / ring_1p, 2, TOTAL_ITEMS);
        CHECK_TRUE(jiffy > 0);
    }
    {
        jiffy_perf_runner<5> r;
        jiffy_perf_runner<5>::queue_t q;
        if (WARMUP_ITEMS > 0) r.run(q, WARMUP_ITEMS);
        double jiffy = r.run(q, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "jiffy (5P, 1C)", jiffy);
        double delta = 100.0 * (jiffy - ring_5p) / ring_5p;
        report.record_with_baseline("jiffy (5P,1C)", jiffy, ring_5p, delta,
                                    jiffy / ring_5p, 6, TOTAL_ITEMS);
        CHECK_TRUE(jiffy > 0);
    }
    {
        double mc = run_moodycamel(1, WARMUP_ITEMS, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "moodycamel (1P, 1C)", mc);
        double delta = 100.0 * (mc - ring_1p) / ring_1p;
        report.record_with_baseline("moodycamel (1P,1C)", mc, ring_1p, delta,
                                    mc / ring_1p, 2, TOTAL_ITEMS);
        CHECK_TRUE(mc > 0);
    }
    {
        double mc = run_moodycamel(5, WARMUP_ITEMS, TOTAL_ITEMS);
        printf("%-50s : %14.0f\n", "moodycamel (5P, 1C)", mc);
        double delta = 100.0 * (mc - ring_5p) / ring_5p;
        report.record_with_baseline("moodycamel (5P,1C)", mc, ring_5p, delta,
                                    mc / ring_5p, 6, TOTAL_ITEMS);
        CHECK_TRUE(mc > 0);
    }

    report.finalize();
}
