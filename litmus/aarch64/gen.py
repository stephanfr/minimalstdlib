#!/usr/bin/env python3
"""Generate the D-I litmus tests (the A-C tests and SB sanity checks are hand-written).

    python3 gen.py        (writes the .litmus files next to this script)

Each test is generated for the three code generations described in README.md:

    a53        LL/SC RMW (LDAXR/STLXR), RCsc acquire loads (LDAR), STLR release stores
    a76        LSE RMW (CASAL / LDADDAL), RCpc acquire loads (LDAPR), STLR release stores
    llsc-rcpc  LL/SC RMW (LDAXR/STLXR), RCpc acquire loads (LDAPR), STLR release stores

An RMW is written without its retry loop: the exists clause requires the LL/SC store-exclusive to succeed
(status register 0) or the CAS to read the expected value.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
CODEGENS = ("a53", "a76", "llsc-rcpc")


def acq_load(cg, dst, addr):
    return f"LDAR {dst},[{addr}]" if cg == "a53" else f"LDAPR {dst},[{addr}]"


class Thread:
    """One thread's instructions plus the conditions its RMWs must meet in the exists clause."""

    def __init__(self, pid, cg):
        self.pid, self.cg, self.ins, self.conds = pid, cg, [], []
        self.reg = 10

    def r(self):
        self.reg += 1
        return f"W{self.reg}"

    def cas(self, addr, old, new):
        """compare_exchange (acq_rel) of addr from old to new; the exists clause requires it to succeed."""
        v = self.r()
        self.ins.append(f"MOV {v},#{new}")
        if self.cg == "a76":
            e = self.r()
            self.ins += [f"MOV {e},#{old}", f"CASAL {e},{v},[{addr}]"]
            self.conds.append(f"{self.pid}:X{e[1:]}={old}")
        else:
            o, s = self.r(), self.r()
            self.ins += [f"LDAXR {o},[{addr}]", f"STLXR {s},{v},[{addr}]"]
            self.conds += [f"{self.pid}:X{o[1:]}={old}", f"{self.pid}:X{s[1:]}=0"]

    def fetch_add(self, addr, old, acquire_only=False):
        """fetch_add(1) (acq_rel, or acquire); the exists clause requires it to read old."""
        if self.cg == "a76":
            one, o = self.r(), self.r()
            self.ins += [f"MOV {one},#1", f"{'LDADDA' if acquire_only else 'LDADDAL'} {one},{o},[{addr}]"]
            self.conds.append(f"{self.pid}:X{o[1:]}={old}")
        else:
            o, n, s = self.r(), self.r(), self.r()
            store = "STXR" if acquire_only else "STLXR"
            self.ins += [f"LDAXR {o},[{addr}]", f"ADD {n},{o},#1", f"{store} {s},{n},[{addr}]"]
            self.conds += [f"{self.pid}:X{o[1:]}={old}", f"{self.pid}:X{s[1:]}=0"]

    def store(self, addr, val, release):
        v = self.r()
        self.ins += [f"MOV {v},#{val}", f"{'STLR' if release else 'STR'} {v},[{addr}]"]

    def load(self, addr, acquire):
        d = self.r()
        self.ins.append(acq_load(self.cg, d, addr) if acquire else f"LDR {d},[{addr}]")
        return f"{self.pid}:X{d[1:]}"

    def load_dep(self, base, on):
        """A plain load from base whose address depends on the register named by on (EOR zero idiom)."""
        z, d = self.r(), self.r()
        src = "W" + on.split(":X")[1]
        self.ins += [f"EOR {z},{src},{src}", f"LDR {d},[{base},{z},SXTW]"]
        return f"{self.pid}:X{d[1:]}"

    def fence(self):
        self.ins.append("DMB ISH")


def write(name, comment, init, regs, threads, exists):
    width = 34
    cols = [t.ins for t in threads]
    rows = max(len(c) for c in cols)
    lines = [f"AArch64 {name}", f'"{comment}"', "{"]
    if init:
        lines.append(init)
    for t, (pid, r) in enumerate(regs.items()):
        lines.append(f"{pid}:" + f" {pid}:".join(f"{k}={v};" for k, v in r.items()))
    lines.append("}")
    lines.append(" | ".join(f"P{i}".ljust(width) for i in range(len(threads))) + " ;")
    for row in range(rows):
        lines.append(" | ".join((c[row] if row < len(c) else "").ljust(width) for c in cols) + " ;")
    conds = [c for t in threads for c in t.conds] + exists
    lines.append("exists (" + " /\\ ".join(conds) + ")")
    with open(os.path.join(HERE, name + ".litmus"), "w") as f:
        f.write("\n".join(lines) + "\n")


def gen_D(cg, variant):
    """skiplist retire_node: final level-0 unlink CAS, retirement_enqueued_ CAS, [fence], epoch load (unlinker U);
    try_advance_epoch: fence, epoch CAS 1->2 (advancer A); reader: enter_read_section's epoch load, fence, link load.
    Bad: U stamps retired_epoch 1, the reader runs in epoch 2 and still reads the unlinked node, so after one more
    advance (reader snapshot 2 > reclaim epoch 3 - 2) the node is freed under the reader."""
    u, a, p = Thread(0, cg), Thread(1, cg), Thread(2, cg)
    u.cas("X0", 1, 2)                       # unlink: link from node(1) to successor(2)
    if variant != "control":
        u.cas("X1", 0, 1)                   # retirement_enqueued_ false -> true
    if variant == "fence":
        u.fence()
    ue = u.load("X2", True)                 # retired_epoch_ = current_epoch()
    a.fence()
    a.cas("X2", 1, 2)
    pe = p.load("X2", True)
    p.fence()
    pl = p.load("X0", True)
    write(f"D-retire-epoch-{cg}-{variant}", "unlinker / epoch advancer / reader",
          "link=1; epoch=1;",
          {0: {"X0": "link", "X1": "flag", "X2": "epoch"}, 1: {"X2": "epoch"}, 2: {"X0": "link", "X2": "epoch"}},
          [u, a, p], [f"{ue}=1", f"{pe}=2", f"{pl}=1"])


def gen_E(cg, variant):
    """lockfree_bitblock_resource hint: add_new_block constructs the block (field=1), publishes it (first_block_
    CAS), allocates from it (bitmap CAS), stores the hint; another core loads the hint and reads the block.
    old: relaxed hint store/load + address dependency (the code before this change);
    old-nodep: as old, without the dependency (C++ does not promise one for relaxed);
    fixed: release store / acquire load; control: no CASes, relaxed, no dependency."""
    w, r = Thread(0, cg), Thread(1, cg)
    w.store("X1", 1, False)                 # block constructor: plain field
    if variant != "control":
        w.cas("X2", 0, 1)                   # first_block_ publish
        w.cas("X3", 0, 1)                   # bitmap acquire
    w.store("X0", 1, variant == "fixed")    # hint store
    h = r.load("X0", variant == "fixed")
    f = r.load_dep("X1", h) if variant == "old" else r.load("X1", False)
    write(f"E-bitblock-hint-{cg}-{variant}", "block construction / hint publication",
          "", {0: {"X0": "hint", "X1": "field", "X2": "first", "X3": "bitmap"}, 1: {"X0": "hint", "X1": "field"}},
          [w, r], [f"{h}=1", f"{f}=0"])


def gen_F(cg, variant):
    """atomic_forward_link block install: relaxed slot_generation_ seeding, then blocks_[idx] CAS (release);
    unpack_ptr: blocks_[idx] acquire load, then slot_generation_ load.  control: relaxed CAS order / plain load."""
    w, r = Thread(0, cg), Thread(1, cg)
    w.store("X1", 1, False)                 # slot_generation_ seeding
    if variant == "code":
        w.cas("X0", 0, 1)                   # blocks_[idx] install (release; acq_rel codegen is no weaker)
    else:
        w.store("X0", 1, False)
    b = r.load("X0", variant == "code")
    g = r.load("X1", False)
    write(f"F-slot-install-{cg}-{variant}", "slot block seeding / install vs unpack_ptr",
          "", {0: {"X0": "blocks", "X1": "gen"}, 1: {"X0": "blocks", "X1": "gen"}}, [w, r], [f"{b}=1", f"{g}=0"])


def gen_G(cg, variant):
    """mp_sc_growable_ring_queue regrow: consumer writes buffer_ (plain), enqueue_pos_ store (release);
    producer: enqueue_pos_ fetch_add (acquire), then buffer_ load.  control: relaxed store."""
    c, p = Thread(0, cg), Thread(1, cg)
    c.store("X1", 1, False)                 # buffer_ / capacity_
    c.store("X0", 1, variant == "code")     # enqueue_pos_ = pack(capacity, 0)
    p.fetch_add("X0", 1, acquire_only=True)
    bu = p.load("X1", False)
    write(f"G-mpsc-regrow-{cg}-{variant}", "consumer regrow / producer claim",
          "", {0: {"X0": "pos", "X1": "buffer"}, 1: {"X0": "pos", "X1": "buffer"}}, [c, p], [f"{bu}=0"])


def gen_H(cg, variant):
    """mp_sc slot publication: producer constructs the element (plain), ready_ store (release); consumer: ready_
    load (acquire), element read.  control: relaxed store and load."""
    p, c = Thread(0, cg), Thread(1, cg)
    p.store("X1", 1, False)
    p.store("X0", 1, variant == "code")
    rd = c.load("X0", variant == "code")
    el = c.load("X1", False)
    write(f"H-mpsc-ready-{cg}-{variant}", "slot publication",
          "", {0: {"X0": "ready", "X1": "elem"}, 1: {"X0": "ready", "X1": "elem"}}, [p, c], [f"{rd}=1", f"{el}=0"])


def gen_I(cg, variant):
    """skiplist live_lists_: retire_block nulls blocks_[idx] (CAS), later sole_live_instance(): fence, live_lists_
    load (sees 1: frees the block after this list's grace period) vs a new skip_list constructor: live_lists_
    fetch_add, fence, then one of its readers loads blocks_[idx] (sees the block).  Bad: both."""
    a, b = Thread(0, cg), Thread(1, cg)
    a.cas("X0", 1, 0)
    if variant == "code":
        a.fence()
    lv = a.load("X1", True)
    b.fetch_add("X1", 1)
    if variant == "code":
        b.fence()
    bl = b.load("X0", True)
    write(f"I-live-lists-{cg}-{variant}", "slot block retirement vs new list",
          "blocks=1; live=1;", {0: {"X0": "blocks", "X1": "live"}, 1: {"X0": "blocks", "X1": "live"}},
          [a, b], [f"{lv}=1", f"{bl}=1"])


def gen_J(cg, variant):
    """skiplist slot-block retirement: retire_block nulls blocks_[idx] (CAS) and pushes the block (release CAS) on
    unlinked_blocks_ (thread X); advance_and_reclaim extracts the list (acq_rel exchange), [fence], and stamps the
    epoch (R); an advancer moves the epoch 1 -> 2; a reader in epoch 2 loads blocks_[idx].  Bad: stamp 1, the reader
    in epoch 2 still finds the block.  control: relaxed push and extract, no fence."""
    x, r, a, p = Thread(0, cg), Thread(1, cg), Thread(2, cg), Thread(3, cg)
    x.cas("X0", 1, 0)
    if variant == "control":
        x.store("X1", 1, False)
        lst = r.load("X1", False)
    else:
        x.cas("X1", 0, 1)
        r.cas("X1", 1, 0)
        lst = None
    if variant == "fence":
        r.fence()
    st = r.load("X2", True)
    a.fence()
    a.cas("X2", 1, 2)
    pe = p.load("X2", True)
    p.fence()
    pb = p.load("X0", True)
    write(f"J-block-stamp-{cg}-{variant}", "block retire / reclaimer stamp / epoch advancer / reader",
          "blocks=1; epoch=1;",
          {0: {"X0": "blocks", "X1": "list"}, 1: {"X1": "list", "X2": "epoch"}, 2: {"X2": "epoch"},
           3: {"X0": "blocks", "X2": "epoch"}},
          [x, r, a, p], ([f"{lst}=1"] if lst else []) + [f"{st}=1", f"{pe}=2", f"{pb}=1"])


for cg in CODEGENS:
    for v in ("code", "fence", "control"):
        gen_J(cg, v)
    for v in ("code", "fence", "control"):
        gen_D(cg, v)
    for v in ("old", "old-nodep", "fixed", "control"):
        gen_E(cg, v)
    for v in ("code", "control"):
        gen_F(cg, v)
        gen_G(cg, v)
        gen_H(cg, v)
        gen_I(cg, v)
