// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Workloads shared by the lock-free vs. std + mutex comparison (LockfreeVsStdComparisonPerformanceTests).
//
//  This header is compiled twice: into the lock-free side (with the minstd headers) and into the std baseline
//  runner (test/performance/external/std_baselines, against the real standard library).  It therefore uses only
//  C headers and pthreads.  Both sides run the same operation sequences: same per-thread random streams, sizes,
//  key ranges, op mixes and counts, timed the same way.

#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace cmp
{
    constexpr size_t MAX_THREADS = 8;

    struct result
    {
        double seconds;
        uint64_t ops;      //  operations performed (the throughput numerator)
        uint64_t checksum; //  work actually done: allocations made, items moved, successful operations ...
    };

    //  xorshift64; one stream per thread, seeded by the thread index.
    struct rng
    {
        uint64_t state;

        explicit rng(uint64_t seed) : state((seed + 1) * 0x9E3779B97F4A7C15ull) {}

        uint64_t next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }
    };

    inline double now_seconds()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

    //  ---- memory resources: per-thread churn over a window of live blocks ----

    constexpr size_t ALLOC_LIVE_WINDOW = 64;

    inline size_t alloc_size(rng &r) { return 16 + static_cast<size_t>(r.next() % 241); } //  16 .. 256 bytes

    //  ---- ordered map ----

    constexpr uint32_t MAP_KEY_RANGE = 65536;

    enum map_op
    {
        MAP_FIND,
        MAP_INSERT,
        MAP_REMOVE
    };

    inline map_op next_map_op(rng &r, unsigned find_percent)
    {
        const uint64_t v = r.next();
        if ((v % 100) < find_percent)
        {
            return MAP_FIND;
        }
        return ((v >> 32) & 1) ? MAP_INSERT : MAP_REMOVE;
    }

    inline uint32_t next_map_key(rng &r) { return static_cast<uint32_t>(r.next() % MAP_KEY_RANGE); }

    //  ---- bit sets ----

    constexpr size_t BITSET_BITS = 4096;
    constexpr size_t BITBLOCK_BITS = 16384;
    constexpr size_t BITBLOCK_HELD_PER_THREAD = 32;

    //  ---- token dispenser ----

    constexpr size_t TOKENS = 8;

    //  ---- parallel runner: threads start together behind a barrier; the clock covers start to last join ----

    typedef void (*thread_body)(void *context, size_t thread_index);

    struct parallel_state
    {
        pthread_barrier_t barrier;
        thread_body body;
        void *context;
    };

    struct thread_slot
    {
        parallel_state *state;
        size_t index;
    };

    inline void *parallel_trampoline(void *arg)
    {
        thread_slot *slot = static_cast<thread_slot *>(arg);
        pthread_barrier_wait(&slot->state->barrier);
        slot->state->body(slot->state->context, slot->index);
        return nullptr;
    }

    //  Runs body(context, i) on num_threads threads; returns the wall time from the common start to the last join.
    inline double run_parallel(size_t num_threads, thread_body body, void *context)
    {
        parallel_state state;
        state.body = body;
        state.context = context;
        pthread_barrier_init(&state.barrier, nullptr, static_cast<unsigned>(num_threads + 1));

        pthread_t threads[MAX_THREADS + 1];
        thread_slot slots[MAX_THREADS + 1];

        for (size_t i = 0; i < num_threads; ++i)
        {
            slots[i].state = &state;
            slots[i].index = i;
            pthread_create(&threads[i], nullptr, parallel_trampoline, &slots[i]);
        }

        pthread_barrier_wait(&state.barrier);
        const double start = now_seconds();

        for (size_t i = 0; i < num_threads; ++i)
        {
            pthread_join(threads[i], nullptr);
        }

        const double elapsed = now_seconds() - start;
        pthread_barrier_destroy(&state.barrier);
        return elapsed;
    }
}
