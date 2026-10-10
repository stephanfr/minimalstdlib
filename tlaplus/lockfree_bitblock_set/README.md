# bitblock_set TLA+ model

`BitblockSet.tla` models `include/lockfree/bitblock_set`: single-word acquire/release (one atomic step: `fetch_or`/`fetch_and` or a CAS loop) and the multi-word paths `acquire_multi_word` / `release_multi_word`, step by step, including the undo steps on their failure paths. Contexts are CPU × interrupt depth, so an interrupt handler can run between any two steps of an interrupted operation on the same CPU.

- **Acquire, multi-word:** `fetch_or` on the first word; on a conflict, clear only the bits it set. Then `fetch_or` on the last word; on a conflict, clear only the last-word bits it set, then give back the first word.
- **Release, multi-word:** CAS on the first word (fails if not all of its bits are set); then `fetch_and` on the last word; if those bits were not all set, restore them and the first word.

Bits set by an acquire that does not (yet) own them are tracked in `tentative` until they are confirmed or undone.

| Invariant | Meaning |
|-----------|---------|
| `MutualExclusion` | no two contexts own the same bit |
| `OwnedImpliesSet` | every owned bit is set |
| `NoOrphanBits` (in `StrictSafety`) | every set bit is owned or tentatively held |
| `TentativeSafe` (in `StrictSafety`) | tentative bits never overlap another context's |

The model was written against code whose failed multi-word acquire did not roll back the bits it set; `BitblockSet_strict.cfg` then failed on `NoOrphanBits`. The rollbacks are in the code now, and the strict configuration is expected to pass.

## Configurations and results (TLC 1.8)

| Config | Parameters | Invariant | Result |
|--------|------------|-----------|--------|
| `BitblockSet_small.cfg` | 2 words × 2 bits, 2 CPUs, no interrupts, ops ≤ 2 bits | `Safety` | no error, 48,388 distinct states (exhaustive) |
| `BitblockSet_strict_small.cfg` | 2 words × 2 bits, 2 CPUs + 1 interrupt level, ops ≤ 3 bits, 1 op per context | `StrictSafety` | no error, 5,051,832 distinct states (exhaustive) |
| `BitblockSet.cfg` | 2 words × 3 bits, 2 CPUs + 1 interrupt level, 2 ops per context | `Safety` | no error in 17,059,550 distinct states (stopped after 20 min, not exhaustive) |
| `BitblockSet_strict.cfg` | as `BitblockSet.cfg` | `StrictSafety` | no error in 12,620,910 distinct states (stopped after 18 min, not exhaustive) |
| `BitblockSet_stress.cfg` | 2 words × 3 bits, 3 CPUs, 2 interrupt levels, 3 ops | `Safety` | not run: far larger than `BitblockSet.cfg`; use `-simulate` |

With the first-word rollback removed from `AcquireMultiFirstUndo` (the original bug), `BitblockSet_strict_small.cfg` fails `StrictSafety` within seconds.

```bash
java -cp ../tla2tools.jar tlc2.TLC -config BitblockSet_strict_small.cfg -workers auto BitblockSet.tla
```
