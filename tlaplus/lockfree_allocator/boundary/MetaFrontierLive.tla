-------------------------- MODULE MetaFrontierLive --------------------------
\* K16 fix, lock-free on both sides: each side re-checks the other after its CAS and, on overlap, undoes its
\*   CAS once it is the top of its own stack, or keeps its claim if the overlap has gone.  No locks, so a
\*   signal/interrupt handler that allocates while the code it interrupted is mid-protocol never blocks on it.  Both re-checks follow a seq_cst fence in the C++, so (store; fence; load) on both sides means
\*   at least one side sees the other (Dekker).  This model is sequentially consistent.
\*
\*   Units are 64 bytes.  Metadata record i occupies unit MS - i; a block is units [start, next).

EXTENDS Naturals, FiniteSets

CONSTANTS MS, Allocs, Growers, Sizes

(* --algorithm MetaFrontierFixed
variables
    fw = 0,                         \* frontier (unit), packed with its predecessor in the C++ (unique per block)
    mc = 1,                         \* current_metadata_record_count_
    keptBlock = [a \in Allocs |-> <<0, 0>>],    \* <<start, next>> of a block an allocator kept, else <<0,0>>
    keptRecord = [g \in Growers |-> 0];          \* 1 + index of a record a grower kept, else 0

define
    \* No kept block contains a kept metadata record.
    NoOverlap == \A a \in Allocs, g \in Growers :
                    (keptRecord[g] # 0 /\ keptBlock[a][2] # 0) => keptBlock[a][2] <= MS - (keptRecord[g] - 1)
    \* The post-checks reuse the pre-check formulas from the C++ (one record of slack).
    Overlaps(next, count) == next >= MS - (count + 1)
end define;

\* ---------------------------------------------------------------- get_next_empty_memory_block (lock-free)
fair process Alloc \in Allocs
variables sz \in Sizes, f = 0, c = 0, nxt = 0, c2 = 0;
begin
 A_load:  f := fw;
 A_check: c := mc;
          if f + sz >= MS - (c + 1) then goto Done; end if;
 A_cas:   nxt := f + sz;
          if fw = f then fw := nxt; else f := fw; goto A_check; end if;
 A_post:  c2 := mc;                                  \* seq_cst fence, then load(count)
          if ~Overlaps(nxt, c2) then
              keptBlock[self] := <<f, nxt>>;
              goto Done;
          end if;
 A_undo:  \* overlap: wait until we are the top block or the overlap is gone (a trim), then undo
          if ~Overlaps(nxt, mc) then
              keptBlock[self] := <<f, nxt>>;
              goto Done;
          elsif fw = nxt then
              fw := f;                                   \* CAS(frontier: our tag -> previous tag)
              goto Done;                                  \* returns nullptr (out of memory)
          else
              goto A_undo;                                \* a block above ours is still being undone
          end if;
end process;

\* ---------------------------------------------------------------- metadata growth (serialised)
fair process Grow \in Growers
variables gc = 0, gf = 0;
begin
 G_count: gc := mc;
 G_load:  gf := fw;
          if MS - (gc + 1) <= gf then goto Done; end if;
 G_cas:   if mc = gc then mc := gc + 1; else goto G_count; end if;
 G_post:  \* seq_cst fence, then load(frontier); same test as the pre-check
          if MS - (gc + 1) > fw then
              keptRecord[self] := gc + 1;
              goto Done;
          elsif mc = gc + 1 then
              mc := gc;                              \* CAS(count: gc+1 -> gc): we are the top record
              goto Done;                             \* returns NULL_INDEX
          else
              goto G_post;                           \* a record above ours is still deciding
          end if;
end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "dba64e7d" /\ chksum(tla) = "8d81e2b7")
VARIABLES pc, fw, mc, keptBlock, keptRecord

(* define statement *)
NoOverlap == \A a \in Allocs, g \in Growers :
                (keptRecord[g] # 0 /\ keptBlock[a][2] # 0) => keptBlock[a][2] <= MS - (keptRecord[g] - 1)

Overlaps(next, count) == next >= MS - (count + 1)

VARIABLES sz, f, c, nxt, c2, gc, gf

vars == << pc, fw, mc, keptBlock, keptRecord, sz, f, c, nxt, c2, gc, gf >>

ProcSet == (Allocs) \cup (Growers)

Init == (* Global variables *)
        /\ fw = 0
        /\ mc = 1
        /\ keptBlock = [a \in Allocs |-> <<0, 0>>]
        /\ keptRecord = [g \in Growers |-> 0]
        (* Process Alloc *)
        /\ sz \in [Allocs -> Sizes]
        /\ f = [self \in Allocs |-> 0]
        /\ c = [self \in Allocs |-> 0]
        /\ nxt = [self \in Allocs |-> 0]
        /\ c2 = [self \in Allocs |-> 0]
        (* Process Grow *)
        /\ gc = [self \in Growers |-> 0]
        /\ gf = [self \in Growers |-> 0]
        /\ pc = [self \in ProcSet |-> CASE self \in Allocs -> "A_load"
                                        [] self \in Growers -> "G_count"]

A_load(self) == /\ pc[self] = "A_load"
                /\ f' = [f EXCEPT ![self] = fw]
                /\ pc' = [pc EXCEPT ![self] = "A_check"]
                /\ UNCHANGED << fw, mc, keptBlock, keptRecord, sz, c, nxt, c2, 
                                gc, gf >>

A_check(self) == /\ pc[self] = "A_check"
                 /\ c' = [c EXCEPT ![self] = mc]
                 /\ IF f[self] + sz[self] >= MS - (c'[self] + 1)
                       THEN /\ pc' = [pc EXCEPT ![self] = "Done"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "A_cas"]
                 /\ UNCHANGED << fw, mc, keptBlock, keptRecord, sz, f, nxt, c2, 
                                 gc, gf >>

A_cas(self) == /\ pc[self] = "A_cas"
               /\ nxt' = [nxt EXCEPT ![self] = f[self] + sz[self]]
               /\ IF fw = f[self]
                     THEN /\ fw' = nxt'[self]
                          /\ pc' = [pc EXCEPT ![self] = "A_post"]
                          /\ f' = f
                     ELSE /\ f' = [f EXCEPT ![self] = fw]
                          /\ pc' = [pc EXCEPT ![self] = "A_check"]
                          /\ fw' = fw
               /\ UNCHANGED << mc, keptBlock, keptRecord, sz, c, c2, gc, gf >>

A_post(self) == /\ pc[self] = "A_post"
                /\ c2' = [c2 EXCEPT ![self] = mc]
                /\ IF ~Overlaps(nxt[self], c2'[self])
                      THEN /\ keptBlock' = [keptBlock EXCEPT ![self] = <<f[self], nxt[self]>>]
                           /\ pc' = [pc EXCEPT ![self] = "Done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "A_undo"]
                           /\ UNCHANGED keptBlock
                /\ UNCHANGED << fw, mc, keptRecord, sz, f, c, nxt, gc, gf >>

A_undo(self) == /\ pc[self] = "A_undo"
                /\ IF ~Overlaps(nxt[self], mc)
                      THEN /\ keptBlock' = [keptBlock EXCEPT ![self] = <<f[self], nxt[self]>>]
                           /\ pc' = [pc EXCEPT ![self] = "Done"]
                           /\ fw' = fw
                      ELSE /\ IF fw = nxt[self]
                                 THEN /\ fw' = f[self]
                                      /\ pc' = [pc EXCEPT ![self] = "Done"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "A_undo"]
                                      /\ fw' = fw
                           /\ UNCHANGED keptBlock
                /\ UNCHANGED << mc, keptRecord, sz, f, c, nxt, c2, gc, gf >>

Alloc(self) == A_load(self) \/ A_check(self) \/ A_cas(self) \/ A_post(self)
                  \/ A_undo(self)

G_count(self) == /\ pc[self] = "G_count"
                 /\ gc' = [gc EXCEPT ![self] = mc]
                 /\ pc' = [pc EXCEPT ![self] = "G_load"]
                 /\ UNCHANGED << fw, mc, keptBlock, keptRecord, sz, f, c, nxt, 
                                 c2, gf >>

G_load(self) == /\ pc[self] = "G_load"
                /\ gf' = [gf EXCEPT ![self] = fw]
                /\ IF MS - (gc[self] + 1) <= gf'[self]
                      THEN /\ pc' = [pc EXCEPT ![self] = "Done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "G_cas"]
                /\ UNCHANGED << fw, mc, keptBlock, keptRecord, sz, f, c, nxt, 
                                c2, gc >>

G_cas(self) == /\ pc[self] = "G_cas"
               /\ IF mc = gc[self]
                     THEN /\ mc' = gc[self] + 1
                          /\ pc' = [pc EXCEPT ![self] = "G_post"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "G_count"]
                          /\ mc' = mc
               /\ UNCHANGED << fw, keptBlock, keptRecord, sz, f, c, nxt, c2, 
                               gc, gf >>

G_post(self) == /\ pc[self] = "G_post"
                /\ IF MS - (gc[self] + 1) > fw
                      THEN /\ keptRecord' = [keptRecord EXCEPT ![self] = gc[self] + 1]
                           /\ pc' = [pc EXCEPT ![self] = "Done"]
                           /\ mc' = mc
                      ELSE /\ IF mc = gc[self] + 1
                                 THEN /\ mc' = gc[self]
                                      /\ pc' = [pc EXCEPT ![self] = "Done"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "G_post"]
                                      /\ mc' = mc
                           /\ UNCHANGED keptRecord
                /\ UNCHANGED << fw, keptBlock, sz, f, c, nxt, c2, gc, gf >>

Grow(self) == G_count(self) \/ G_load(self) \/ G_cas(self) \/ G_post(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Allocs: Alloc(self))
           \/ (\E self \in Growers: Grow(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Allocs : WF_vars(Alloc(self))
        /\ \A self \in Growers : WF_vars(Grow(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 
=============================================================================
