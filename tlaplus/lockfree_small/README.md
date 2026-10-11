# TLA+ models of the smaller lock-free classes

Each model has one label per shared-memory access. Each checks safety and `Termination` under weak fairness, or names
the intended blocking condition.

| Model | C++ | Checked | Config | Result |
|-------|-----|---------|--------|--------|
| `EliminationArray.tla` | `include/lockfree/elimination_array`: one slot's state machine | every element handed over exactly once (asserts, `ExactlyOnce`); `Termination` | `EliminationArray.cfg`: 3 threads × 2 ops | no error, 206,980 distinct states |
| | | the same, with thread 2 an interrupt handler on thread 1's CPU | `EliminationArray_irq.cfg`: 3 threads × 2 ops | no error, 174,124 distinct states |
| `SpMcValueStack.tla` | `include/lockfree/sp_mc_value_stack`: `(generation, count)` state, single producer | nothing popped that was not pushed, nothing lost; `Termination` | `SpMcValueStack.cfg`: 1 producer (4 pushes), 2 consumers × 2 pops | no error, 116,609 distinct states |
| | | the same, with a 1-value generation | `SpMcValueStack_genwrap.cfg` | violated (ABA, which the 32-bit generation prevents) |
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
- `token_dispenser` through its CAS and `and_fetch`.
