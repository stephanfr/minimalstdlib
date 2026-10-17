// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include "atomic"
#include "new"

#include "lockfree/arena_tagged_ptr"
#include "lockfree/intrusive_tagged_stack"
#include "lockfree/tagged_ptr"
#include "memory_resource.h"

#include <stdint.h>

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        //
        //  A lock-free allocator over a caller-supplied arena.  Requests are rounded to 64-byte size classes; freed
        //      blocks go to per-class lock-free free bins and are reused, and new memory comes from a bump pointer.
        //
        //  The free bins' links are kept in a side table, one 4-byte cell per 64-byte block, never inside the
        //      blocks: a popper stalled between loading a bin head and its CAS may still read the link of a block
        //      another thread has already handed to its caller, and that read must not touch memory the caller is
        //      writing.  The table takes 1/17 of the arena (after aligning its start to 64 bytes), so the usable
        //      capacity is about 94% of the buffer.
        //

        class lockfree_static_arena_resource : public memory_resource
        {
        private:
            //  The only field kept in a free block is its size, written by the thread freeing the block and read only
            //      by a thread that owns the block through a bin-7 steal_all().
            struct free_block_header
            {
                size_t size;
            };

            //  A free-bin link: the index (+1, 0 for none) of the next block's cell
            struct link_cell
            {
                uint32_t next;
            };

            static constexpr size_t NUM_BINS = 8;
            static constexpr size_t MIN_ALIGNMENT = 64;
            static constexpr size_t LARGEST_CLASS = 4096; //  bins 0-6 hold exactly 64, 128, ... 4096 bytes; bin 7 holds larger blocks

            //  Free-bin heads hold an arena-relative cell index rather than a pointer: there is one cell per block,
            //      so the index needs only log2(arena size / 64) bits and the rest of the word is ABA counter (39 bits
            //      for a 1 GiB arena, against tagged_ptr's 16).  A popper stalled between its head load and its CAS is
            //      fooled only if its bin changes 2^counter_bits times meanwhile.
            using tagged_ptr_type = lockfree::tagged_ptr<link_cell>;
            using head_codec_type = lockfree::arena_tagged_ptr<link_cell, sizeof(link_cell)>;
            using stack_type = lockfree::intrusive_tagged_stack<link_cell, tagged_ptr_type, uint32_t, &link_cell::next>;

            static uint8_t *aligned_start(void *start)
            {
                const uintptr_t address = reinterpret_cast<uintptr_t>(start);
                return reinterpret_cast<uint8_t *>((address + MIN_ALIGNMENT - 1) & ~(uintptr_t)(MIN_ALIGNMENT - 1));
            }

            //  Blocks that fit with their cells after the start is aligned
            static size_t block_count(void *start, size_t size)
            {
                const size_t padding = static_cast<size_t>(aligned_start(start) - static_cast<uint8_t *>(start));
                const size_t usable = (size > padding) ? size - padding : 0;
                const size_t blocks = usable / (MIN_ALIGNMENT + sizeof(link_cell));

                return (blocks < UINT32_MAX) ? blocks : UINT32_MAX - 1; //  cell links are 32-bit indices
            }

            uint8_t* arena_start_;
            uint8_t* arena_end_;
            link_cell *cells_;
            atomic<uint8_t*> current_ptr_;
            const head_codec_type head_codec_;

            alignas(64) atomic<typename tagged_ptr_type::storage_type> free_stack_heads_[NUM_BINS];

            link_cell *cell_of(const void *block) const
            {
                return cells_ + ((static_cast<const uint8_t *>(block) - arena_start_) / MIN_ALIGNMENT);
            }

            uint8_t *block_of(const link_cell *cell) const
            {
                return arena_start_ + (static_cast<size_t>(cell - cells_) * MIN_ALIGNMENT);
            }

            static free_block_header *header_of(uint8_t *block)
            {
                return reinterpret_cast<free_block_header *>(block);
            }

            //  The size actually reserved for a request.  Up to 4 KiB every request is rounded up to a whole size
            //      class, so any block in a class bin fits any request for that bin.  Larger requests are rounded
            //      to MIN_ALIGNMENT and kept in bin 7, which is searched best-fit.  Both allocate and deallocate
            //      use this, so a block always returns to the bin it came from.
            static size_t block_size_for(size_t bytes)
            {
                size_t size = (bytes < sizeof(free_block_header)) ? sizeof(free_block_header) : bytes;
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

            //  Converts between cells and the 32-bit links stored in them
            struct adapter_type {
                link_cell *cells;

                uint32_t to_link(link_cell *cell) const { return (cell != nullptr) ? static_cast<uint32_t>(cell - cells) + 1 : 0; }
                link_cell *to_node(uint32_t link) const { return (link != 0) ? cells + (link - 1) : nullptr; }
            };

            // backoff adapter for intrusive stack
            struct backoff_type {
                void operator()(size_t& retries) const {
                    retries++;
                }
            };

            adapter_type adapter() const
            {
                return adapter_type{cells_};
            }

            void push_free(uint8_t *block, size_t block_size)
            {
                header_of(block)->size = block_size;

                backoff_type backoff;
                stack_type::push(free_stack_heads_[get_bin(block_size)], *cell_of(block), adapter(), backoff, head_codec_);
            }

            //  Return [block, block + bytes) to the free bins as pieces that are each a whole size class (or, above
            //      LARGEST_CLASS, one bin-7 block).  bytes is a multiple of MIN_ALIGNMENT.  Used for the remainder when a
            //      larger free block is split, so a block is always handed out at exactly block_size_for(request):
            //      deallocate() only knows the request size, and a larger block would otherwise shrink for good.
            void free_pieces(uint8_t* block, size_t bytes)
            {
                if (bytes > LARGEST_CLASS)
                {
                    push_free(block, bytes);
                    return;
                }

                for (size_t class_size = LARGEST_CLASS; bytes > 0; class_size >>= 1)
                {
                    if (bytes >= class_size)
                    {
                        push_free(block, class_size);
                        block += class_size;
                        bytes -= class_size;
                    }
                }
            }

        public:
            lockfree_static_arena_resource(void* start, size_t size)
                : arena_start_(aligned_start(start)),
                  arena_end_(arena_start_ + block_count(start, size) * MIN_ALIGNMENT),
                  cells_(reinterpret_cast<link_cell *>(arena_end_)),
                  current_ptr_(arena_start_),
                  head_codec_(cells_, block_count(start, size) * sizeof(link_cell))
            {
                for (size_t i = 0; i < NUM_BINS; ++i) {
                    free_stack_heads_[i].store(head_codec_.make(nullptr), memory_order_relaxed);
                }
            }

            ~lockfree_static_arena_resource() override = default;

            //  Bytes of the buffer available to allocations (the rest holds the free-bin links)
            size_t capacity() const noexcept
            {
                return static_cast<size_t>(arena_end_ - arena_start_);
            }

        protected:
            void* do_allocate(size_t bytes, size_t alignment) override
            {
                if (alignment > MIN_ALIGNMENT) {
                    return nullptr;
                }

                //  Larger than the arena (this also keeps block_size_for()'s rounding from wrapping)
                if (bytes > static_cast<size_t>(arena_end_ - arena_start_)) {
                    return nullptr;
                }

                const size_t alloc_size = block_size_for(bytes);

                // Bump pointer allocation
                uint8_t* expected = current_ptr_.load(memory_order_relaxed);
                while (true)
                {
                    uintptr_t ptr_as_int = reinterpret_cast<uintptr_t>(expected);
                    uintptr_t aligned_int = (ptr_as_int + MIN_ALIGNMENT - 1) & ~(MIN_ALIGNMENT - 1);
                    const uintptr_t end_int = reinterpret_cast<uintptr_t>(arena_end_);

                    //  Compare remaining space, not end pointers, so a large request cannot wrap the address
                    if ((aligned_int > end_int) || (alloc_size > end_int - aligned_int)) {
                        break;
                    }

                    uint8_t* aligned_expected = reinterpret_cast<uint8_t*>(aligned_int);
                    uint8_t* new_ptr = aligned_expected + alloc_size;

                    if (current_ptr_.compare_exchange_weak(expected, new_ptr, memory_order_acquire, memory_order_relaxed))
                    {
                        return aligned_expected;
                    }
                }

                // Fallback: Check free stacks
                const adapter_type links = adapter();
                backoff_type backoff;

                const size_t start_bin = get_bin(alloc_size);

                //  Class bins (up to 4 KiB): every block in a bin is at least as large as any request for it, so a
                //      single lock-free pop is enough.  A larger class bin is tried if ours is empty.  The bin is never
                //      emptied temporarily, so a concurrent allocator never sees a false out-of-memory.
                for (size_t bin = start_bin; bin < NUM_BINS - 1; ++bin)
                {
                    if (link_cell* cell = stack_type::pop(free_stack_heads_[bin], links, backoff, head_codec_)) {
                        uint8_t *block = block_of(cell);

                        //  A block from a larger class: hand out the requested class and free the rest as smaller
                        //      classes, so the whole block is returned whatever size deallocate() is given.
                        const size_t node_size = MIN_ALIGNMENT << bin;
                        if (node_size > alloc_size) {
                            free_pieces(block + alloc_size, node_size - alloc_size);
                        }
                        return block;
                    }
                }

                //  Bin 7 (> 4 KiB) holds blocks of different sizes: steal the list, take the best fit, and push the
                //      rest back.  While a list is stolen, a concurrent large allocation can see bin 7 empty; large
                //      blocks are expected to be rare in this resource.  The stolen blocks belong to this thread, so
                //      their size headers can be read.
                const size_t bin = NUM_BINS - 1;

                link_cell* stolen_list = stack_type::steal_all(free_stack_heads_[bin], head_codec_);

                link_cell* best_fit = nullptr;
                link_cell* prev = nullptr;
                link_cell* best_fit_prev = nullptr;
                size_t best_fit_size = 0;

                for (link_cell* current = stolen_list; current != nullptr; current = links.to_node(stack_type::load_link(*current)))
                {
                    const size_t size = header_of(block_of(current))->size;

                    if ((size >= alloc_size) && ((best_fit == nullptr) || (size < best_fit_size)))
                    {
                        best_fit = current;
                        best_fit_prev = prev;
                        best_fit_size = size;
                    }
                    prev = current;
                }

                if (best_fit) {
                    const uint32_t after_best = stack_type::load_link(*best_fit);

                    if (best_fit_prev) {
                        stack_type::store_link(*best_fit_prev, after_best);
                    } else {
                        stolen_list = links.to_node(after_best);
                    }
                    stack_type::store_link(*best_fit, 0);
                }

                // Put remainder back into the same bin
                for (link_cell* current = stolen_list; current != nullptr;)
                {
                    link_cell* next = links.to_node(stack_type::load_link(*current));
                    stack_type::push(free_stack_heads_[bin], *current, links, backoff, head_codec_);
                    current = next;
                }

                if (best_fit == nullptr) {
                    return nullptr;
                }

                uint8_t *block = block_of(best_fit);

                //  Hand out exactly alloc_size and free the tail (see free_pieces())
                if (best_fit_size > alloc_size) {
                    free_pieces(block + alloc_size, best_fit_size - alloc_size);
                }

                return block;
            }

            void do_deallocate(void* p, size_t bytes, size_t alignment) override
            {
                if (p == nullptr) {
                    return;
                }

                //  A block from elsewhere would index outside the link table
                MINIMAL_STD_ASSERT((static_cast<uint8_t *>(p) >= arena_start_) && (static_cast<uint8_t *>(p) < arena_end_) &&
                                   (((static_cast<uint8_t *>(p) - arena_start_) & (MIN_ALIGNMENT - 1)) == 0));

                //  Same size as do_allocate() reserved, so the block goes back to the bin it came from.
                push_free(static_cast<uint8_t *>(p), block_size_for(bytes));
            }

            bool do_is_equal(memory_resource const& other) const noexcept override
            {
                return this == &other;
            }
        };
    }
}
