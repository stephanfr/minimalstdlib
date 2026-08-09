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

//  Two scenarios are exercised because they stress genuinely different parts
//  of the implementation:
//
//  - FixedLargeCapacityNoRegrow: capacity pre-sized above the item count so
//    NO regrow ever happens -- isolates steady-state per-operation cost.
//  - GrowthStrategies: the drain-then-regrow path, comparing three ways to
//    size a growing queue. The important finding it captures: a queue that
//    starts tiny and grows UNBOUNDED pays heavy churn -- dozens of
//    reallocate + reconstruct + free regrows during the run, and it grows
//    past cache -- so it is the SLOWEST configuration, not the fastest.
//    (An earlier version of this comment claimed continuous growth was
//    faster than fixed capacity; that was measured with a warmup that
//    amortized the growth churn out of the timed window. Cold, as measured
//    here and as callers actually experience it on first use, unbounded
//    growth-from-tiny is slower. Retracted.) Starting from a sane initial
//    capacity, and/or setting a max_capacity ceiling so growth stops and
//    the ring stays cache-resident, recovers and usually beats the fixed
//    number -- see the header's max_capacity note.
//
//  Absolute numbers are highly machine-specific (allocation cost, cache
//  size, memory bandwidth, core count); the RELATIVE ordering within a
//  scenario is what to read.
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
                                          size_t growth_numerator = 5, size_t growth_denominator = 4,
                                          size_t max_capacity = static_cast<size_t>(-1))
        {
            items_per_producer_ = total_items / NumProducers;
            popped_.store(0, minstd::memory_order_relaxed);
            finished_producers_.store(0, minstd::memory_order_relaxed);

            new_delete_test_resource resource;
            alloc_t alloc(&resource);
            queue_t q(alloc, initial_capacity, growth_numerator, growth_denominator, 0, max_capacity);
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

    //  Best-of-N throughput for one configuration. A relative comparison
    //  between growth strategies is easily inverted by a single noisy run
    //  (scheduler hiccup, other tenants), so report the best (least-
    //  interfered) run rather than a single shot. Final capacity is
    //  deterministic for a given initial/ceiling, so any run's is fine.
    template <size_t SegmentCount, size_t NumProducers, size_t NumConsumers>
    minstd::pair<double, size_t> best_of(int rounds, size_t total_items, size_t initial_capacity,
                                         size_t max_capacity = static_cast<size_t>(-1))
    {
        double best_ops = 0.0;
        size_t cap = 0;
        for (int i = 0; i < rounds; ++i)
        {
            ring_perf_runner<SegmentCount, NumProducers, NumConsumers> r;
            auto [ops, c] = r.run(total_items, initial_capacity, 5, 4, max_capacity);
            if (ops > best_ops)
            {
                best_ops = ops;
            }
            cap = c;
        }
        return {best_ops, cap};
    }
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

//  Growth strategies compared, at 1P/1C and (heavier) 5P/1C. Three ways to
//  size a growing queue:
//
//    1. "grow from tiny, unbounded" -- starts at 64 and ratchets 1.25x on
//       every full drain. WORST case: dozens of reallocate+reconstruct+free
//       regrows during the run, and it grows past cache. Included only as a
//       cautionary baseline; do NOT ship this configuration under sustained
//       load.
//    2. "grow from sane initial, unbounded" -- a realistic starting
//       capacity, so the queue does a handful of regrows instead of dozens.
//    3. "grow from tiny, ceiling" -- small start but a max_capacity ceiling,
//       so growth stops after a bounded number of regrows and the ring stays
//       cache-resident. The RECOMMENDED sustained-load configuration, and
//       typically the fastest of the three while also bounding memory.
//
//  Read the RELATIVE ordering, not the absolute numbers (see the file-level
//  comment). The expected story: (1) is clearly slowest; (2) and (3) recover
//  most of the loss; (3) also caps memory (final capacity <= SegmentCount x
//  CEILING).
TEST(MpScGrowableRingQueuePerformanceTests, GrowthStrategies)
{
    constexpr size_t TOTAL_ITEMS = DEFAULT_TOTAL_ITEMS;
    constexpr size_t TINY = 64;
    constexpr size_t SANE = 262144;         // ~256K slots: a realistic starting point
    constexpr size_t CEILING = 1u << 20;    // ~1M slots: keeps the ring near L3

    perf_report report("MpScGrowableRingQueuePerformanceTests", "GrowthStrategies");

    constexpr int ROUNDS = 3; // best-of-3 per row, so noise can't invert the ordering

    printf("\n=== Growth strategies (best of %d): %zu items ===\n", ROUNDS, TOTAL_ITEMS);
    printf("%-46s : %14s   %s\n", "Scenario", "Items/sec", "Final capacity");

    {
        auto [ops, cap] = best_of<2, 1, 1>(ROUNDS, TOTAL_ITEMS, TINY);
        printf("%-46s : %14.0f   %zu\n", "ring grow-from-tiny UNBOUNDED (1P, 1C)", ops, cap);
        report.record("ring grow tiny unbounded (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * TINY);
    }
    {
        auto [ops, cap] = best_of<2, 1, 1>(ROUNDS, TOTAL_ITEMS, SANE);
        printf("%-46s : %14.0f   %zu\n", "ring grow-from-sane unbounded (1P, 1C)", ops, cap);
        report.record("ring grow sane unbounded (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }
    {
        auto [ops, cap] = best_of<2, 1, 1>(ROUNDS, TOTAL_ITEMS, TINY, CEILING);
        printf("%-46s : %14.0f   %zu\n", "ring grow-from-tiny CEILING 1M (1P, 1C)", ops, cap);
        report.record("ring grow tiny ceiling (1P,1C)", ops, 2, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap <= 2 * CEILING); // ceiling honored: bounded memory
    }

    {
        auto [ops, cap] = best_of<2, 5, 1>(ROUNDS, TOTAL_ITEMS, TINY);
        printf("%-46s : %14.0f   %zu\n", "ring grow-from-tiny UNBOUNDED (5P, 1C)", ops, cap);
        report.record("ring grow tiny unbounded (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap > 2 * TINY);
    }
    {
        auto [ops, cap] = best_of<2, 5, 1>(ROUNDS, TOTAL_ITEMS, SANE);
        printf("%-46s : %14.0f   %zu\n", "ring grow-from-sane unbounded (5P, 1C)", ops, cap);
        report.record("ring grow sane unbounded (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
    }
    {
        auto [ops, cap] = best_of<2, 5, 1>(ROUNDS, TOTAL_ITEMS, TINY, CEILING);
        printf("%-46s : %14.0f   %zu\n", "ring grow-from-tiny CEILING 1M (5P, 1C)", ops, cap);
        report.record("ring grow tiny ceiling (5P,1C)", ops, 6, TOTAL_ITEMS);
        CHECK_TRUE(ops > 0);
        CHECK_TRUE(cap <= 2 * CEILING); // ceiling honored: bounded memory
    }

    report.finalize();
}
