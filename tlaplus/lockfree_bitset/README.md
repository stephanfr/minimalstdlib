# bitset TLA+ model

`LockfreeBitset.tla` models `include/lockfree/bitset`: `acquire(i)` is one `fetch_or` and `release(i)` one
`fetch_and` on the word holding bit `i`; each returns whether the bit was clear (acquire) or set (release).

- **Contexts** are CPU × interrupt depth. A handler can run on a CPU between any two steps of the code it interrupted,
  and only the innermost context of a CPU runs.
- **Acquire:** any context may try to acquire any bit it does not hold, and the attempt may fail.
- **Release:** a context releases only bits it owns, as callers must.
- **Ownership** is taken from the value each RMW returns, so the invariants check what callers rely on.
- **One word:** bits in different words never interact, so one word is enough.

| Invariant | Meaning |
|-----------|---------|
| `MutualExclusion` | no two contexts hold the same bit |
| `OwnedImpliesSet`, `SetImpliesOwned` | the word and the holders agree |
| `ReleaseByOwnerSucceeds` | an owner's release returns `SUCCESS` |

## Configurations and results (TLC 1.8, exhaustive)

| Config | Parameters | Result |
|--------|------------|--------|
| `LockfreeBitset_small.cfg` | 2 bits, 2 CPUs, no interrupts | no error, 18 distinct states |
| `LockfreeBitset.cfg` | 2 bits, 2 CPUs, 1 interrupt level | no error, 200 distinct states |
| `LockfreeBitset_stress.cfg` | 3 bits, 3 CPUs, 2 interrupt levels | no error, 54,000 distinct states |

```bash
java -cp ../tla2tools.jar tlc2.TLC -config LockfreeBitset.cfg -workers auto LockfreeBitset.tla
```
