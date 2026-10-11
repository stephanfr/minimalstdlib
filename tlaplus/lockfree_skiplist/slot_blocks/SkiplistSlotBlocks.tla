------------------------- MODULE SkiplistSlotBlocks -------------------------
(***************************************************************************)
(* Slot blocks of include/lockfree/skiplist (atomic_forward_link) shared by *)
(* every skip_list instance of one type and Tag, and the rules that decide *)
(* when a retired block's memory may be freed.                             *)
(*                                                                         *)
(* C++ steps modelled:                                                     *)
(*   unpack_ptr / pin_block    load blocks_[idx]; then read the block      *)
(*                             (slot_generation_, slot_table_ or           *)
(*                             active_slots_) - all inside a read section  *)
(*   retire_block              last slot freed: blocks_[idx] -> null, push *)
(*                             onto the static unlinked_blocks_            *)
(*   advance_and_reclaim       extract_unlinked_blocks, stamp with this    *)
(*                             instance's epoch, keep on                   *)
(*                             cpu_retired_blocks_; free each once the     *)
(*                             instance's readers have passed its epoch    *)
(*   ~skip_list                reclaim_all_retired_nodes, then the last    *)
(*                             instance frees every remaining block        *)
(*                                                                         *)
(* Key fact: once retire_block has nulled blocks_[idx], no reader can load *)
(* the block again, so only readers active at that moment can hold it.    *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - Epoch grace is abstracted to what it guarantees: a block stamped at  *)
(*    time t by instance i is freed only after every reader of i active at *)
(*    t has left its read section (LockfreeSkiplist.tla models the epochs).*)
(*  - Which slot a reader resolves does not matter: a stale link can name *)
(*    any slot, so any reader inside a read section may load the block.   *)
(*  - One block index; installs and retirements are nondeterministic.     *)
(*  - A destructor runs only when its instance has no operation in flight *)
(*    (the C++ precondition).                                              *)
(*                                                                         *)
(* Fix = "none": the code at d5d12c1.                                      *)
(* Fix = "sole": advance_and_reclaim extracts the shared list only while   *)
(*   its instance is the only live one; a destructor frees only its own   *)
(*   stamped blocks while other instances are live, and the last instance *)
(*   frees everything.                                                     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Instances,      \* instance ids in 1..9
    ReadersPer,     \* reader threads per instance
    MaxBlocks,      \* block incarnations installed at the index over the run
    LateStart,      \* instances constructed only after the run has started
    Fix             \* "none" | "sole"

ASSUME Fix \in {"none", "sole"} /\ \A i \in Instances : i \in 1..9 /\ LateStart \subseteq Instances

\* Process ids are integers so TLC can compare them: reader k of instance i is 10*i + k,
\* the slot allocator is 0, and instance i's own process (reclaim, destructor) is i.
Readers == {10 * i + k : i \in Instances, k \in 1..ReadersPer}
SLOTS == 0

(* --algorithm SkiplistSlotBlocks {
variables
    installed = 0,                          \* blocks_[idx]: 0 = null, else a block id
    nextBlock = 1,
    freed = {},                             \* blocks whose memory was deleted
    unlinked = {},                          \* static unlinked_blocks_
    retiredBlocks = [i \in Instances |-> {}],   \* cpu_retired_blocks_: [b, waitFor] records
    active = {},                            \* readers inside a read section
    live = Instances \ LateStart,           \* constructed and not yet destroyed
    destroyed = {};

define {
    InstanceOf(r) == r \div 10
    MyReaders(i) == {r \in Readers : InstanceOf(r) = i}
    MayExtract(i) == (Fix = "none") \/ (live = {i})
}

macro free_blocks(bs) {
    assert bs \cap freed = {};              \* no double free
    freed := freed \cup bs;
}

\* ---------------------------------------------------------------- readers
fair process (Reader \in Readers)
variables held = 0, rounds = 0;
{
 R_loop:
    while (rounds < 2) {
        await InstanceOf(self) \in live;
        active := active \cup {self};       \* enter_read_section
 R_load:
        held := installed;                  \* blocks_[idx].load(acquire)
        if (held = 0) { goto R_exit; };
 R_use:
        skip;                               \* read block memory (generation / table / active_slots_)
 R_exit:
        active := active \ {self};          \* exit_read_section
        rounds := rounds + 1;
    };
}

\* ------------------------------------------------- slot allocator / node reclaim
fair process (Slots = SLOTS)
{
 S_loop:
    while (TRUE) {
        either {
            await live # {} /\ installed = 0 /\ nextBlock <= MaxBlocks;  \* install a block (fresh or reinstalled)
            installed := nextBlock;
            nextBlock := nextBlock + 1;
        } or {
            await live # {} /\ installed # 0;           \* last slot freed -> retire_block
            unlinked := unlinked \cup {installed};
            installed := 0;
        } or {
            await destroyed = Instances;                 \* no operations after the last list is gone
            goto S_done;
        };
    };
 S_done:
    skip;
}

\* --------------------------------------------- each instance: reclaim, then destroy
fair process (Instance \in Instances)
variables rec = {};
{
 I_construct:
    await self \in live \/ self \in LateStart;
    if (self \notin live) { live := live \cup {self}; };
 I_loop:
    while (TRUE) {
        either {
            \* advance_and_reclaim: extract the shared list and stamp it with this instance's readers
            await unlinked # {} /\ MayExtract(self);
            retiredBlocks[self] := retiredBlocks[self] \cup {[b |-> x, waitFor |-> active \cap MyReaders(self)] : x \in unlinked};
            unlinked := {};
        } or {
            \* free the stamped blocks whose readers have all left
            await \E x \in retiredBlocks[self] : x.waitFor \cap active = {};
            with (x \in {y \in retiredBlocks[self] : y.waitFor \cap active = {}}) {
                rec := {x.b};
                retiredBlocks[self] := retiredBlocks[self] \ {x};
            };
 I_free:
            free_blocks(rec);
        } or {
            \* destroy, once this instance's readers are finished
            await \A r \in MyReaders(self) : pc[r] = "Done";
            goto I_destroy;
        };
    };
 I_destroy:
    \* reclaim_all_retired_nodes(): this instance's stamped blocks, and (d5d12c1) the shared list
    if (Fix = "none" \/ live = {self}) {
        free_blocks(unlinked \cup {x.b : x \in retiredBlocks[self]});
        unlinked := {};
    } else {
        free_blocks({x.b : x \in retiredBlocks[self]});
    };
    retiredBlocks[self] := {};
 I_last:
    live := live \ {self};
    destroyed := destroyed \cup {self};
    \* the last instance drains the shared list again and frees every installed block (the tables are reset)
    if (live = {}) {
        free_blocks(unlinked \cup (IF installed # 0 THEN {installed} ELSE {}));
        unlinked := {};
        installed := 0;
    };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, installed, nextBlock, freed, unlinked, retiredBlocks, active, 
          live, destroyed

(* define statement *)
InstanceOf(r) == r \div 10
MyReaders(i) == {r \in Readers : InstanceOf(r) = i}
MayExtract(i) == (Fix = "none") \/ (live = {i})

VARIABLES held, rounds, rec

vars == << pc, installed, nextBlock, freed, unlinked, retiredBlocks, active, 
           live, destroyed, held, rounds, rec >>

ProcSet == (Readers) \cup {SLOTS} \cup (Instances)

Init == (* Global variables *)
        /\ installed = 0
        /\ nextBlock = 1
        /\ freed = {}
        /\ unlinked = {}
        /\ retiredBlocks = [i \in Instances |-> {}]
        /\ active = {}
        /\ live = Instances \ LateStart
        /\ destroyed = {}
        (* Process Reader *)
        /\ held = [self \in Readers |-> 0]
        /\ rounds = [self \in Readers |-> 0]
        (* Process Instance *)
        /\ rec = [self \in Instances |-> {}]
        /\ pc = [self \in ProcSet |-> CASE self \in Readers -> "R_loop"
                                        [] self = SLOTS -> "S_loop"
                                        [] self \in Instances -> "I_construct"]

R_loop(self) == /\ pc[self] = "R_loop"
                /\ IF rounds[self] < 2
                      THEN /\ InstanceOf(self) \in live
                           /\ active' = (active \cup {self})
                           /\ pc' = [pc EXCEPT ![self] = "R_load"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                           /\ UNCHANGED active
                /\ UNCHANGED << installed, nextBlock, freed, unlinked, 
                                retiredBlocks, live, destroyed, held, rounds, 
                                rec >>

R_load(self) == /\ pc[self] = "R_load"
                /\ held' = [held EXCEPT ![self] = installed]
                /\ IF held'[self] = 0
                      THEN /\ pc' = [pc EXCEPT ![self] = "R_exit"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "R_use"]
                /\ UNCHANGED << installed, nextBlock, freed, unlinked, 
                                retiredBlocks, active, live, destroyed, rounds, 
                                rec >>

R_use(self) == /\ pc[self] = "R_use"
               /\ TRUE
               /\ pc' = [pc EXCEPT ![self] = "R_exit"]
               /\ UNCHANGED << installed, nextBlock, freed, unlinked, 
                               retiredBlocks, active, live, destroyed, held, 
                               rounds, rec >>

R_exit(self) == /\ pc[self] = "R_exit"
                /\ active' = active \ {self}
                /\ rounds' = [rounds EXCEPT ![self] = rounds[self] + 1]
                /\ pc' = [pc EXCEPT ![self] = "R_loop"]
                /\ UNCHANGED << installed, nextBlock, freed, unlinked, 
                                retiredBlocks, live, destroyed, held, rec >>

Reader(self) == R_loop(self) \/ R_load(self) \/ R_use(self) \/ R_exit(self)

S_loop == /\ pc[SLOTS] = "S_loop"
          /\ \/ /\ live # {} /\ installed = 0 /\ nextBlock <= MaxBlocks
                /\ installed' = nextBlock
                /\ nextBlock' = nextBlock + 1
                /\ pc' = [pc EXCEPT ![SLOTS] = "S_loop"]
                /\ UNCHANGED unlinked
             \/ /\ live # {} /\ installed # 0
                /\ unlinked' = (unlinked \cup {installed})
                /\ installed' = 0
                /\ pc' = [pc EXCEPT ![SLOTS] = "S_loop"]
                /\ UNCHANGED nextBlock
             \/ /\ destroyed = Instances
                /\ pc' = [pc EXCEPT ![SLOTS] = "S_done"]
                /\ UNCHANGED <<installed, nextBlock, unlinked>>
          /\ UNCHANGED << freed, retiredBlocks, active, live, destroyed, held, 
                          rounds, rec >>

S_done == /\ pc[SLOTS] = "S_done"
          /\ TRUE
          /\ pc' = [pc EXCEPT ![SLOTS] = "Done"]
          /\ UNCHANGED << installed, nextBlock, freed, unlinked, retiredBlocks, 
                          active, live, destroyed, held, rounds, rec >>

Slots == S_loop \/ S_done

I_construct(self) == /\ pc[self] = "I_construct"
                     /\ self \in live \/ self \in LateStart
                     /\ IF self \notin live
                           THEN /\ live' = (live \cup {self})
                           ELSE /\ TRUE
                                /\ live' = live
                     /\ pc' = [pc EXCEPT ![self] = "I_loop"]
                     /\ UNCHANGED << installed, nextBlock, freed, unlinked, 
                                     retiredBlocks, active, destroyed, held, 
                                     rounds, rec >>

I_loop(self) == /\ pc[self] = "I_loop"
                /\ \/ /\ unlinked # {} /\ MayExtract(self)
                      /\ retiredBlocks' = [retiredBlocks EXCEPT ![self] = retiredBlocks[self] \cup {[b |-> x, waitFor |-> active \cap MyReaders(self)] : x \in unlinked}]
                      /\ unlinked' = {}
                      /\ pc' = [pc EXCEPT ![self] = "I_loop"]
                      /\ rec' = rec
                   \/ /\ \E x \in retiredBlocks[self] : x.waitFor \cap active = {}
                      /\ \E x \in {y \in retiredBlocks[self] : y.waitFor \cap active = {}}:
                           /\ rec' = [rec EXCEPT ![self] = {x.b}]
                           /\ retiredBlocks' = [retiredBlocks EXCEPT ![self] = retiredBlocks[self] \ {x}]
                      /\ pc' = [pc EXCEPT ![self] = "I_free"]
                      /\ UNCHANGED unlinked
                   \/ /\ \A r \in MyReaders(self) : pc[r] = "Done"
                      /\ pc' = [pc EXCEPT ![self] = "I_destroy"]
                      /\ UNCHANGED <<unlinked, retiredBlocks, rec>>
                /\ UNCHANGED << installed, nextBlock, freed, active, live, 
                                destroyed, held, rounds >>

I_free(self) == /\ pc[self] = "I_free"
                /\ Assert(rec[self] \cap freed = {}, 
                          "Failure of assertion at line 73, column 5 of macro called at line 140, column 13.")
                /\ freed' = (freed \cup rec[self])
                /\ pc' = [pc EXCEPT ![self] = "I_loop"]
                /\ UNCHANGED << installed, nextBlock, unlinked, retiredBlocks, 
                                active, live, destroyed, held, rounds, rec >>

I_destroy(self) == /\ pc[self] = "I_destroy"
                   /\ IF Fix = "none" \/ live = {self}
                         THEN /\ Assert((unlinked \cup {x.b : x \in retiredBlocks[self]}) \cap freed = {}, 
                                        "Failure of assertion at line 73, column 5 of macro called at line 150, column 9.")
                              /\ freed' = (freed \cup (unlinked \cup {x.b : x \in retiredBlocks[self]}))
                              /\ unlinked' = {}
                         ELSE /\ Assert(({x.b : x \in retiredBlocks[self]}) \cap freed = {}, 
                                        "Failure of assertion at line 73, column 5 of macro called at line 153, column 9.")
                              /\ freed' = (freed \cup ({x.b : x \in retiredBlocks[self]}))
                              /\ UNCHANGED unlinked
                   /\ retiredBlocks' = [retiredBlocks EXCEPT ![self] = {}]
                   /\ pc' = [pc EXCEPT ![self] = "I_last"]
                   /\ UNCHANGED << installed, nextBlock, active, live, 
                                   destroyed, held, rounds, rec >>

I_last(self) == /\ pc[self] = "I_last"
                /\ live' = live \ {self}
                /\ destroyed' = (destroyed \cup {self})
                /\ IF live' = {}
                      THEN /\ Assert((unlinked \cup (IF installed # 0 THEN {installed} ELSE {})) \cap freed = {}, 
                                     "Failure of assertion at line 73, column 5 of macro called at line 161, column 9.")
                           /\ freed' = (freed \cup (unlinked \cup (IF installed # 0 THEN {installed} ELSE {})))
                           /\ unlinked' = {}
                           /\ installed' = 0
                      ELSE /\ TRUE
                           /\ UNCHANGED << installed, freed, unlinked >>
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << nextBlock, retiredBlocks, active, held, rounds, 
                                rec >>

Instance(self) == I_construct(self) \/ I_loop(self) \/ I_free(self)
                     \/ I_destroy(self) \/ I_last(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == Slots
           \/ (\E self \in Readers: Reader(self))
           \/ (\E self \in Instances: Instance(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Readers : WF_vars(Reader(self))
        /\ WF_vars(Slots)
        /\ \A self \in Instances : WF_vars(Instance(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* Every read of block memory is of a block that has not been freed.
NoAccessToFreedBlock == \A r \in Readers : pc[r] = "R_use" => held[r] \notin freed

\* Once every instance is gone, every block ever installed has been freed exactly once (no leak).
NoLeakAtEnd == (\A p \in ProcSet : pc[p] = "Done") => freed = 1..(nextBlock - 1)

Safety == NoAccessToFreedBlock /\ NoLeakAtEnd
=============================================================================
