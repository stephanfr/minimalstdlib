// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.


#pragma once

#include "minstdconfig.h"

#include "memory_resource.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        //  A memory_resource over malloc() and free().  malloc() aligns to max_align_t; a larger alignment is met by
        //      over-allocating and keeping malloc's pointer in the word just before the aligned block.  As for any
        //      memory_resource, deallocate() must be given the alignment the block was allocated with.

        class malloc_free_wrapper_memory_resource : public memory_resource
        {
            public :
                malloc_free_wrapper_memory_resource() = default;

                malloc_free_wrapper_memory_resource(memory_resource *upstream_resource)
                {
                    (void)upstream_resource;
                }

                ~malloc_free_wrapper_memory_resource() = default;

private :

            static bool over_aligned(size_t alignment)
            {
                return alignment > alignof(max_align_t);
            }

            virtual void *do_allocate(size_t number_of_bytes, size_t alignment)
            {
                if (!over_aligned(alignment))
                {
                    return malloc(number_of_bytes);
                }

                if ((alignment & (alignment - 1)) != 0)
                {
                    return nullptr; //  not a power of two
                }

                if (number_of_bytes > SIZE_MAX - alignment - sizeof(void *))
                {
                    return nullptr;
                }

                char *raw = static_cast<char *>(malloc(number_of_bytes + alignment + sizeof(void *)));

                if (raw == nullptr)
                {
                    return nullptr;
                }

                const uintptr_t first = reinterpret_cast<uintptr_t>(raw) + sizeof(void *);
                char *aligned = reinterpret_cast<char *>((first + alignment - 1) & ~(uintptr_t)(alignment - 1));

                reinterpret_cast<void **>(aligned)[-1] = raw;

                return aligned;
            }

            virtual void do_deallocate(void * block, size_t number_of_bytes, size_t alignment)
            {
                (void)number_of_bytes;

                if ((block != nullptr) && over_aligned(alignment))
                {
                    free(reinterpret_cast<void **>(block)[-1]);
                    return;
                }

                free(block);
            }

            virtual bool do_is_equal(memory_resource const &other) const noexcept
            {
                return this == &other;
            }
        };
    }
}
