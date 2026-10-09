// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include "atomic"
#include "new"

#include "lockfree/intrusive_tagged_stack"
#include "lockfree/tagged_ptr"
#include "memory_resource.h"

#include <stdint.h>

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        class lockfree_static_arena_resource : public memory_resource
        {
        private:
            struct free_block_node
            {
                size_t size;
                free_block_node* next;
            };

            using tagged_ptr_type = lockfree::tagged_ptr<free_block_node>;
            using stack_type = lockfree::intrusive_tagged_stack<free_block_node, tagged_ptr_type, free_block_node*, &free_block_node::next>;

            uint8_t* arena_start_;
            uint8_t* arena_end_;
            atomic<uint8_t*> current_ptr_;

            static constexpr size_t NUM_BINS = 8;
            static constexpr size_t MIN_ALIGNMENT = 64;
            static constexpr size_t LARGEST_CLASS = 4096; //  bins 0-6 hold exactly 64, 128, ... 4096 bytes; bin 7 holds larger blocks
            alignas(64) atomic<typename tagged_ptr_type::storage_type> free_stack_heads_[NUM_BINS];

            //  The size actually reserved for a request.  Up to 4 KiB every request is rounded up to a whole size
            //      class, so any block in a class bin fits any request for that bin.  Larger requests are rounded
            //      to MIN_ALIGNMENT and kept in bin 7, which is searched best-fit.  Both allocate and deallocate
            //      use this, so a block always returns to the bin it came from.
            static size_t block_size_for(size_t bytes)
            {
                size_t size = (bytes < sizeof(free_block_node)) ? sizeof(free_block_node) : bytes;
                size = (size + (MIN_ALIGNMENT - 1)) & ~(MIN_ALIGNMENT - 1);

                if (size > LARGEST_CLASS) {
                    return size;
                }

                size_t class_size = MIN_ALIGNMENT;
                while (class_size < size) {
                    class_size <<= 1;
                }
                return class_size;
            }

            //  block_size must come from block_size_for().
            static size_t get_bin(size_t block_size) {
                if (block_size > LARGEST_CLASS) return NUM_BINS - 1;

                size_t bin = 0;
                for (size_t class_size = MIN_ALIGNMENT; class_size < block_size; class_size <<= 1) {
                    ++bin;
                }
                return bin;
            }

            // adapter for intrusive stack
            struct adapter_type {
                free_block_node* to_link(free_block_node* ptr) const { return ptr; }
                free_block_node* to_node(free_block_node* ptr) const { return ptr; }
            };

            // backoff adapter for intrusive stack
            struct backoff_type {
                void operator()(size_t& retries) const {
                    retries++;
                }
            };

        public:
            lockfree_static_arena_resource(void* start, size_t size)
                : arena_start_(static_cast<uint8_t*>(start)),
                  arena_end_(arena_start_ + size),
                  current_ptr_(arena_start_)
            {
                for (size_t i = 0; i < NUM_BINS; ++i) {
                    free_stack_heads_[i].store(0, memory_order_relaxed);
                }
            }

            ~lockfree_static_arena_resource() override = default;

        protected:
            void* do_allocate(size_t bytes, size_t alignment) override
            {
                if (alignment > MIN_ALIGNMENT) {
                    return nullptr;
                }

                const size_t alloc_size = block_size_for(bytes);

                // Bump pointer allocation
                uint8_t* expected = current_ptr_.load(memory_order_relaxed);
                while (true)
                {
                    uintptr_t ptr_as_int = reinterpret_cast<uintptr_t>(expected);
                    uintptr_t aligned_int = (ptr_as_int + MIN_ALIGNMENT - 1) & ~(MIN_ALIGNMENT - 1);
                    uint8_t* aligned_expected = reinterpret_cast<uint8_t*>(aligned_int);

                    uint8_t* new_ptr = aligned_expected + alloc_size;

                    if (new_ptr > arena_end_) {
                        break;
                    }

                    if (current_ptr_.compare_exchange_weak(expected, new_ptr, memory_order_acquire, memory_order_relaxed))
                    {
                        return aligned_expected;
                    }
                }

                // Fallback: Check free stacks
                adapter_type adapter;
                backoff_type backoff;

                const size_t start_bin = get_bin(alloc_size);

                //  Class bins (up to 4 KiB): every block in a bin is at least as large as any request for it, so a
                //      single lock-free pop is enough.  A larger class bin is tried if ours is empty.  The bin is never
                //      emptied temporarily, so a concurrent allocator never sees a false out-of-memory.
                for (size_t bin = start_bin; bin < NUM_BINS - 1; ++bin)
                {
                    if (free_block_node* node = stack_type::pop(free_stack_heads_[bin], adapter, backoff)) {
                        return node;
                    }
                }

                //  Bin 7 (> 4 KiB) holds blocks of different sizes: steal the list, take the best fit, and push the
                //      rest back.  While a list is stolen, a concurrent large allocation can see bin 7 empty; large
                //      blocks are expected to be rare in this resource.
                const size_t bin = NUM_BINS - 1;

                free_block_node* stolen_list = stack_type::steal_all(free_stack_heads_[bin]);

                free_block_node* best_fit = nullptr;
                free_block_node* prev = nullptr;
                free_block_node* best_fit_prev = nullptr;

                for (free_block_node* current = stolen_list; current != nullptr; current = current->next)
                {
                    const bool aligned = (reinterpret_cast<uintptr_t>(current) & (MIN_ALIGNMENT - 1)) == 0;

                    if (aligned && current->size >= alloc_size && (!best_fit || current->size < best_fit->size))
                    {
                        best_fit = current;
                        best_fit_prev = prev;
                    }
                    prev = current;
                }

                if (best_fit) {
                    if (best_fit_prev) {
                        best_fit_prev->next = best_fit->next;
                    } else {
                        stolen_list = best_fit->next;
                    }
                    best_fit->next = nullptr;
                }

                // Put remainder back into the same bin
                for (free_block_node* current = stolen_list; current != nullptr;)
                {
                    free_block_node* next = current->next;
                    stack_type::push(free_stack_heads_[bin], *current, adapter, backoff);
                    current = next;
                }

                return best_fit;
            }

            void do_deallocate(void* p, size_t bytes, size_t alignment) override
            {
                if (p == nullptr) {
                    return;
                }

                //  Same size as do_allocate() reserved, so the block goes back to the bin it came from.
                const size_t rounded_bytes = block_size_for(bytes);

                free_block_node* node = static_cast<free_block_node*>(p);
                node->size = rounded_bytes;
                node->next = nullptr;

                adapter_type adapter;
                backoff_type backoff;

                size_t bin = get_bin(rounded_bytes);
                stack_type::push(free_stack_heads_[bin], *node, adapter, backoff);
            }

            bool do_is_equal(memory_resource const& other) const noexcept override
            {
                return this == &other;
            }
        };
    }
}
