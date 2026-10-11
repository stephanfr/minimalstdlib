// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  C-linkage interface to the std + mutex baselines (std_runner.cpp).  That file is compiled WITHOUT -Iinclude, so
//  <map>, <mutex>, <memory_resource> ... are the real standard library, not the minstd overlay.

#pragma once

#include "../../comparison/workload.h"

extern "C"
{
    enum cmp_std_alloc_kind
    {
        CMP_STD_MALLOC = 0,         //  glibc malloc/free (internally thread-safe)
        CMP_STD_SYNC_POOL = 1,      //  std::pmr::synchronized_pool_resource
        CMP_STD_UNSYNC_POOL_MUTEX = 2 //  std::pmr::unsynchronized_pool_resource + std::mutex
    };

    enum cmp_std_map_kind
    {
        CMP_STD_MAP_SHARED_MUTEX = 0, //  std::map + std::shared_mutex (finds shared)
        CMP_STD_MAP_MUTEX = 1         //  std::map + std::mutex
    };

    cmp::result cmp_std_alloc(int kind, int cross_thread_free, size_t threads, size_t ops_per_thread);
    cmp::result cmp_std_map(int kind, unsigned find_percent, size_t threads, size_t ops_per_thread);
    cmp::result cmp_std_mpsc(size_t producers, size_t total_items);
    cmp::result cmp_std_spsc(size_t total_items, size_t capacity);
    cmp::result cmp_std_spmc_stack(size_t consumers, size_t total_items, size_t capacity);
    cmp::result cmp_std_pointer_stack(size_t threads, size_t ops_per_thread);
    cmp::result cmp_std_exchange(size_t threads, size_t items_per_producer);
    cmp::result cmp_std_tokens(size_t threads, size_t ops_per_thread);
    cmp::result cmp_std_bitset(size_t threads, size_t ops_per_thread);
    cmp::result cmp_std_bitblock(size_t threads, size_t ops_per_thread);
}
