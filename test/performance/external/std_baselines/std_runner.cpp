// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  std + mutex baselines for LockfreeVsStdComparisonPerformanceTests.  Compiled WITHOUT -Iinclude: everything here is
//  the real standard library.  Each function mirrors a lock-free kernel in
//  test/performance/lockfree_vs_std_comparison_test.cpp operation for operation (see comparison/workload.h).

#include "std_runner.h"

#include <atomic>
#include <bitset>
#include <map>
#include <memory_resource>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <vector>

#include <stdlib.h>
#include <string.h>

namespace
{
    using cmp::result;

    //  ---- memory resources ----

    struct malloc_allocator
    {
        void *allocate(size_t bytes) { return malloc(bytes); }
        void deallocate(void *p, size_t) { free(p); }
    };

    struct sync_pool_allocator
    {
        std::pmr::synchronized_pool_resource pool;
        void *allocate(size_t bytes) { return pool.allocate(bytes, 16); }
        void deallocate(void *p, size_t bytes) { pool.deallocate(p, bytes, 16); }
    };

    struct unsync_pool_mutex_allocator
    {
        std::pmr::unsynchronized_pool_resource pool;
        std::mutex mutex;

        void *allocate(size_t bytes)
        {
            std::lock_guard<std::mutex> lock(mutex);
            return pool.allocate(bytes, 16);
        }

        void deallocate(void *p, size_t bytes)
        {
            std::lock_guard<std::mutex> lock(mutex);
            pool.deallocate(p, bytes, 16);
        }
    };

    template <typename allocator_type>
    struct alloc_context
    {
        allocator_type *allocator;
        size_t ops_per_thread;
        size_t threads;
        std::atomic<void *> mailbox[cmp::MAX_THREADS * cmp::ALLOC_LIVE_WINDOW];
        std::atomic<uint64_t> allocations{0};
    };

    template <typename allocator_type>
    void alloc_own_thread(void *arg, size_t tid)
    {
        auto *ctx = static_cast<alloc_context<allocator_type> *>(arg);
        cmp::rng r(tid);
        void *live[cmp::ALLOC_LIVE_WINDOW] = {};
        size_t sizes[cmp::ALLOC_LIVE_WINDOW] = {};
        uint64_t allocations = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            const size_t slot = r.next() % cmp::ALLOC_LIVE_WINDOW;

            if (live[slot] != nullptr)
            {
                ctx->allocator->deallocate(live[slot], sizes[slot]);
                live[slot] = nullptr;
            }
            else
            {
                const size_t size = cmp::alloc_size(r);
                void *p = ctx->allocator->allocate(size);
                if (p != nullptr)
                {
                    *static_cast<volatile char *>(p) = 1;
                    live[slot] = p;
                    sizes[slot] = size;
                    ++allocations;
                }
            }
        }

        for (size_t slot = 0; slot < cmp::ALLOC_LIVE_WINDOW; ++slot)
        {
            if (live[slot] != nullptr)
            {
                ctx->allocator->deallocate(live[slot], sizes[slot]);
            }
        }

        ctx->allocations.fetch_add(allocations);
    }

    //  Every allocation is swapped into a shared mailbox; whatever comes out (usually another thread's block) is freed.
    template <typename allocator_type>
    void alloc_cross_thread(void *arg, size_t tid)
    {
        auto *ctx = static_cast<alloc_context<allocator_type> *>(arg);
        cmp::rng r(tid);
        const size_t mailbox_slots = ctx->threads * cmp::ALLOC_LIVE_WINDOW;
        uint64_t allocations = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            const size_t size = cmp::alloc_size(r);
            void *p = ctx->allocator->allocate(size);
            if (p == nullptr)
            {
                continue;
            }
            *static_cast<size_t *>(p) = size;
            ++allocations;

            void *old = ctx->mailbox[r.next() % mailbox_slots].exchange(p, std::memory_order_acq_rel);
            if (old != nullptr)
            {
                ctx->allocator->deallocate(old, *static_cast<size_t *>(old));
            }
        }

        ctx->allocations.fetch_add(allocations);
    }

    template <typename allocator_type>
    result run_alloc(allocator_type &allocator, int cross, size_t threads, size_t ops_per_thread)
    {
        auto *ctx = new alloc_context<allocator_type>();
        ctx->allocator = &allocator;
        ctx->ops_per_thread = ops_per_thread;
        ctx->threads = threads;
        for (auto &slot : ctx->mailbox)
        {
            slot.store(nullptr);
        }

        const double seconds = cmp::run_parallel(threads, cross ? alloc_cross_thread<allocator_type> : alloc_own_thread<allocator_type>, ctx);

        for (auto &slot : ctx->mailbox)
        {
            void *p = slot.load();
            if (p != nullptr)
            {
                allocator.deallocate(p, *static_cast<size_t *>(p));
            }
        }

        result res{seconds, threads * ops_per_thread, ctx->allocations.load()};
        delete ctx;
        return res;
    }

    //  ---- ordered map ----

    template <typename mutex_type, bool SHARED_FINDS>
    struct map_context
    {
        std::map<uint32_t, uint32_t> map;
        mutex_type mutex;
        unsigned find_percent;
        size_t ops_per_thread;
        std::atomic<uint64_t> successes{0};
    };

    template <typename mutex_type, bool SHARED_FINDS>
    void map_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<map_context<mutex_type, SHARED_FINDS> *>(arg);
        cmp::rng r(tid);
        uint64_t successes = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            const cmp::map_op op = cmp::next_map_op(r, ctx->find_percent);
            const uint32_t key = cmp::next_map_key(r);

            if (op == cmp::MAP_FIND)
            {
                if constexpr (SHARED_FINDS)
                {
                    std::shared_lock<mutex_type> lock(ctx->mutex);
                    successes += (ctx->map.find(key) != ctx->map.end()) ? 1 : 0;
                }
                else
                {
                    std::lock_guard<mutex_type> lock(ctx->mutex);
                    successes += (ctx->map.find(key) != ctx->map.end()) ? 1 : 0;
                }
            }
            else if (op == cmp::MAP_INSERT)
            {
                std::lock_guard<mutex_type> lock(ctx->mutex);
                successes += ctx->map.emplace(key, key).second ? 1 : 0;
            }
            else
            {
                std::lock_guard<mutex_type> lock(ctx->mutex);
                successes += ctx->map.erase(key);
            }
        }

        ctx->successes.fetch_add(successes);
    }

    template <typename mutex_type, bool SHARED_FINDS>
    result run_map(unsigned find_percent, size_t threads, size_t ops_per_thread)
    {
        auto *ctx = new map_context<mutex_type, SHARED_FINDS>();
        ctx->find_percent = find_percent;
        ctx->ops_per_thread = ops_per_thread;

        for (uint32_t key = 0; key < cmp::MAP_KEY_RANGE; key += 2)
        {
            ctx->map.emplace(key, key);
        }

        const double seconds = cmp::run_parallel(threads, map_body<mutex_type, SHARED_FINDS>, ctx);
        result res{seconds, threads * ops_per_thread, ctx->successes.load()};
        delete ctx;
        return res;
    }

    //  ---- queues and stacks ----

    struct mpsc_context
    {
        std::queue<uint64_t> queue;
        std::mutex mutex;
        size_t producers;
        size_t items_per_producer;
        uint64_t checksum = 0;
    };

    void mpsc_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<mpsc_context *>(arg);

        if (tid < ctx->producers)
        {
            for (uint64_t k = 0; k < ctx->items_per_producer; ++k)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->queue.push((static_cast<uint64_t>(tid) << 32) | (k + 1));
            }
            return;
        }

        const size_t total = ctx->producers * ctx->items_per_producer;
        uint64_t sum = 0;

        for (size_t popped = 0; popped < total;)
        {
            std::unique_lock<std::mutex> lock(ctx->mutex);
            if (!ctx->queue.empty())
            {
                sum += ctx->queue.front();
                ctx->queue.pop();
                ++popped;
            }
        }

        ctx->checksum = sum;
    }

    struct spsc_context
    {
        std::queue<uint64_t> queue;
        std::mutex mutex;
        size_t capacity;
        size_t total;
        uint64_t checksum = 0;
    };

    void spsc_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<spsc_context *>(arg);

        if (tid == 0)
        {
            for (uint64_t k = 0; k < ctx->total;)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (ctx->queue.size() < ctx->capacity)
                {
                    ctx->queue.push(++k);
                }
            }
            return;
        }

        uint64_t sum = 0;
        for (size_t popped = 0; popped < ctx->total;)
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (!ctx->queue.empty())
            {
                sum += ctx->queue.front();
                ctx->queue.pop();
                ++popped;
            }
        }
        ctx->checksum = sum;
    }

    struct spmc_context
    {
        std::vector<uint32_t> stack;
        std::mutex mutex;
        size_t capacity;
        size_t total;
        std::atomic<size_t> popped{0};
        std::atomic<uint64_t> checksum{0};
    };

    void spmc_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<spmc_context *>(arg);

        if (tid == 0)
        {
            for (uint32_t k = 0; k < ctx->total;)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (ctx->stack.size() < ctx->capacity)
                {
                    ctx->stack.push_back(++k);
                }
            }
            return;
        }

        uint64_t sum = 0;
        while (ctx->popped.load(std::memory_order_acquire) < ctx->total)
        {
            uint32_t value = 0;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (!ctx->stack.empty())
                {
                    value = ctx->stack.back();
                    ctx->stack.pop_back();
                }
            }
            if (value != 0)
            {
                sum += value;
                ctx->popped.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        ctx->checksum.fetch_add(sum);
    }

    struct pointer_node
    {
        pointer_node *next;
        uint64_t payload;
    };

    struct pointer_stack_context
    {
        std::vector<pointer_node *> stack;
        std::mutex mutex;
        size_t ops_per_thread;
        std::atomic<uint64_t> pops{0};
    };

    void pointer_stack_body(void *arg, size_t)
    {
        auto *ctx = static_cast<pointer_stack_context *>(arg);
        uint64_t pops = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            pointer_node *node = nullptr;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (!ctx->stack.empty())
                {
                    node = ctx->stack.back();
                    ctx->stack.pop_back();
                }
            }
            if (node != nullptr)
            {
                ++pops;
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->stack.push_back(node);
            }
        }

        ctx->pops.fetch_add(pops);
    }

    struct exchange_context
    {
        std::vector<uint64_t> stack;
        std::mutex mutex;
        size_t producers;
        size_t items_per_producer;
        std::atomic<size_t> received{0};
        std::atomic<uint64_t> checksum{0};
    };

    void exchange_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<exchange_context *>(arg);
        const size_t total = ctx->producers * ctx->items_per_producer;

        if (tid < ctx->producers)
        {
            for (uint64_t k = 0; k < ctx->items_per_producer; ++k)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->stack.push_back(tid * ctx->items_per_producer + k + 1);
            }
            return;
        }

        uint64_t sum = 0;
        while (ctx->received.load(std::memory_order_acquire) < total)
        {
            uint64_t value = 0;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (!ctx->stack.empty())
                {
                    value = ctx->stack.back();
                    ctx->stack.pop_back();
                }
            }
            if (value != 0)
            {
                sum += value;
                ctx->received.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        ctx->checksum.fetch_add(sum);
    }

    //  ---- tokens and bit sets ----

    struct token_context
    {
        uint64_t held = 0;
        std::mutex mutex;
        size_t ops_per_thread;
        std::atomic<uint64_t> cycles{0};
    };

    void token_body(void *arg, size_t)
    {
        auto *ctx = static_cast<token_context *>(arg);
        uint64_t cycles = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            size_t token = cmp::TOKENS;
            while (token == cmp::TOKENS)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                const uint64_t free_tokens = ~ctx->held & ((uint64_t{1} << cmp::TOKENS) - 1);
                if (free_tokens != 0)
                {
                    token = static_cast<size_t>(__builtin_ctzll(free_tokens));
                    ctx->held |= uint64_t{1} << token;
                }
            }

            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->held &= ~(uint64_t{1} << token);
            }
            ++cycles;
        }

        ctx->cycles.fetch_add(cycles);
    }

    struct bitset_context
    {
        std::bitset<cmp::BITSET_BITS> bits;
        std::mutex mutex;
        size_t ops_per_thread;
        std::atomic<uint64_t> successes{0};
    };

    void bitset_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<bitset_context *>(arg);
        cmp::rng r(tid);
        uint64_t successes = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            const size_t bit = r.next() % cmp::BITSET_BITS;
            bool acquired = false;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (!ctx->bits.test(bit))
                {
                    ctx->bits.set(bit);
                    acquired = true;
                }
            }
            if (acquired)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->bits.reset(bit);
                ++successes;
            }
        }

        ctx->successes.fetch_add(successes);
    }

    struct bitblock_context
    {
        uint64_t words[cmp::BITBLOCK_BITS / 64] = {};
        size_t hint_word = 0;
        std::mutex mutex;
        size_t ops_per_thread;
        std::atomic<uint64_t> successes{0};

        //  First run of num_bits free bits inside one word, scanning words from the hint (the same word-wise
        //      search bitblock_set does).  Caller holds the mutex.
        bool acquire(size_t num_bits, size_t &start_bit)
        {
            constexpr size_t NUM_WORDS = cmp::BITBLOCK_BITS / 64;

            for (size_t n = 0; n < NUM_WORDS; ++n)
            {
                const size_t w = (hint_word + n) % NUM_WORDS;
                const uint64_t free_bits = ~words[w];
                uint64_t run_starts = free_bits;
                for (size_t j = 1; j < num_bits; ++j)
                {
                    run_starts &= free_bits >> j;
                }
                if (run_starts != 0)
                {
                    const size_t bit = static_cast<size_t>(__builtin_ctzll(run_starts));
                    words[w] |= ((uint64_t{1} << num_bits) - 1) << bit;
                    hint_word = w;
                    start_bit = w * 64 + bit;
                    return true;
                }
            }
            return false;
        }

        void release(size_t start_bit, size_t num_bits)
        {
            words[start_bit / 64] &= ~(((uint64_t{1} << num_bits) - 1) << (start_bit % 64));
        }
    };

    void bitblock_body(void *arg, size_t tid)
    {
        auto *ctx = static_cast<bitblock_context *>(arg);
        cmp::rng r(tid);
        size_t held_start[cmp::BITBLOCK_HELD_PER_THREAD];
        size_t held_bits[cmp::BITBLOCK_HELD_PER_THREAD] = {};
        uint64_t successes = 0;

        for (size_t i = 0; i < ctx->ops_per_thread; ++i)
        {
            const size_t slot = i % cmp::BITBLOCK_HELD_PER_THREAD;
            if (held_bits[slot] != 0)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->release(held_start[slot], held_bits[slot]);
                held_bits[slot] = 0;
            }

            const size_t num_bits = 1 + (r.next() % 4);
            size_t start = 0;
            bool acquired;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                acquired = ctx->acquire(num_bits, start);
            }
            if (acquired)
            {
                held_start[slot] = start;
                held_bits[slot] = num_bits;
                ++successes;
            }
        }

        for (size_t slot = 0; slot < cmp::BITBLOCK_HELD_PER_THREAD; ++slot)
        {
            if (held_bits[slot] != 0)
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->release(held_start[slot], held_bits[slot]);
            }
        }

        ctx->successes.fetch_add(successes);
    }
}

extern "C" cmp::result cmp_std_alloc(int kind, int cross_thread_free, size_t threads, size_t ops_per_thread)
{
    if (kind == CMP_STD_MALLOC)
    {
        malloc_allocator allocator;
        return run_alloc(allocator, cross_thread_free, threads, ops_per_thread);
    }
    if (kind == CMP_STD_SYNC_POOL)
    {
        sync_pool_allocator allocator;
        return run_alloc(allocator, cross_thread_free, threads, ops_per_thread);
    }
    unsync_pool_mutex_allocator allocator;
    return run_alloc(allocator, cross_thread_free, threads, ops_per_thread);
}

extern "C" cmp::result cmp_std_map(int kind, unsigned find_percent, size_t threads, size_t ops_per_thread)
{
    if (kind == CMP_STD_MAP_SHARED_MUTEX)
    {
        return run_map<std::shared_mutex, true>(find_percent, threads, ops_per_thread);
    }
    return run_map<std::mutex, false>(find_percent, threads, ops_per_thread);
}

extern "C" cmp::result cmp_std_mpsc(size_t producers, size_t total_items)
{
    auto *ctx = new mpsc_context();
    ctx->producers = producers;
    ctx->items_per_producer = total_items / producers;
    const double seconds = cmp::run_parallel(producers + 1, mpsc_body, ctx);
    cmp::result res{seconds, producers * ctx->items_per_producer, ctx->checksum};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_spsc(size_t total_items, size_t capacity)
{
    auto *ctx = new spsc_context();
    ctx->capacity = capacity;
    ctx->total = total_items;
    const double seconds = cmp::run_parallel(2, spsc_body, ctx);
    cmp::result res{seconds, total_items, ctx->checksum};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_spmc_stack(size_t consumers, size_t total_items, size_t capacity)
{
    auto *ctx = new spmc_context();
    ctx->stack.reserve(capacity);
    ctx->capacity = capacity;
    ctx->total = total_items;
    const double seconds = cmp::run_parallel(consumers + 1, spmc_body, ctx);
    cmp::result res{seconds, total_items, ctx->checksum.load()};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_pointer_stack(size_t threads, size_t ops_per_thread)
{
    auto *ctx = new pointer_stack_context();
    ctx->ops_per_thread = ops_per_thread;
    std::vector<pointer_node> nodes(threads * 64);
    ctx->stack.reserve(nodes.size());
    for (auto &node : nodes)
    {
        ctx->stack.push_back(&node);
    }
    const double seconds = cmp::run_parallel(threads, pointer_stack_body, ctx);
    cmp::result res{seconds, threads * ops_per_thread, ctx->pops.load()};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_exchange(size_t threads, size_t items_per_producer)
{
    auto *ctx = new exchange_context();
    ctx->producers = threads / 2;
    ctx->items_per_producer = items_per_producer;
    ctx->stack.reserve(1024);
    const double seconds = cmp::run_parallel(threads, exchange_body, ctx);
    cmp::result res{seconds, ctx->producers * items_per_producer, ctx->checksum.load()};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_tokens(size_t threads, size_t ops_per_thread)
{
    auto *ctx = new token_context();
    ctx->ops_per_thread = ops_per_thread;
    const double seconds = cmp::run_parallel(threads, token_body, ctx);
    cmp::result res{seconds, threads * ops_per_thread, ctx->cycles.load()};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_bitset(size_t threads, size_t ops_per_thread)
{
    auto *ctx = new bitset_context();
    ctx->ops_per_thread = ops_per_thread;
    const double seconds = cmp::run_parallel(threads, bitset_body, ctx);
    cmp::result res{seconds, threads * ops_per_thread, ctx->successes.load()};
    delete ctx;
    return res;
}

extern "C" cmp::result cmp_std_bitblock(size_t threads, size_t ops_per_thread)
{
    auto *ctx = new bitblock_context();
    ctx->ops_per_thread = ops_per_thread;
    const double seconds = cmp::run_parallel(threads, bitblock_body, ctx);
    cmp::result res{seconds, threads * ops_per_thread, ctx->successes.load()};
    delete ctx;
    return res;
}
