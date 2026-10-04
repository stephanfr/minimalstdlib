// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <lockfree/sp_mc_value_stack>

#include "../shared/perf_report.h"
#include "../shared/perf_test_config.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(SpMcValueStackPerformanceTests)
    {
    };
#pragma GCC diagnostic pop

    static constexpr size_t STACK_CAPACITY = 1 << 16;

    double now_seconds()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

    //  -----------------------------------------------------------------------
    //  Single-threaded throughput: alternating push/pop pairs.
    //  -----------------------------------------------------------------------

    TEST(SpMcValueStackPerformanceTests, SingleThreadedThroughput)
    {
        const size_t iters = perf_config_get_iterations(
            "SPMC_PERF_ITERS", 1000000, 1, 1000);

        minstd::lockfree::sp_mc_value_stack<uint32_t, STACK_CAPACITY> stack;

        double start = now_seconds();

        for (size_t i = 0; i < iters; i++)
        {
            stack.push(static_cast<uint32_t>(i));
            uint32_t v;
            stack.pop(v);
        }

        double elapsed = now_seconds() - start;
        double ops_per_sec = static_cast<double>(iters * 2) / elapsed;

        perf_report report("SpMcValueStackPerformanceTests", "SingleThreadedThroughput");
        report.record("1 thread (push+pop pairs)", ops_per_sec, 1, iters);
        report.finalize();

        printf("\n  [sp_mc_value_stack] 1 thread: %.1f M ops/s  (%zu iters)\n",
               ops_per_sec / 1e6, iters);
    }

    //  -----------------------------------------------------------------------
    //  Multi-consumer throughput: 1 producer, N consumer threads.
    //  Producer fills the stack in batches; consumers drain concurrently.
    //  -----------------------------------------------------------------------

    struct mc_bench_args
    {
        minstd::lockfree::sp_mc_value_stack<uint32_t, STACK_CAPACITY> *stack;
        minstd::atomic<bool>  done{false};
        minstd::atomic<size_t> total_popped{0};
    };

    void *mc_consumer(void *arg)
    {
        mc_bench_args *a = static_cast<mc_bench_args *>(arg);
        size_t local = 0;

        while (!a->done.load(minstd::memory_order_acquire) || !a->stack->empty())
        {
            uint32_t v;
            if (a->stack->pop(v))
            {
                local++;
            }
        }

        a->total_popped.fetch_add(local, minstd::memory_order_relaxed);
        return nullptr;
    }

    void run_mc_bench(size_t num_consumers, size_t iters, perf_report &report)
    {
        static minstd::lockfree::sp_mc_value_stack<uint32_t, STACK_CAPACITY> stack;

        mc_bench_args args;
        args.stack = &stack;

        pthread_t threads[8];
        for (size_t i = 0; i < num_consumers; i++)
        {
            pthread_create(&threads[i], nullptr, mc_consumer, &args);
        }

        double start = now_seconds();

        for (size_t i = 0; i < iters; i++)
        {
            while (!stack.push(static_cast<uint32_t>(i)))
            {
            }
        }

        args.done.store(true, minstd::memory_order_release);

        for (size_t i = 0; i < num_consumers; i++)
        {
            pthread_join(threads[i], nullptr);
        }

        double elapsed = now_seconds() - start;
        double ops_per_sec = static_cast<double>(iters) / elapsed;

        char label[64];
        snprintf(label, sizeof(label), "%zu consumer thread(s)", num_consumers);
        report.record(label, ops_per_sec, num_consumers, iters);

        printf("  [sp_mc_value_stack] %zu consumer(s): %.1f M push/s  (%zu iters, "
               "%zu popped)\n",
               num_consumers, ops_per_sec / 1e6, iters,
               args.total_popped.load());
    }

    TEST(SpMcValueStackPerformanceTests, MultiConsumerThroughput)
    {
        const size_t iters = perf_config_get_iterations(
            "SPMC_PERF_ITERS", 500000, 1, 1000);

        perf_report report("SpMcValueStackPerformanceTests", "MultiConsumerThroughput");
        printf("\n");

        run_mc_bench(1, iters, report);
        run_mc_bench(2, iters, report);
        run_mc_bench(4, iters, report);

        report.finalize();
    }
}
