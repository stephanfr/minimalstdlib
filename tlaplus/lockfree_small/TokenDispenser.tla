-------------------------- MODULE TokenDispenser --------------------------
(***************************************************************************)
(* include/lockfree/token_dispenser: get_token spins over                  *)
(*   index = counter.fetch_add(1) % N; word = tokens.load;                 *)
(*   if bit set: continue; CAS tokens word -> word | bit;                  *)
(* release_token: tokens.and_fetch(~bit).                                  *)
(* get_token waits while every token is held (intended blocking).         *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS N, Threads, RoundsPer

(* --algorithm TokenDispenser {
variables counter = 0, tokens = {}, holder = [i \in 0..(N - 1) |-> 0];

fair process (T \in Threads)
variables rounds = 0, idx = 0, word = {};
{
 L: while (rounds < RoundsPer) {
        rounds := rounds + 1;
 G1:    idx := counter % N; counter := (counter + 1) % (2 * N);   \* fetch_add (wrap kept small)
 G2:    word := tokens;
        if (idx \in word) { goto G1; };
 G3:    if (tokens = word) {
            tokens := word \cup {idx};
            assert holder[idx] = 0;                           \* nobody else holds this token
            holder[idx] := self;
        } else { goto G1; };
 R1:    holder[idx] := 0;
        tokens := tokens \ {idx};                            \* release_token (and_fetch)
    };
}
} *)

\* BEGIN TRANSLATION
VARIABLES pc, counter, tokens, holder, rounds, idx, word

vars == << pc, counter, tokens, holder, rounds, idx, word >>

ProcSet == (Threads)

Init == (* Global variables *)
        /\ counter = 0
        /\ tokens = {}
        /\ holder = [i \in 0..(N - 1) |-> 0]
        (* Process T *)
        /\ rounds = [self \in Threads |-> 0]
        /\ idx = [self \in Threads |-> 0]
        /\ word = [self \in Threads |-> {}]
        /\ pc = [self \in ProcSet |-> "L"]

L(self) == /\ pc[self] = "L"
           /\ IF rounds[self] < RoundsPer
                 THEN /\ rounds' = [rounds EXCEPT ![self] = rounds[self] + 1]
                      /\ pc' = [pc EXCEPT ![self] = "G1"]
                 ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                      /\ UNCHANGED rounds
           /\ UNCHANGED << counter, tokens, holder, idx, word >>

G1(self) == /\ pc[self] = "G1"
            /\ idx' = [idx EXCEPT ![self] = counter % N]
            /\ counter' = (counter + 1) % (2 * N)
            /\ pc' = [pc EXCEPT ![self] = "G2"]
            /\ UNCHANGED << tokens, holder, rounds, word >>

G2(self) == /\ pc[self] = "G2"
            /\ word' = [word EXCEPT ![self] = tokens]
            /\ IF idx[self] \in word'[self]
                  THEN /\ pc' = [pc EXCEPT ![self] = "G1"]
                  ELSE /\ pc' = [pc EXCEPT ![self] = "G3"]
            /\ UNCHANGED << counter, tokens, holder, rounds, idx >>

G3(self) == /\ pc[self] = "G3"
            /\ IF tokens = word[self]
                  THEN /\ tokens' = (word[self] \cup {idx[self]})
                       /\ Assert(holder[idx[self]] = 0, 
                                 "Failure of assertion at line 26, column 13.")
                       /\ holder' = [holder EXCEPT ![idx[self]] = self]
                       /\ pc' = [pc EXCEPT ![self] = "R1"]
                  ELSE /\ pc' = [pc EXCEPT ![self] = "G1"]
                       /\ UNCHANGED << tokens, holder >>
            /\ UNCHANGED << counter, rounds, idx, word >>

R1(self) == /\ pc[self] = "R1"
            /\ holder' = [holder EXCEPT ![idx[self]] = 0]
            /\ tokens' = tokens \ {idx[self]}
            /\ pc' = [pc EXCEPT ![self] = "L"]
            /\ UNCHANGED << counter, rounds, idx, word >>

T(self) == L(self) \/ G1(self) \/ G2(self) \/ G3(self) \/ R1(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Threads: T(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Threads : WF_vars(T(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

Safety == \A i \in 0..(N - 1) : (holder[i] # 0) <=> (i \in tokens)
=============================================================================
