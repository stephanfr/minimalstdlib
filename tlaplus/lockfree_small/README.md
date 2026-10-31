# TLA+ models of the smaller lock-free classes

Each model has one label per shared-memory access. Each checks safety and `Termination` under weak fairness, or names
the intended blocking condition.

| Model | C++ | Checked | Config | Result |
|-------|-----|---------|--------|--------|
| `EliminationArray.tla` | `include/lockfree/elimination_array`: one slot's state machine | every element handed over exactly once (asserts, `ExactlyOnce`); `Termination` | `EliminationArray.cfg`: 3 threads × 2 ops | no error, 206,980 distinct states |
| | | the same, with thread 2 an interrupt handler on thread 1's CPU | `EliminationArray_irq.cfg`: 3 threads × 2 ops | no error, 174,124 distinct states |
| `LatestValueSlotArray.tla` | `include/lockfree/latest_value_slot_array`: seqlock slots, packed `(seq, STORED_FLAG \| slot)` `current_`, `writer_busy_` try-lock | `writer_busy_` held by exactly one store; the store never writes the published slot; no torn load; every load linearisable (returns a value published at some instant while it ran) and returns `EMPTY` exactly when that value is the initial one; `FairDone` | `LatestValueSlotArray.cfg`: 2 slots, 2-word payload, 2 writers × 3 stores, 1 reader × 2 loads (readers write nothing shared, so one is enough) | no error, 927,072 distinct states; a mutant reader that always reports a stored value fails the `EMPTY` assertion |
| | | the same, with writer 2 unfair: it may stop forever at any step, including mid-store holding `writer_busy_`; writer 1 and the reader must still finish (lock-freedom) | `LatestValueSlotArray_stalled_writer.cfg` | no error, 927,072 distinct states; a mutant reader that waits while `writer_busy_` is set violates `FairDone` |
| | | the same, with reader 3 an interrupt handler on writer 1's CPU and writer 2 one on reader 3's (nested) | `LatestValueSlotArray_irq.cfg`: 2 writers × 3 stores, 1 reader × 2 loads | no error, 40,192 distinct states |
| | | expected to fail: the first version of `load()`, which accepted any even, unchanged `seq_` without comparing it to the published one | `LatestValueSlotArray_bug_unpublished_read.cfg` | linearisability assertion fails: a lapped reader returns a value written but not yet published, and a later load the older published one |
| | | expected to fail: `seq_` wraps at 4 | `LatestValueSlotArray_bug_seq_wraps.cfg` | an assertion fails; documents the 32-bit `seq_` assumption |
| `SpMcValueStack.tla` | `include/lockfree/sp_mc_value_stack`: `(generation, count)` state, single producer | nothing popped that was not pushed, nothing lost; `Termination` | `SpMcValueStack.cfg`: 1 producer (4 pushes), 2 consumers × 2 pops | no error, 116,609 distinct states |
| `SpscQueue.tla` | `include/lockfree/spsc_queue` | neither side touches a slot the other owns; FIFO; nothing lost; the destructor destroys exactly the live elements; `Termination` | `SpscQueue.cfg`: 3 slots, 4 pushes, 3 pops | no error, 852 distinct states |
| `TokenDispenser.tla` | `include/lockfree/token_dispenser` | no token held twice; `Termination` (`get_token` blocks while every token is held, which is intended) | `TokenDispenser.cfg`: 2 tokens, 3 threads × 2 rounds | no error, 335,387 distinct states |

**Elimination array and interrupts:** with a handler on the same CPU, the interrupted thread does not run until the
handler finishes, and the run still terminates. A handler only spins after its own CAS installed `WAITING_GET` or
`WAITING_PUT`. The thread it interrupted froze before that install, so it is never the partner the handler waits for.
A slot left mid-transition just makes the handler's CAS fail. The C++ tries three random slots, while the model uses
one slot with repeated attempts, which covers every interleaving on a slot because slots are independent.

**Weak memory:** each class publishes with a plain release/acquire pair, in the style of the `H-mpsc-ready` litmus
test:
- `spsc_queue` publishes through its indices;
- `sp_mc_value_stack` through `state_`: every push CAS is a release, and pops extend its release sequence;
- `elimination_array` through `state_`;
- `token_dispenser` through its CAS and `and_fetch`;
- `latest_value_slot_array` through `current_` (release/acquire), plus the seqlock fence pair: the writer's
  release fence after the odd `seq_` store and the reader's acquire fence before its second `seq_` load. The
  payload words are relaxed atomics, so if a reader's copy saw any word of a lapping store, the fences pair up
  and that second load sees the odd (or a later) `seq_`, so the copy is retried. No litmus test covers this yet.

**Latest value slot array and interrupts:** an interrupting writer finds `writer_busy_` set and drops its value;
an interrupting reader reads the published slot, which the frozen writer is not writing, so neither waits on the
thread it interrupted.
