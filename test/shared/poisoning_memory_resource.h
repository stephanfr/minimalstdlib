// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <minstdconfig.h>

#include <__memory_resource/memory_resource.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        namespace test
        {
            //  This memory resource is intended to help find use-after-free bugs in code that uses memory resources.
            //      It never reuses memory, it fills freed blocks with POISON (0xDD) and then quarantines them.
            //      Use-after-free writes are detected by freed_memory_intact().
            //
            //  ASAN can do this as well but this memory resource allows for deterministic detection without ASan.

            class poisoning_memory_resource : public memory_resource
            {
            public:
                static constexpr unsigned char POISON = 0xDD;
                static constexpr size_t MAX_BLOCKS = 256;

                poisoning_memory_resource() = default;

                poisoning_memory_resource(const poisoning_memory_resource &) = delete;
                poisoning_memory_resource &operator=(const poisoning_memory_resource &) = delete;

                ~poisoning_memory_resource() override
                {
                    for (size_t i = 0; i < num_blocks_; i++)
                    {
                        free(blocks_[i].ptr);
                    }
                }

                size_t bytes_in_use() const noexcept { return bytes_in_use_; }

                //  false => something wrote into a block after it was freed

                bool freed_memory_intact() const noexcept
                {
                    for (size_t i = 0; i < num_blocks_; i++)
                    {
                        if (!blocks_[i].freed)
                        {
                            continue;
                        }

                        const unsigned char *bytes = static_cast<const unsigned char *>(blocks_[i].ptr);

                        for (size_t j = 0; j < blocks_[i].bytes; j++)
                        {
                            if (bytes[j] != POISON)
                            {
                                return false;
                            }
                        }
                    }

                    return true;
                }

            private:
                struct block_record
                {
                    void *ptr;
                    size_t bytes;
                    bool freed;
                };

                block_record blocks_[MAX_BLOCKS] = {};
                size_t num_blocks_ = 0;
                size_t bytes_in_use_ = 0;

                void *do_allocate(size_t bytes, size_t alignment) override
                {
                    if ((num_blocks_ == MAX_BLOCKS) || (alignment > alignof(max_align_t)))
                    {
                        return nullptr;
                    }

                    void *ptr = malloc(bytes);

                    blocks_[num_blocks_++] = {ptr, bytes, false};
                    bytes_in_use_ += bytes;

                    return ptr;
                }

                void do_deallocate(void *ptr, size_t bytes, size_t) override
                {
                    for (size_t i = 0; i < num_blocks_; i++)
                    {
                        if ((blocks_[i].ptr == ptr) && !blocks_[i].freed)
                        {
                            memset(ptr, POISON, blocks_[i].bytes);
                            blocks_[i].freed = true;
                            bytes_in_use_ -= bytes;
                        }
                    }
                }

                bool do_is_equal(const memory_resource &other) const noexcept override
                {
                    return this == &other;
                }
            };
        }
    }
}
