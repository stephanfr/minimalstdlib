#include <minstdconfig.h>
#include <atomic>
using namespace minstd;
//  A: frontier/metadata boundary (lockfree_single_arena_resource.h)
bool a_allocator(atomic<uint64_t> &frontier, atomic<size_t> &count, uint64_t cur, uint64_t next, size_t *seen)
{
    if (!frontier.compare_exchange_strong(cur, next, memory_order_acq_rel, memory_order_acquire)) return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *seen = count.load(memory_order_acquire);
    return true;
}
bool a_grower(atomic<size_t> &count, atomic<uint64_t> &frontier, size_t c, uint64_t *seen)
{
    if (!count.compare_exchange_strong(c, c + 1, memory_order_acq_rel, memory_order_acquire)) return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *seen = frontier.load(memory_order_acquire);
    return true;
}
//  B: upper-level publication (skiplist insert / remove)
uint64_t b_inserter(atomic<int32_t> &remaining, atomic<uint64_t> &own_link)
{
    remaining.fetch_add(1, memory_order_seq_cst);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return own_link.load(memory_order_acquire);
}
int32_t b_drained(atomic<int32_t> &remaining)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return remaining.load(memory_order_acquire);
}
//  C: epoch read section vs reclaim (epoch_reclaimer_core, slot_policy)
uint64_t c_reader(atomic<uint32_t> &depth, atomic<uint64_t> &reader_epoch, atomic<uint64_t> &link, uint64_t e)
{
    depth.fetch_add(1, memory_order_acq_rel);
    reader_epoch.store(e, memory_order_release);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return link.load(memory_order_acquire);
}
uint32_t c_reclaimer(atomic<uint64_t> &link, uint64_t node, uint64_t succ, atomic<uint32_t> &depth)
{
    link.compare_exchange_strong(node, succ, memory_order_acq_rel, memory_order_acquire);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return depth.load(memory_order_acquire);
}
