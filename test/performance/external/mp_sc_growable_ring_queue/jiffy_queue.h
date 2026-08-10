// Vendored from https://github.com/DolevAdas/Jiffy (MIT license).
// Adapted for minstd::atomic: std::atomic -> minstd::atomic, std::memory_order_*
// -> minstd::memory_order_*, helpers renamed with jiffy_ prefix to avoid ODR
// conflicts, macros given JIFFY_ prefix, aligned_node typedef removed.

#pragma once

#include <minstdconfig.h>
#include <atomic>
#include <string.h>
#include <stdlib.h>

#define JIFFY_PAGE_SIZE      4096
#define JIFFY_CACHE_LINE     64
#define JIFFY_NODE_SIZE      1620

static inline void *jiffy_align_malloc(size_t align, size_t size)
{
    void *ptr;
    if (posix_memalign(&ptr, align, size) != 0)
        abort();
    return ptr;
}

static inline void jiffy_align_free(void *ptr)
{
    free(ptr);
}

template <class T>
class JiffyMpScQueue
{
private:
    class Node
    {
    public:
        T data;
        minstd::atomic<char> is_set;

        Node() : data(), is_set(0) {}
        Node(T d) : data(d), is_set(0) {}
        Node(const Node &n)
        {
            data = n.data;
            is_set = n.is_set;
        }
    };

    class bufferList
    {
    public:
        Node currbuffer[JIFFY_NODE_SIZE] alignas(JIFFY_CACHE_LINE);
        minstd::atomic<bufferList *> next alignas(JIFFY_CACHE_LINE);
        bufferList *prev alignas(JIFFY_CACHE_LINE);
        unsigned int head;
        unsigned int positionInQueue;

        bufferList()
            : next(nullptr), prev(nullptr), head(0), positionInQueue(0) {}

        bufferList(unsigned int /*sz*/)
            : next(nullptr), prev(nullptr), head(0), positionInQueue(1)
        {
        }

        bufferList(unsigned int /*sz*/, unsigned int pos, bufferList *p)
            : next(nullptr), prev(p), head(0), positionInQueue(pos)
        {
        }
    };

    bufferList *headOfQueue;
    unsigned int bufferSize;
    minstd::atomic<bufferList *> tailOfQueue alignas(JIFFY_CACHE_LINE);
    minstd::atomic<uint_fast64_t> gTail alignas(JIFFY_CACHE_LINE);

public:
    JiffyMpScQueue()
        : bufferSize(JIFFY_NODE_SIZE), tailOfQueue(nullptr), gTail(0)
    {
        void *buffer = jiffy_align_malloc(JIFFY_PAGE_SIZE, sizeof(bufferList));
        headOfQueue = new (buffer) bufferList(bufferSize);
        tailOfQueue = headOfQueue;
    }

    ~JiffyMpScQueue()
    {
        while (headOfQueue->next.load(minstd::memory_order_acquire) != nullptr)
        {
            bufferList *next = headOfQueue->next.load(minstd::memory_order_acquire);
            jiffy_align_free(headOfQueue);
            headOfQueue = next;
        }
        jiffy_align_free(headOfQueue);
    }

    // Single-consumer dequeue; returns false when the queue appears empty.
    bool dequeue(T &data)
    {
        while (true)
        {
            bufferList *tempTail = tailOfQueue.load(minstd::memory_order_seq_cst);
            unsigned int prevSize = bufferSize * (tempTail->positionInQueue - 1);

            if ((headOfQueue == tailOfQueue.load(minstd::memory_order_acquire)) &&
                (headOfQueue->head == (gTail.load(minstd::memory_order_acquire) - prevSize)))
            {
                return false;
            }
            else if (headOfQueue->head < bufferSize)
            {
                Node *n = &(headOfQueue->currbuffer[headOfQueue->head]);

                if (n->is_set.load(minstd::memory_order_acquire) == 2)
                {
                    headOfQueue->head++;
                    continue;
                }

                bufferList *tempHeadOfQueue = headOfQueue;
                unsigned int tempHead = tempHeadOfQueue->head;
                bool flag_moveToNewBuffer = false, flag_buffer_all_handeld = true;

                while (n->is_set.load(minstd::memory_order_acquire) == 0)
                {
                    if (tempHead < bufferSize)
                    {
                        Node *tn = &(tempHeadOfQueue->currbuffer[tempHead++]);

                        if (tn->is_set.load(minstd::memory_order_acquire) == 1 &&
                            n->is_set.load(minstd::memory_order_acquire) == 0)
                        {
                            bufferList *scanHeadOfQueue = headOfQueue;
                            for (unsigned int scanHead = scanHeadOfQueue->head;
                                 (scanHeadOfQueue != tempHeadOfQueue ||
                                  (scanHead < (tempHead - 1) &&
                                   n->is_set.load(minstd::memory_order_acquire) == 0));
                                 scanHead++)
                            {
                                if (scanHead >= bufferSize)
                                {
                                    scanHeadOfQueue = scanHeadOfQueue->next.load(minstd::memory_order_acquire);
                                    scanHead = scanHeadOfQueue->head;
                                    continue;
                                }
                                Node *scanN = &(scanHeadOfQueue->currbuffer[scanHead]);
                                if (scanN->is_set.load(minstd::memory_order_acquire) == 1)
                                {
                                    tempHead = scanHead;
                                    tempHeadOfQueue = scanHeadOfQueue;
                                    tn = scanN;
                                    scanHeadOfQueue = headOfQueue;
                                    scanHead = scanHeadOfQueue->head;
                                }
                            }

                            if (n->is_set.load(minstd::memory_order_acquire) == 1)
                                break;

                            data = tn->data;
                            tn->is_set.store(2, minstd::memory_order_release);
                            if (flag_moveToNewBuffer && (tempHead - 1) == tempHeadOfQueue->head)
                                tempHeadOfQueue->head++;

                            return true;
                        }

                        if (tn->is_set.load(minstd::memory_order_acquire) == 0)
                            flag_buffer_all_handeld = false;
                    }

                    if (tempHead >= bufferSize)
                    {
                        if (flag_buffer_all_handeld && flag_moveToNewBuffer)
                        {
                            if (tempHeadOfQueue == tailOfQueue.load(minstd::memory_order_acquire))
                                return false;

                            bufferList *next = tempHeadOfQueue->next.load(minstd::memory_order_acquire);
                            bufferList *prev = tempHeadOfQueue->prev;
                            if (next == nullptr)
                                return false;

                            next->prev = prev;
                            prev->next.store(next, minstd::memory_order_release);
                            jiffy_align_free(tempHeadOfQueue);

                            tempHeadOfQueue = next;
                            tempHead = tempHeadOfQueue->head;
                            flag_buffer_all_handeld = true;
                            flag_moveToNewBuffer = true;
                        }
                        else
                        {
                            bufferList *next = tempHeadOfQueue->next.load(minstd::memory_order_acquire);
                            if (next == nullptr)
                                return false;
                            tempHeadOfQueue = next;
                            tempHead = tempHeadOfQueue->head;
                            flag_moveToNewBuffer = true;
                            flag_buffer_all_handeld = true;
                        }
                    }
                }

                if (n->is_set.load(minstd::memory_order_acquire) == 1)
                {
                    headOfQueue->head++;
                    data = n->data;
                    return true;
                }
            }

            if (headOfQueue->head >= bufferSize)
            {
                if (headOfQueue == tailOfQueue.load(minstd::memory_order_acquire))
                    return false;

                bufferList *next = headOfQueue->next.load(minstd::memory_order_acquire);
                if (next == nullptr)
                    return false;

                jiffy_align_free(headOfQueue);
                headOfQueue = next;
            }
        }
    }

    // Wait-free multi-producer enqueue.
    void enqueue(T const &data)
    {
        bufferList *tempTail;
        unsigned int location = static_cast<unsigned int>(
            gTail.fetch_add(1, minstd::memory_order_seq_cst));
        bool go_back = false;

        while (true)
        {
            tempTail = tailOfQueue.load(minstd::memory_order_acquire);
            unsigned int prevSize = bufferSize * (tempTail->positionInQueue - 1);

            while (location < prevSize)
            {
                go_back = true;
                tempTail = tempTail->prev;
                prevSize -= bufferSize;
            }

            unsigned int globalSize = bufferSize + prevSize;

            if (prevSize <= location && location < globalSize)
            {
                int index = static_cast<int>(location - prevSize);
                Node *n = &(tempTail->currbuffer[index]);
                n->data = data;
                n->is_set.store(1, minstd::memory_order_relaxed);

                if (index == 1 && !go_back)
                {
                    void *buffer = jiffy_align_malloc(JIFFY_PAGE_SIZE, sizeof(bufferList));
                    bufferList *newArr = new (buffer) bufferList(
                        bufferSize, tempTail->positionInQueue + 1, tempTail);
                    bufferList *Nullptr = nullptr;
                    if (!(tempTail->next).compare_exchange_strong(
                            Nullptr, newArr, minstd::memory_order_seq_cst))
                        jiffy_align_free(newArr);
                }
                return;
            }

            if (location >= globalSize)
            {
                bufferList *next = (tempTail->next).load(minstd::memory_order_acquire);
                if (next == nullptr)
                {
                    void *buffer = jiffy_align_malloc(JIFFY_PAGE_SIZE, sizeof(bufferList));
                    bufferList *newArr = new (buffer) bufferList(
                        bufferSize, tempTail->positionInQueue + 1, tempTail);
                    bufferList *Nullptr = nullptr;
                    if ((tempTail->next).compare_exchange_strong(
                            Nullptr, newArr, minstd::memory_order_seq_cst))
                    {
                        tailOfQueue.store(newArr, minstd::memory_order_release);
                    }
                    else
                    {
                        jiffy_align_free(newArr);
                    }
                }
                else
                {
                    tailOfQueue.compare_exchange_strong(
                        tempTail, next, minstd::memory_order_seq_cst);
                }
            }
        }
    }
};
