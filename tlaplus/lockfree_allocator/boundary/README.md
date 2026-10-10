# Frontier / metadata boundary (single-arena resource)

`lockfree_single_arena_resource` grows its block region up from the frontier and its metadata region down from
`metadata_start_`.  These specs model just the two code paths that keep them apart:
`get_next_empty_memory_block()` (frontier side) and `grow_metadata_region()` (metadata side).

| Spec | What it models | Config | Result |
|------|----------------|--------|--------|
| `MetaFrontier.tla` | the original code: each side checks the other, then CASes its own word | `MetaFrontier_bug.cfg` (4 growers) | invariant `FrontierBelowMetadata` violated: four metadata growths between the frontier's count read and its CAS put a record inside the new block (3 growers: no error) |
| `MetaFrontierLockFree.tla` | the fix: both sides re-check after their CAS (behind a seq_cst fence in the C++); on overlap the top of each stack undoes its CAS | `MetaFrontierLockFree.cfg` (2 allocators, 4 growers) | `NoOverlap` holds |
| `MetaFrontierLive.tla` | the fix with weak fairness | `MetaFrontierLive.cfg` | `Termination` holds: the undo loops cannot livelock |

Units are 64 bytes; metadata record `i` occupies unit `MS - i`.  The models are sequentially consistent: the C++ needs
the seq_cst fences on both sides for the same guarantee on weakly ordered hardware.

Run with TLC (from this directory):

```bash
java -cp ../../tla2tools.jar pcal.trans -nocfg MetaFrontierLockFree.tla
java -cp ../../tla2tools.jar tlc2.TLC -config MetaFrontierLockFree.cfg -workers auto MetaFrontierLockFree.tla
```
