# AArch64 litmus tests for the library's seq_cst fences

TLA+ models are sequentially consistent, so they cannot show whether the code is correct on AArch64's weaker memory
model. These tests check the three places where correctness depends on a `__atomic_thread_fence(__ATOMIC_SEQ_CST)`
(`dmb ish`). They use **herd7**, which enumerates every outcome the Arm architecture's own axiomatic model
(`aarch64.cat`) allows.

All three have the store-buffering shape: each side writes its own word, fences, then reads the other side's word. The
bug is the outcome in which both sides miss the other's write.

| Test | Code | Bad outcome (the `exists` clause) |
|------|------|-----------------------------------|
| `A-boundary-*` | `lockfree_single_arena_resource`: the frontier CAS, then the metadata count (`get_next_empty_memory_block`) vs. the count CAS, then the frontier (`grow_metadata_region`) | both claims succeed and neither sees the other: a block overlaps a metadata record |
| `B-upperlevels-*` | skiplist: `upper_levels_remaining_` `fetch_add`, then the inserter's own link vs. the remover's tombstone CAS, then `upper_levels_drained()` | the inserter publishes over an unmarked link while the remover sees no reservation and retires the node |
| `C-epoch-*` | `epoch_reclaimer_core` / `slot_policy`: `enter_read_section` (`reader_depth_` `fetch_add`, epoch store, fence), then a link load vs. an unlink CAS, then the reclaimer's `reader_depth_` load | the reader still sees the unlinked node while the reclaimer sees no reader and frees it |

Each comes in three code generations, taken from clang 18 `-O2` output for `probe.cpp` (the same atomic operations and
memory orders as the library), with and without the `DMB ISH`:

| Suffix | Codegen | RMW | Acquire load |
|--------|---------|-----|--------------|
| `a53` | `-mcpu=cortex-a53` (Armv8.0) | `LDAXR`/`STLXR` | `LDAR` |
| `a76` | `-mcpu=cortex-a76` (LSE, RCpc) | `CASAL` / `LDADDAL` | `LDAPR` |
| `llsc-rcpc` | `-mcpu=generic+rcpc`, `cortex-a55+nolse`, `cortex-a76+nolse` | `LDAXR`/`STLXR` | `LDAPR` |

Locations are 32-bit in the tests (herd7 rejects mixed sizes); only the ordering matters.

## Results (herdtools7 7.58+1, `aarch64.cat`)

| Test | with `DMB ISH` (the code) | without |
|------|---------------------------|---------|
| `A-boundary` a53 / a76 / llsc-rcpc | Never / Never / Never | Never / Never / **Sometimes** |
| `B-upperlevels` a53 / a76 / llsc-rcpc | Never / Never / Never | Never / Never / **Sometimes** |
| `C-epoch` a53 / a76 / llsc-rcpc | Never / Never / Never | Never / Never / **Sometimes** |
| `SB` / `SB+dmbs` (sanity check) | Never | Sometimes |

- With the fences the bad outcome is impossible for every codegen.
- Without them it is still impossible for the `a53` and `a76` codegens: a release write followed by an `LDAR` is
  ordered (RCsc), and `CASAL`/`LDADDAL` with both acquire and release order later loads.
- It becomes possible when an RCpc `LDAPR` follows an LL/SC release write. That is what clang emits for an RCpc target
  without LSE atomics. So the fences are needed on AArch64, and they are sufficient there.
- C++ itself only guarantees this ordering with the seq_cst fences, whatever the target.

Limits: the tests cover the instruction sequences around each fence, not whole functions; they model the address-free
core of each pattern (two threads, one word each).

```bash
HERD=/path/to/herd7 ./run.sh
```
