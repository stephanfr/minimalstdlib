// Compiled WITHOUT -Iinclude so #include <atomic>, <mutex> etc. resolve to
// the real standard library rather than the minstd overlay headers.

#include "moodycamel_runner.h"
#include "moodycamel_queue.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <atomic>

struct mc_item { uint64_t seq; };

static double mc_now_sec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

struct mc_state
{
    moodycamel::ConcurrentQueue<mc_item> *q;
    size_t items_per_producer;
    size_t num_producers;
    std::atomic<size_t> finished_producers{0};
    std::atomic<size_t> popped{0};
};

struct mc_prod_args { mc_state *s; };

static void *mc_produce(void *arg)
{
    auto *a = static_cast<mc_prod_args *>(arg);
    mc_state *s = a->s;
    delete a;

    moodycamel::ProducerToken tok(*s->q);
    for (size_t i = 0; i < s->items_per_producer; ++i)
        s->q->enqueue(tok, mc_item{i});

    s->finished_producers.fetch_add(1, std::memory_order_acq_rel);
    return nullptr;
}

static void *mc_consume(void *arg)
{
    auto *s = static_cast<mc_state *>(arg);
    size_t total = s->items_per_producer * s->num_producers;

    moodycamel::ConsumerToken tok(*s->q);
    while (true)
    {
        mc_item item{};
        if (s->q->try_dequeue(tok, item))
        {
            s->popped.fetch_add(1, std::memory_order_acq_rel);
        }
        else if (s->finished_producers.load(std::memory_order_acquire) == s->num_producers &&
                 s->popped.load(std::memory_order_acquire) >= total)
        {
            break;
        }
    }
    return nullptr;
}

extern "C" double run_moodycamel(size_t num_producers, size_t total_items)
{
    size_t items_per_producer = total_items / num_producers;

    moodycamel::ConcurrentQueue<mc_item> q;
    mc_state state;
    state.q = &q;
    state.items_per_producer = items_per_producer;
    state.num_producers = num_producers;

    pthread_t producers[8];  // sized for max producers used in tests
    pthread_t consumer;

    double start = mc_now_sec();

    for (size_t i = 0; i < num_producers; ++i)
        pthread_create(&producers[i], nullptr, mc_produce, new mc_prod_args{&state});
    pthread_create(&consumer, nullptr, mc_consume, &state);

    for (size_t i = 0; i < num_producers; ++i)
        pthread_join(producers[i], nullptr);
    pthread_join(consumer, nullptr);

    size_t actual_total = items_per_producer * num_producers;
    return (double)actual_total / (mc_now_sec() - start);
}
