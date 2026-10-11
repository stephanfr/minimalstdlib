// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <lockfree/intrusive_tagged_stack>
#include <lockfree/tagged_ptr>

#include <atomic>
#include <stdint.h>

#include <pthread.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(LockfreeIntrusiveTaggedStackTests)
    {
    };
#pragma GCC diagnostic pop

    struct indexed_node
    {
        int value_;
        uint32_t next_index_;
        uint32_t aux_index_;
    };

    struct indexed_adapter
    {
        indexed_node *base_;
        uint32_t null_index_;

        indexed_node *to_node(uint32_t index) const
        {
            return (index == null_index_) ? nullptr : (base_ + index);
        }

        uint32_t to_link(const indexed_node *node) const
        {
            return (node == nullptr) ? null_index_ : static_cast<uint32_t>(node - base_);
        }
    };

    struct pointer_node
    {
        int value_;
        pointer_node *next_ptr_;
    };

    struct pointer_adapter
    {
        pointer_node *to_node(pointer_node *node) const
        {
            return node;
        }

        pointer_node *to_link(pointer_node *node) const
        {
            return node;
        }
    };

    static void no_backoff(size_t &retries)
    {
        ++retries;
    }

    TEST(LockfreeIntrusiveTaggedStackTests, IndexedNodesPushPopLifoOrder)
    {
        using tag_type = minstd::lockfree::tagged_ptr<indexed_node, uint16_t>;
        using stack_type = minstd::lockfree::intrusive_tagged_stack<indexed_node, tag_type, uint32_t, &indexed_node::next_index_>;

        indexed_node nodes[3] = {
            {1, UINT32_MAX, UINT32_MAX},
            {2, UINT32_MAX, UINT32_MAX},
            {3, UINT32_MAX, UINT32_MAX},
        };

        indexed_adapter adapter{nodes, UINT32_MAX};
        minstd::atomic<uint64_t> head{tag_type::make(nullptr)};

        stack_type::push(head, nodes[0], adapter, no_backoff);
        stack_type::push(head, nodes[1], adapter, no_backoff);
        stack_type::push(head, nodes[2], adapter, no_backoff);

        indexed_node *first = stack_type::pop(head, adapter, no_backoff);
        indexed_node *second = stack_type::pop(head, adapter, no_backoff);
        indexed_node *third = stack_type::pop(head, adapter, no_backoff);
        indexed_node *empty = stack_type::pop(head, adapter, no_backoff);

        POINTERS_EQUAL(&nodes[2], first);
        POINTERS_EQUAL(&nodes[1], second);
        POINTERS_EQUAL(&nodes[0], third);
        POINTERS_EQUAL(nullptr, empty);

        CHECK_EQUAL(UINT32_MAX, first->next_index_);
        CHECK_EQUAL(UINT32_MAX, second->next_index_);
        CHECK_EQUAL(UINT32_MAX, third->next_index_);
    }

    TEST(LockfreeIntrusiveTaggedStackTests, AlternateNextFieldTemplateParameterWorks)
    {
        using tag_type = minstd::lockfree::tagged_ptr<indexed_node, uint16_t>;
        using stack_type = minstd::lockfree::intrusive_tagged_stack<indexed_node, tag_type, uint32_t, &indexed_node::aux_index_>;

        indexed_node nodes[2] = {
            {10, UINT32_MAX, UINT32_MAX},
            {20, UINT32_MAX, UINT32_MAX},
        };

        indexed_adapter adapter{nodes, UINT32_MAX};
        minstd::atomic<uint64_t> head{tag_type::make(nullptr)};

        stack_type::push(head, nodes[0], adapter, no_backoff);
        stack_type::push(head, nodes[1], adapter, no_backoff);

        indexed_node *first = stack_type::pop(head, adapter, no_backoff);
        indexed_node *second = stack_type::pop(head, adapter, no_backoff);

        POINTERS_EQUAL(&nodes[1], first);
        POINTERS_EQUAL(&nodes[0], second);
        CHECK_EQUAL(UINT32_MAX, first->aux_index_);
        CHECK_EQUAL(UINT32_MAX, second->aux_index_);
    }

    TEST(LockfreeIntrusiveTaggedStackTests, PointerLinkedNodesWork)
    {
        using tag_type = minstd::lockfree::tagged_ptr<pointer_node, uint16_t>;
        using stack_type = minstd::lockfree::intrusive_tagged_stack<pointer_node, tag_type, pointer_node *, &pointer_node::next_ptr_>;

        pointer_node a{11, nullptr};
        pointer_node b{22, nullptr};

        pointer_adapter adapter;
        minstd::atomic<uint64_t> head{tag_type::make(nullptr)};

        stack_type::push(head, a, adapter, no_backoff);
        stack_type::push(head, b, adapter, no_backoff);

        pointer_node *first = stack_type::pop(head, adapter, no_backoff);
        pointer_node *second = stack_type::pop(head, adapter, no_backoff);

        POINTERS_EQUAL(&b, first);
        POINTERS_EQUAL(&a, second);
        POINTERS_EQUAL(nullptr, first->next_ptr_);
        POINTERS_EQUAL(nullptr, second->next_ptr_);
    }

    TEST(LockfreeIntrusiveTaggedStackTests, StealAllReturnsCurrentHeadChain)
    {
        using tag_type = minstd::lockfree::tagged_ptr<indexed_node, uint16_t>;
        using stack_type = minstd::lockfree::intrusive_tagged_stack<indexed_node, tag_type, uint32_t, &indexed_node::next_index_>;

        indexed_node nodes[3] = {
            {1, UINT32_MAX, UINT32_MAX},
            {2, UINT32_MAX, UINT32_MAX},
            {3, UINT32_MAX, UINT32_MAX},
        };

        indexed_adapter adapter{nodes, UINT32_MAX};
        minstd::atomic<uint64_t> head{tag_type::make(nullptr)};

        stack_type::push(head, nodes[0], adapter, no_backoff);
        stack_type::push(head, nodes[1], adapter, no_backoff);
        stack_type::push(head, nodes[2], adapter, no_backoff);

        indexed_node *stolen_head = stack_type::steal_all(head);

        POINTERS_EQUAL(&nodes[2], stolen_head);
        CHECK_EQUAL(1, static_cast<int>(stolen_head->next_index_));

        indexed_node *after_steal = stack_type::pop(head, adapter, no_backoff);
        POINTERS_EQUAL(nullptr, after_steal);

        CHECK_EQUAL(0, static_cast<int>(nodes[1].next_index_));
        CHECK_EQUAL(UINT32_MAX, nodes[0].next_index_);
    }

    TEST(LockfreeIntrusiveTaggedStackTests, ConcurrentPopPushKeepsExclusiveOwnership)
    {
        using tag_type = minstd::lockfree::tagged_ptr<indexed_node, uint16_t>;
        using stack_type = minstd::lockfree::intrusive_tagged_stack<indexed_node, tag_type, uint32_t, &indexed_node::next_index_>;

        constexpr size_t NUM_NODES = 4;
        constexpr size_t NUM_THREADS = 4;
        constexpr size_t ITERATIONS = 200000;

        static indexed_node nodes[NUM_NODES];
        static minstd::atomic<int> owners[NUM_NODES];

        struct shared_state
        {
            indexed_adapter adapter;
            minstd::atomic<uint64_t> head;
            minstd::atomic<bool> go;
            minstd::atomic<uint32_t> violations;
        };

        static shared_state state{{nodes, UINT32_MAX}, {tag_type::make(nullptr)}, {false}, {0}};

        for (size_t i = 0; i < NUM_NODES; ++i)
        {
            nodes[i] = {static_cast<int>(i), UINT32_MAX, UINT32_MAX};
            owners[i].store(0);
            stack_type::push(state.head, nodes[i], state.adapter, no_backoff);
        }

        auto worker = [](void *) -> void *
        {
            while (!state.go.load(minstd::memory_order_acquire)) {}

            for (size_t i = 0; i < ITERATIONS; ++i)
            {
                indexed_node *node = stack_type::pop(state.head, state.adapter, no_backoff);
                if (node == nullptr)
                {
                    continue;
                }

                const size_t index = static_cast<size_t>(node - nodes);

                if (owners[index].fetch_add(1) != 0) //  someone else also holds this node
                {
                    state.violations.fetch_add(1);
                }

                owners[index].fetch_sub(1);
                stack_type::push(state.head, *node, state.adapter, no_backoff);
            }
            return nullptr;
        };

        pthread_t threads[NUM_THREADS];
        for (size_t t = 0; t < NUM_THREADS; ++t)
        {
            CHECK_EQUAL(0, pthread_create(&threads[t], nullptr, worker, nullptr));
        }

        state.go.store(true, minstd::memory_order_release);

        for (size_t t = 0; t < NUM_THREADS; ++t)
        {
            pthread_join(threads[t], nullptr);
        }

        CHECK_EQUAL(0u, state.violations.load());

        size_t count = 0;
        while (stack_type::pop(state.head, state.adapter, no_backoff) != nullptr)
        {
            ++count;
        }

        CHECK_EQUAL(NUM_NODES, count); //  no node lost or duplicated
    }
}
