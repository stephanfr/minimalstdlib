---------------------------- MODULE MetaFrontier ----------------------------
\* The two places in lockfree_single_arena_resource.h where the block region (growing up from the frontier) and
\*   the metadata region (growing down from metadata_start_) are kept apart, and nothing else:
\*
\*   get_next_empty_memory_block():  tag = load(frontier);  loop { c = load(count);
\*                                    if (next >= ms - (c+1)*64) return nullptr;  CAS(frontier: tag -> next) }
\*   get_next_metadata_record_index(): c = load(count); loop { f = load(frontier);
\*                                    if (ms - (c+1)*64 <= f) fail;  CAS(count: c -> c+1) }
\*
\*   Units are 64 bytes.  Metadata record i occupies unit MS - i; the block region is units [0, fw).
\*   Each check reads the other side's variable, then CASes its own: nothing re-validates after the CAS.

EXTENDS Naturals

CONSTANTS MS, Size, Growers

(* --algorithm MetaFrontier
variables fw = 0, mc = 1;          \* one record already exists (the allocator's own)

define
    \* No metadata record lies inside the block region.
    FrontierBelowMetadata == fw <= MS - mc + 1
end define;

process Frontier = 0
variables f = 0, c = 0;
begin
 F_load:  f := fw;
 F_check: c := mc;
          if f + Size >= MS - (c + 1) then goto Done; end if;
 F_cas:   if fw = f then fw := f + Size; else f := fw; goto F_check; end if;
end process;

process Grower \in 1..Growers
variables gc = 0, gf = 0;
begin
 G_count: gc := mc;
 G_load:  gf := fw;
          if MS - (gc + 1) <= gf then goto Done; end if;
 G_cas:   if mc = gc then mc := gc + 1; else gc := mc; goto G_load; end if;
end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "47576ac4" /\ chksum(tla) = "48e72d89")
VARIABLES pc, fw, mc

(* define statement *)
FrontierBelowMetadata == fw <= MS - mc + 1

VARIABLES f, c, gc, gf

vars == << pc, fw, mc, f, c, gc, gf >>

ProcSet == {0} \cup (1..Growers)

Init == (* Global variables *)
        /\ fw = 0
        /\ mc = 1
        (* Process Frontier *)
        /\ f = 0
        /\ c = 0
        (* Process Grower *)
        /\ gc = [self \in 1..Growers |-> 0]
        /\ gf = [self \in 1..Growers |-> 0]
        /\ pc = [self \in ProcSet |-> CASE self = 0 -> "F_load"
                                        [] self \in 1..Growers -> "G_count"]

F_load == /\ pc[0] = "F_load"
          /\ f' = fw
          /\ pc' = [pc EXCEPT ![0] = "F_check"]
          /\ UNCHANGED << fw, mc, c, gc, gf >>

F_check == /\ pc[0] = "F_check"
           /\ c' = mc
           /\ IF f + Size >= MS - (c' + 1)
                 THEN /\ pc' = [pc EXCEPT ![0] = "Done"]
                 ELSE /\ pc' = [pc EXCEPT ![0] = "F_cas"]
           /\ UNCHANGED << fw, mc, f, gc, gf >>

F_cas == /\ pc[0] = "F_cas"
         /\ IF fw = f
               THEN /\ fw' = f + Size
                    /\ pc' = [pc EXCEPT ![0] = "Done"]
                    /\ f' = f
               ELSE /\ f' = fw
                    /\ pc' = [pc EXCEPT ![0] = "F_check"]
                    /\ fw' = fw
         /\ UNCHANGED << mc, c, gc, gf >>

Frontier == F_load \/ F_check \/ F_cas

G_count(self) == /\ pc[self] = "G_count"
                 /\ gc' = [gc EXCEPT ![self] = mc]
                 /\ pc' = [pc EXCEPT ![self] = "G_load"]
                 /\ UNCHANGED << fw, mc, f, c, gf >>

G_load(self) == /\ pc[self] = "G_load"
                /\ gf' = [gf EXCEPT ![self] = fw]
                /\ IF MS - (gc[self] + 1) <= gf'[self]
                      THEN /\ pc' = [pc EXCEPT ![self] = "Done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "G_cas"]
                /\ UNCHANGED << fw, mc, f, c, gc >>

G_cas(self) == /\ pc[self] = "G_cas"
               /\ IF mc = gc[self]
                     THEN /\ mc' = gc[self] + 1
                          /\ pc' = [pc EXCEPT ![self] = "Done"]
                          /\ gc' = gc
                     ELSE /\ gc' = [gc EXCEPT ![self] = mc]
                          /\ pc' = [pc EXCEPT ![self] = "G_load"]
                          /\ mc' = mc
               /\ UNCHANGED << fw, f, c, gf >>

Grower(self) == G_count(self) \/ G_load(self) \/ G_cas(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == Frontier
           \/ (\E self \in 1..Growers: Grower(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 
=============================================================================
