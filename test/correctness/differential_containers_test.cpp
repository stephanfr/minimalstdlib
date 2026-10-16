// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//
//  Differential tests: seeded random operation sequences run in lockstep on a minstd container and on a reference
//      model simple enough to be obviously right (flat arrays), comparing the observable state after every step.
//      The std containers cannot be used as the reference here: minstd's headers take their names (<vector>,
//      <list>, <map>, ...) on the test include path.
//

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <avl_tree>
#include <dynamic_string>
#include <fixed_string>
#include <forward_list>
#include <list>
#include <lru_cache>
#include <random>
#include <vector>

#include <__memory_resource/malloc_free_wrapper_memory_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include <stdint.h>
#include <stdio.h>

namespace
{
    minstd::pmr::malloc_free_wrapper_memory_resource heap;

    int memcmp_like(const char *a, const char *b, size_t n);

    //  Reports the first difference with the step and operation that produced it

    struct divergence
    {
        bool found = false;
        char text[256] = "";

        void at(size_t step, const char *operation, const char *what)
        {
            if (!found)
            {
                found = true;
                snprintf(text, sizeof(text), "step %zu, after %s: %s", step, operation, what);
            }
        }
    };

    //
    //  avl_tree against a flat present/value array over a small key range
    //

    TEST_GROUP(DifferentialContainerTests){};

    TEST(DifferentialContainerTests, AvlTreeMatchesAFlatMap)
    {
        using tree_type = minstd::avl_tree<uint32_t, uint32_t>;
        minstd::pmr::polymorphic_allocator<tree_type::node_type> allocator(&heap);

        constexpr uint32_t KEYS = 96;

        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            tree_type tree(allocator);
            minstd::xoroshiro128_plus_plus engine(seed);

            bool present[KEYS] = {};
            uint32_t value[KEYS] = {};
            const uint32_t *address[KEYS] = {}; //  erase must not move other elements
            size_t count = 0;

            divergence d;

            for (size_t step = 0; (step < 6000) && !d.found; step++)
            {
                const uint32_t key = static_cast<uint32_t>(engine() % KEYS);
                const char *operation = "";

                switch (engine() % 5)
                {
                case 0:
                case 1:
                {
                    operation = "insert";
                    const uint32_t v = static_cast<uint32_t>(engine());
                    auto result = tree.insert(key, v);

                    if (result.second == present[key])
                    {
                        d.at(step, operation, "inserted flag wrong");
                    }

                    if (!present[key])
                    {
                        present[key] = true;
                        value[key] = v;
                        address[key] = &result.first->second;
                        count++;
                    }

                    if ((result.first == tree.end()) || (result.first->first != key) || (result.first->second != value[key]))
                    {
                        d.at(step, operation, "returned iterator does not reference the key's element");
                    }

                    break;
                }

                case 2:
                {
                    operation = "erase(key)";
                    const size_t erased = tree.erase(key);

                    if (erased != (present[key] ? 1u : 0u))
                    {
                        d.at(step, operation, "erase count wrong");
                    }

                    if (present[key])
                    {
                        present[key] = false;
                        count--;
                    }

                    break;
                }

                case 3:
                {
                    operation = "erase(iterator)";
                    auto it = tree.find(key);

                    if ((it == tree.end()) == present[key])
                    {
                        d.at(step, operation, "find disagrees before erase");
                        break;
                    }

                    if (it == tree.end())
                    {
                        break;
                    }

                    auto next = tree.erase(it);
                    present[key] = false;
                    count--;

                    uint32_t expected_next = key + 1;
                    while ((expected_next < KEYS) && !present[expected_next])
                    {
                        expected_next++;
                    }

                    if ((expected_next == KEYS) != (next == tree.end()))
                    {
                        d.at(step, operation, "returned iterator is not the next element");
                    }
                    else if ((next != tree.end()) && (next->first != expected_next))
                    {
                        d.at(step, operation, "returned iterator has the wrong key");
                    }

                    break;
                }

                default:
                {
                    operation = "find";
                    auto it = tree.find(key);

                    if ((it != tree.end()) != present[key])
                    {
                        d.at(step, operation, "presence differs");
                    }
                    else if ((it != tree.end()) && (it->second != value[key]))
                    {
                        d.at(step, operation, "value differs");
                    }

                    break;
                }
                }

                //  Whole-state comparison: size, forward and backward iteration, element addresses

                if (tree.size() != count)
                {
                    d.at(step, operation, "size differs");
                }

                uint32_t expected = 0;
                size_t visited = 0;

                for (auto it = tree.begin(); it != tree.end(); ++it, ++expected, ++visited)
                {
                    while ((expected < KEYS) && !present[expected])
                    {
                        expected++;
                    }

                    if ((expected == KEYS) || (it->first != expected) || (it->second != value[expected]))
                    {
                        d.at(step, operation, "forward iteration differs");
                        break;
                    }

                    if (&it->second != address[expected])
                    {
                        d.at(step, operation, "an element moved in memory");
                        break;
                    }
                }

                if (visited != count)
                {
                    d.at(step, operation, "forward iteration visits the wrong number of elements");
                }

                if (count > 0)
                {
                    auto it = tree.end();
                    int expected_back = KEYS - 1;
                    size_t back_visited = 0;

                    do
                    {
                        --it;
                        while ((expected_back >= 0) && !present[expected_back])
                        {
                            expected_back--;
                        }

                        if ((expected_back < 0) || (it->first != static_cast<uint32_t>(expected_back)))
                        {
                            d.at(step, operation, "backward iteration differs");
                            break;
                        }

                        expected_back--;
                        back_visited++;
                    } while (it != tree.begin());

                    if (back_visited != count)
                    {
                        d.at(step, operation, "backward iteration visits the wrong number of elements");
                    }
                }
            }

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }

    //
    //  list and forward_list against a flat array
    //

    template <typename list_type>
    bool same_sequence(list_type &list, const int *reference, size_t count)
    {
        size_t i = 0;

        for (auto it = list.begin(); it != list.end(); ++it, ++i)
        {
            if ((i >= count) || (*it != reference[i]))
            {
                return false;
            }
        }

        return i == count;
    }

    template <typename iterator_type>
    iterator_type advance(iterator_type it, size_t steps)
    {
        for (size_t i = 0; i < steps; i++)
        {
            ++it;
        }

        return it;
    }

    TEST(DifferentialContainerTests, ListMatchesAnArray)
    {
        using list_type = minstd::list<int>;
        minstd::pmr::polymorphic_allocator<list_type::node_type> allocator(&heap);

        constexpr size_t CAPACITY = 256;

        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            list_type list(allocator);
            minstd::xoroshiro128_plus_plus engine(seed);

            int reference[CAPACITY];
            size_t count = 0;

            divergence d;

            auto insert_at = [&](size_t index, int v)
            {
                for (size_t i = count; i > index; i--)
                {
                    reference[i] = reference[i - 1];
                }

                reference[index] = v;
                count++;
            };

            auto erase_at = [&](size_t index)
            {
                for (size_t i = index; i + 1 < count; i++)
                {
                    reference[i] = reference[i + 1];
                }

                count--;
            };

            for (size_t step = 0; (step < 8000) && !d.found; step++)
            {
                const int v = static_cast<int>(engine() % 1000);
                const char *operation = "";

                switch (engine() % 8)
                {
                case 0:
                    operation = "push_front";
                    if (count < CAPACITY)
                    {
                        list.push_front(v);
                        insert_at(0, v);
                    }
                    break;

                case 1:
                    operation = "push_back";
                    if (count < CAPACITY)
                    {
                        list.push_back(v);
                        insert_at(count, v);
                    }
                    break;

                case 2:
                    operation = "pop_front";
                    if (count > 0)
                    {
                        list.pop_front();
                        erase_at(0);
                    }
                    break;

                case 3:
                    operation = "pop_back";
                    if (count > 0)
                    {
                        list.pop_back();
                        erase_at(count - 1);
                    }
                    break;

                case 4:
                    operation = "insert_after";
                    if ((count > 0) && (count < CAPACITY))
                    {
                        const size_t index = engine() % count;
                        list.insert_after(advance(list.begin(), index), v);
                        insert_at(index + 1, v);
                    }
                    break;

                case 5:
                    operation = "erase";
                    if (count > 0)
                    {
                        const size_t index = engine() % count;
                        list.erase(advance(list.begin(), index));
                        erase_at(index);
                    }
                    break;

                case 6:
                    operation = "erase_after";
                    if (count > 1)
                    {
                        const size_t index = engine() % (count - 1);
                        list.erase_after(advance(list.begin(), index));
                        erase_at(index + 1);
                    }
                    break;

                default:
                    operation = "clear";
                    if ((engine() % 50) == 0)
                    {
                        list.clear();
                        count = 0;
                    }
                    break;
                }

                if ((list.size() != count) || (list.empty() != (count == 0)))
                {
                    d.at(step, operation, "size differs");
                }

                if (!same_sequence(list, reference, count))
                {
                    d.at(step, operation, "sequence differs");
                }

                if ((count > 0) && ((list.front() != reference[0]) || (list.back() != reference[count - 1])))
                {
                    d.at(step, operation, "front or back differs");
                }

                //  Backward iteration

                if (count > 0)
                {
                    auto it = list.end();
                    size_t i = count;

                    do
                    {
                        --it;
                        --i;

                        if (*it != reference[i])
                        {
                            d.at(step, operation, "backward iteration differs");
                            break;
                        }
                    } while (it != list.begin());

                    if (i != 0)
                    {
                        d.at(step, operation, "backward iteration visits the wrong number of elements");
                    }
                }
            }

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }

    TEST(DifferentialContainerTests, ForwardListMatchesAnArray)
    {
        using list_type = minstd::forward_list<int>;
        minstd::pmr::polymorphic_allocator<list_type::node_type> allocator(&heap);

        constexpr size_t CAPACITY = 256;

        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            list_type list(allocator);
            minstd::xoroshiro128_plus_plus engine(seed);

            int reference[CAPACITY];
            size_t count = 0;

            divergence d;

            for (size_t step = 0; (step < 8000) && !d.found; step++)
            {
                const int v = static_cast<int>(engine() % 1000);
                const char *operation = "";

                switch (engine() % 4)
                {
                case 0:
                    operation = "push_front";
                    if (count < CAPACITY)
                    {
                        list.push_front(v);
                        for (size_t i = count; i > 0; i--)
                        {
                            reference[i] = reference[i - 1];
                        }
                        reference[0] = v;
                        count++;
                    }
                    break;

                case 1:
                    operation = "pop_front";
                    if (count > 0)
                    {
                        list.pop_front();
                        for (size_t i = 0; i + 1 < count; i++)
                        {
                            reference[i] = reference[i + 1];
                        }
                        count--;
                    }
                    break;

                case 2:
                    operation = "insert_after";
                    if ((count > 0) && (count < CAPACITY))
                    {
                        const size_t index = engine() % count;
                        list.insert_after(advance(list.begin(), index), v);
                        for (size_t i = count; i > index + 1; i--)
                        {
                            reference[i] = reference[i - 1];
                        }
                        reference[index + 1] = v;
                        count++;
                    }
                    break;

                default:
                    operation = "erase_after";
                    if (count > 1)
                    {
                        const size_t index = engine() % (count - 1);
                        list.erase_after(advance(list.begin(), index));
                        for (size_t i = index + 1; i + 1 < count; i++)
                        {
                            reference[i] = reference[i + 1];
                        }
                        count--;
                    }
                    break;
                }

                if (list.empty() != (count == 0))
                {
                    d.at(step, operation, "empty() differs");
                }

                if (!same_sequence(list, reference, count))
                {
                    d.at(step, operation, "sequence differs");
                }

                if ((count > 0) && (list.front() != reference[0]))
                {
                    d.at(step, operation, "front differs");
                }
            }

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }

    //
    //  vector within its fixed capacity
    //

    TEST(DifferentialContainerTests, VectorMatchesAnArray)
    {
        constexpr size_t CAPACITY = 48;

        using vector_type = minstd::vector<int, CAPACITY, CAPACITY>;
        minstd::pmr::polymorphic_allocator<int> allocator(&heap);

        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            vector_type vec(allocator);
            minstd::xoroshiro128_plus_plus engine(seed);

            int reference[CAPACITY];
            size_t count = 0;

            divergence d;

            for (size_t step = 0; (step < 8000) && !d.found; step++)
            {
                const int v = static_cast<int>(engine() % 1000);
                const char *operation = "";

                switch (engine() % 6)
                {
                case 0:
                case 1:
                    operation = "push_back";
                    if (count < CAPACITY)
                    {
                        vec.push_back(v);
                        reference[count++] = v;
                    }
                    break;

                case 2:
                    operation = "pop_back";
                    if (count > 0)
                    {
                        vec.pop_back();
                        count--;
                    }
                    break;

                case 3:
                case 4:
                {
                    operation = (step % 2) ? "insert" : "emplace";
                    if (count < CAPACITY)
                    {
                        const size_t index = engine() % (count + 1);
                        auto it = (step % 2) ? vec.insert(advance(vec.begin(), index), v) : vec.emplace(advance(vec.begin(), index), v);

                        for (size_t i = count; i > index; i--)
                        {
                            reference[i] = reference[i - 1];
                        }

                        reference[index] = v;
                        count++;

                        if (*it != v)
                        {
                            d.at(step, operation, "returned iterator does not reference the new element");
                        }
                    }
                    break;
                }

                default:
                    operation = "assign element";
                    if (count > 0)
                    {
                        const size_t index = engine() % count;
                        vec[index] = v;
                        reference[index] = v;
                    }
                    break;
                }

                if (vec.size() != count)
                {
                    d.at(step, operation, "size differs");
                }

                for (size_t i = 0; i < count; i++)
                {
                    if (vec[i] != reference[i])
                    {
                        d.at(step, operation, "element differs");
                        break;
                    }
                }

                size_t i = 0;
                for (auto it = vec.begin(); it != vec.end(); ++it, ++i)
                {
                    if ((i >= count) || (*it != reference[i]))
                    {
                        d.at(step, operation, "iteration differs");
                        break;
                    }
                }
            }

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }

    //
    //  lru_cache against a recency-ordered array
    //

    TEST(DifferentialContainerTests, LRUCacheMatchesARecencyList)
    {
        using cache_type = minstd::lru_cache<uint32_t, uint32_t>;
        minstd::pmr::polymorphic_allocator<cache_type::list_entry_type> list_allocator(&heap);
        minstd::pmr::polymorphic_allocator<cache_type::map_entry_type> map_allocator(&heap);

        constexpr size_t CAPACITY = 8;
        constexpr uint32_t KEYS = 24;

        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            cache_type cache(CAPACITY, list_allocator, map_allocator);
            minstd::xoroshiro128_plus_plus engine(seed);

            uint32_t keys[CAPACITY]; //  most recent first
            uint32_t values[CAPACITY];
            size_t count = 0;

            divergence d;

            auto index_of = [&](uint32_t key) -> int
            {
                for (size_t i = 0; i < count; i++)
                {
                    if (keys[i] == key)
                    {
                        return static_cast<int>(i);
                    }
                }

                return -1;
            };

            auto move_to_front = [&](size_t index)
            {
                const uint32_t k = keys[index];
                const uint32_t v = values[index];

                for (size_t i = index; i > 0; i--)
                {
                    keys[i] = keys[i - 1];
                    values[i] = values[i - 1];
                }

                keys[0] = k;
                values[0] = v;
            };

            for (size_t step = 0; (step < 8000) && !d.found; step++)
            {
                const uint32_t key = static_cast<uint32_t>(engine() % KEYS);
                const char *operation = "";
                const int index = index_of(key);

                switch (engine() % 3)
                {
                case 0:
                {
                    operation = "add";
                    const uint32_t v = static_cast<uint32_t>(engine());
                    const bool added = cache.add(key, v);

                    if (added != (index < 0))
                    {
                        d.at(step, operation, "add result differs");
                    }

                    if (index < 0)
                    {
                        if (count == CAPACITY)
                        {
                            count--; //  evict the least recently used
                        }

                        for (size_t i = count; i > 0; i--)
                        {
                            keys[i] = keys[i - 1];
                            values[i] = values[i - 1];
                        }

                        keys[0] = key;
                        values[0] = v;
                        count++;
                    }

                    break;
                }

                case 1:
                {
                    operation = "find";
                    auto found = cache.find(key);

                    if (found.has_value() != (index >= 0))
                    {
                        d.at(step, operation, "presence differs");
                    }
                    else if (index >= 0)
                    {
                        if (found.value().get() != values[index])
                        {
                            d.at(step, operation, "value differs");
                        }

                        move_to_front(static_cast<size_t>(index));
                    }

                    break;
                }

                default:
                {
                    operation = "remove";
                    const bool removed = cache.remove(key);

                    if (removed != (index >= 0))
                    {
                        d.at(step, operation, "remove result differs");
                    }

                    if (index >= 0)
                    {
                        for (size_t i = static_cast<size_t>(index); i + 1 < count; i++)
                        {
                            keys[i] = keys[i + 1];
                            values[i] = values[i + 1];
                        }

                        count--;
                    }

                    break;
                }
                }

                if (cache.size() != count)
                {
                    d.at(step, operation, "size differs");
                }

                //  Recency order: iteration runs most recent first

                size_t i = 0;
                for (auto it = cache.begin(); it != cache.end(); ++it, ++i)
                {
                    if ((i >= count) || (it->key() != keys[i]) || (it->value() != values[i]))
                    {
                        d.at(step, operation, "recency order differs");
                        break;
                    }
                }
            }

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }

    //
    //  fixed_string and dynamic_string against a flat character array with the same truncation rule
    //

    template <size_t MAX>
    struct reference_string
    {
        char text[MAX + 1] = "";
        size_t length = 0;

        void assign(const char *source, size_t count)
        {
            char copy[2 * MAX + 64];
            for (size_t i = 0; i < count; i++)
            {
                copy[i] = source[i]; //  the source may alias text
            }

            length = (count < MAX) ? count : MAX;
            for (size_t i = 0; i < length; i++)
            {
                text[i] = copy[i];
            }
            text[length] = 0;
        }

        void append(const char *source, size_t count)
        {
            char copy[2 * MAX + 64];
            for (size_t i = 0; i < count; i++)
            {
                copy[i] = source[i];
            }

            for (size_t i = 0; (i < count) && (length < MAX); i++)
            {
                text[length++] = copy[i];
            }
            text[length] = 0;
        }

        void erase(size_t index, size_t count)
        {
            if (index >= length)
            {
                return;
            }

            const size_t removed = ((count > length) || (index + count > length)) ? length - index : count;

            for (size_t i = index; i + removed < length; i++)
            {
                text[i] = text[i + removed];
            }

            length -= removed;
            text[length] = 0;
        }

        size_t find(const char *needle, size_t needle_length, size_t pos) const
        {
            if (pos > length)
            {
                return SIZE_MAX;
            }

            for (size_t i = pos; i + needle_length <= length; i++)
            {
                size_t j = 0;
                while ((j < needle_length) && (text[i + j] == needle[j]))
                {
                    j++;
                }

                if (j == needle_length)
                {
                    return i;
                }
            }

            return SIZE_MAX;
        }

        size_t find(char c, size_t pos) const
        {
            for (size_t i = pos; i < length; i++)
            {
                if (text[i] == c)
                {
                    return i;
                }
            }

            return SIZE_MAX;
        }

        size_t find_last_of(char c, size_t pos) const
        {
            if (length == 0)
            {
                return SIZE_MAX;
            }

            for (size_t i = ((pos >= length) ? length - 1 : pos) + 1; i > 0; i--)
            {
                if (text[i - 1] == c)
                {
                    return i - 1;
                }
            }

            return SIZE_MAX;
        }
    };

    template <typename string_type, size_t MAX>
    void string_differential(string_type &s, string_type &other, uint64_t seed, divergence &d)
    {
        reference_string<MAX> r;
        minstd::xoroshiro128_plus_plus engine(seed);

        auto random_text = [&](char *out, size_t length)
        {
            for (size_t i = 0; i < length; i++)
            {
                out[i] = static_cast<char>('a' + (engine() % 4)); //  a small alphabet, so finds hit
            }
            out[length] = 0;
        };

        s.clear();

        for (size_t step = 0; (step < 6000) && !d.found; step++)
        {
            char source[2 * MAX + 16];
            const size_t source_length = engine() % (MAX + 8);
            random_text(source, source_length);

            const char *operation = "";
            size_t got = 0;
            size_t want = 0;
            bool compare_result = false;

            switch (engine() % 12)
            {
            case 0:
                operation = "assign";
                s = source;
                r.assign(source, source_length);
                break;

            case 1:
                operation = "append";
                s.append(source, source_length);
                r.append(source, source_length);
                break;

            case 2:
                operation = "+= text";
                s += source;
                r.append(source, source_length);
                break;

            case 3:
                operation = "push_back";
            {
                const bool pushed = s.push_back(source[0] ? source[0] : 'z');
                if (pushed != (r.length < MAX))
                {
                    d.at(step, operation, "push_back result differs");
                }
                const char c = source[0] ? source[0] : 'z';
                r.append(&c, 1);
                break;
            }

            case 4:
                operation = "pop_back";
                s.pop_back();
                if (r.length > 0)
                {
                    r.text[--r.length] = 0;
                }
                break;

            case 5:
            {
                operation = "erase";
                const size_t index = engine() % (MAX + 2);
                const size_t count = ((engine() % 4) == 0) ? SIZE_MAX : engine() % (MAX + 2);
                s.erase(index, count);
                r.erase(index, count);
                break;
            }

            case 6:
                operation = "self-append";
                s += s.c_str();
                r.append(r.text, r.length);
                break;

            case 7:
            {
                operation = "assign own suffix";
                const size_t k = (r.length > 0) ? engine() % r.length : 0;
                s = s.c_str() + k;
                r.assign(r.text + k, r.length - k);
                break;
            }

            case 8:
            {
                operation = "find(text)";
                const size_t needle_length = engine() % 4;
                const size_t pos = engine() % (MAX + 2);
                got = s.find(source + (source_length > needle_length ? source_length - needle_length : 0), pos);
                const char *needle = source + (source_length > needle_length ? source_length - needle_length : 0);
                want = r.find(needle, (source_length > needle_length) ? needle_length : source_length, pos);
                compare_result = true;
                break;
            }

            case 9:
            {
                operation = "find(char)";
                const char c = static_cast<char>('a' + (engine() % 5));
                const size_t pos = engine() % (MAX + 2);
                got = s.find(c, pos);
                want = r.find(c, pos);
                compare_result = true;
                break;
            }

            case 10:
            {
                operation = "find_last_of";
                const char c = static_cast<char>('a' + (engine() % 5));
                const size_t pos = ((engine() % 4) == 0) ? SIZE_MAX : engine() % (MAX + 2);
                got = s.find_last_of(c, pos);
                want = r.find_last_of(c, pos);
                compare_result = true;
                break;
            }

            default:
            {
                operation = "substr";
                const size_t pos = engine() % (MAX + 2);
                const size_t count = ((engine() % 4) == 0) ? SIZE_MAX : engine() % (MAX + 2);
                s.substr(other, pos, count);

                const size_t start = (pos < r.length) ? pos : r.length;
                const size_t take = ((count < r.length - start) ? count : r.length - start);

                if ((other.size() != take) || (memcmp_like(other.c_str(), r.text + start, take) != 0) || (other.c_str()[take] != 0))
                {
                    d.at(step, operation, "substring differs");
                }

                break;
            }
            }

            if (compare_result)
            {
                const size_t normalised = (got == string_type::npos) ? SIZE_MAX : got;

                if (normalised != want)
                {
                    char text[96];
                    snprintf(text, sizeof(text), "returned %zu, reference %zu", got, want);
                    d.at(step, operation, text);
                }
            }

            if ((s.size() != r.length) || (memcmp_like(s.c_str(), r.text, r.length + 1) != 0))
            {
                char text[200];
                snprintf(text, sizeof(text), "contents differ: \"%s\" (%zu) vs \"%s\" (%zu)", s.c_str(), s.size(), r.text, r.length);
                d.at(step, operation, text);
            }
        }
    }

    int memcmp_like(const char *a, const char *b, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (a[i] != b[i])
            {
                return (a[i] < b[i]) ? -1 : 1;
            }
        }

        return 0;
    }

    TEST(DifferentialContainerTests, FixedStringMatchesACharArray)
    {
        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            minstd::fixed_string<24> s;
            minstd::fixed_string<24> other;
            divergence d;

            string_differential<minstd::basic_string<char>, 24>(s, other, seed, d);

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }

    TEST(DifferentialContainerTests, DynamicStringMatchesACharArray)
    {
        minstd::pmr::polymorphic_allocator<char> allocator(&heap);

        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            minstd::dynamic_string<96> s(allocator);
            minstd::dynamic_string<96> other(allocator);
            divergence d;

            string_differential<minstd::basic_string<char>, 96>(s, other, seed, d);

            if (d.found)
            {
                FAIL(d.text);
            }
        }
    }
}
