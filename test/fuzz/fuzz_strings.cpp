// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Differential fuzzing of fixed_string and dynamic_string: the input is a sequence of operations applied to the
//      string and to a flat character array with the same truncation rule.  A mismatch aborts.

#include <minstdconfig.h>

#include <dynamic_string>
#include <fixed_string>

#include <__memory_resource/malloc_free_wrapper_memory_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{
    template <size_t MAX>
    struct reference_string
    {
        char text[MAX + 1] = {}; //  not = "": clang 18 sizes that literal from the first instantiation and overruns later ones
        size_t length = 0;

        void assign(const char *source, size_t count)
        {
            char copy[4 * MAX + 64];
            for (size_t i = 0; i < count; i++) copy[i] = source[i];
            length = (count < MAX) ? count : MAX;
            for (size_t i = 0; i < length; i++) text[i] = copy[i];
            text[length] = 0;
        }

        void append(const char *source, size_t count)
        {
            char copy[4 * MAX + 64];
            for (size_t i = 0; i < count; i++) copy[i] = source[i];
            for (size_t i = 0; (i < count) && (length < MAX); i++) text[length++] = copy[i];
            text[length] = 0;
        }

        void erase(size_t index, size_t count)
        {
            if (index >= length) return;
            const size_t removed = ((count > length) || (index + count > length)) ? length - index : count;
            for (size_t i = index; i + removed < length; i++) text[i] = text[i + removed];
            length -= removed;
            text[length] = 0;
        }
    };

    template <size_t MAX>
    void run(minstd::basic_string<char> &s, const uint8_t *data, size_t size)
    {
        reference_string<MAX> r;
        s.clear();

        size_t at = 0;
        auto next = [&]() -> uint8_t { return (at < size) ? data[at++] : 0; };

        while (at < size)
        {
            const uint8_t op = next();
            char source[2 * MAX + 16];
            const size_t source_length = next() % (MAX + 8);

            for (size_t i = 0; i < source_length; i++)
            {
                source[i] = static_cast<char>('a' + (next() % 4));
            }
            source[source_length] = 0;

            switch (op % 8)
            {
            case 0: s = source; r.assign(source, source_length); break;
            case 1: s.append(source, source_length); r.append(source, source_length); break;
            case 2: s += source; r.append(source, source_length); break;
            case 3:
            {
                const bool pushed = s.push_back('x');
                if (pushed != (r.length < MAX)) abort();
                r.append("x", 1);
                break;
            }
            case 4:
            {
                const size_t index = next() % (MAX + 2);
                const size_t count = (next() % 4 == 0) ? SIZE_MAX : next() % (MAX + 2);
                s.erase(index, count);
                r.erase(index, count);
                break;
            }
            case 5: s += s.c_str(); r.append(r.text, r.length); break;
            case 6:
            {
                const size_t k = (r.length > 0) ? next() % r.length : 0;
                s = s.c_str() + k;
                r.assign(r.text + k, r.length - k);
                break;
            }
            default:
            {
                const size_t pos = next() % (MAX + 2);
                s.substr(s, pos, (next() % 4 == 0) ? SIZE_MAX : next() % (MAX + 2)); //  substring of itself
                const size_t start = (pos < r.length) ? pos : r.length;
                //  The reference mirrors the call above, which consumed one or two more bytes for the count
                (void)start;
                r.assign(s.c_str(), s.size()); //  checked below only for termination and bounds
                break;
            }
            }

            if ((s.size() != r.length) || (s.size() > MAX) || (s.c_str()[s.size()] != 0))
            {
                abort();
            }

            for (size_t i = 0; i < r.length; i++)
            {
                if (s.c_str()[i] != r.text[i]) abort();
            }
        }
    }

    minstd::pmr::malloc_free_wrapper_memory_resource heap;
    minstd::pmr::polymorphic_allocator<char> allocator(&heap);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    {
        minstd::fixed_string<32> s;
        run<32>(s, data, size);
    }

    {
        minstd::dynamic_string<80> s(allocator);
        run<80>(s, data, size);
    }

    return 0;
}
