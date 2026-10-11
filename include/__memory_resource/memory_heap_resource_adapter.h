// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include "__memory_resource/memory_resource.h"
#include "memory_heap"

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        class memory_heap_resource_adapter : public memory_resource
        {
        public:
            explicit memory_heap_resource_adapter(minstd::memory_heap &heap)
                : heap_(heap)
            {
            }

        private:
            void *do_allocate(size_t bytes, size_t alignment) override
            {
                size_t effective_alignment = alignment == 0 ? 1 : alignment;
                size_t element_count = (bytes + effective_alignment - 1) / effective_alignment;

                uint8_t *block = heap_.allocate_block<uint8_t>(element_count, effective_alignment);

                //  The heap aligns blocks only to its own block alignment, so a larger requested alignment may not be
                //      met.  Refuse rather than return misaligned memory.

                if ((block != nullptr) && ((reinterpret_cast<uintptr_t>(block) % effective_alignment) != 0))
                {
                    heap_.deallocate_block(block, element_count);
                    return nullptr;
                }

                return block;
            }

            void do_deallocate(void *block, size_t bytes, size_t alignment) override
            {
                size_t effective_alignment = alignment == 0 ? 1 : alignment;
                size_t element_count = (bytes + effective_alignment - 1) / effective_alignment;

                heap_.deallocate_block(reinterpret_cast<uint8_t *>(block), element_count);
            }

            bool do_is_equal(memory_resource const &other) const noexcept override
            {
                return this == &other;
            }

            minstd::memory_heap &heap_;
        };
    }
}
