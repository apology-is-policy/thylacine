---------------------------- MODULE caught_target ----------------------------
(***************************************************************************)
(* Which thread of a multi-thread Proc takes a process-directed CAUGHT     *)
(* note (ARCH 8.8.3, operator votes 2026-10-12: "Prefer running/main", and *)
(* "Model it" for this hand-off).                                          *)
(*                                                                         *)
(* A Linux-phenotype thread blocked in a wait on signal(7)'s list unwinds  *)
(* it with EINTR for a caught note; every other thread takes the note at   *)
(* its EL0-return tail (the syscall/fault tail, and since #964 the IRQ     *)
(* tail). Until 2026-10-12 every post woke every blocked thread and the    *)
(* first to claim the note's family unwound, so a child's SIGCHLD unwound  *)
(* a worker's ppoll while the main thread ran (L285). Now the posting      *)
(* walk DESIGNATES one claimant: it sets the family's claim bit on that    *)
(* thread's behalf, and only a designee or a claimant may unwind for a     *)
(* claimed family. The kernel prefers a thread that is not blocked (it     *)
(* reaches a tail; an EL0 spinner is kicked into the IRQ tail) and among   *)
(* blocked ones the main thread; this model lets the walk pick ANY         *)
(* candidate, so it checks the protocol under every choice the preference  *)
(* can make, and the preference itself is pinned by tests.                 *)
(*                                                                         *)
(* A designation is a promise that its thread takes the note soon. It ends *)
(* at the thread's tail, delivered or not; when the thread enters a wait   *)
(* that will not unwind for a caught note (the hand-off, before it         *)
(* blocks); and at its exit. Each end with the note still queued owes a    *)
(* new walk. Every walk re-wakes a claim holder blocked in an unwinding    *)
(* wait, because a claim can outlive the note it was taken for (a peer's   *)
(* tail delivered it) and a later note of the family must still reach     *)
(* that holder.                                                            *)
(*                                                                         *)
(* A THREAD-DIRECTED note (operator vote 5, 2026-10-21; thread-model C4:    *)
(* tkill, tgkill, pthread_kill) has one taker, so none of the above applies *)
(* to it: it sits on the target's own word (dq, Thread.note_direct), takes  *)
(* no claim and needs no walk, and the post rouses the target itself. The   *)
(* target's tail takes it before anything queued. Under a mask it waits for *)
(* that thread's unmask, whatever its peers have masked. DIRECT_FATAL makes *)
(* the note one whose default ends the Proc: then any wait ends for it, not *)
(* only an unwinding one, and the tail runs the group's one death. With     *)
(* MAX_DIRECT = 0 none of this is reachable and the process-directed state  *)
(* space is the one the older cfgs pin, so a changed count there means the  *)
(* extension disturbed the protocol above.                                  *)
(*                                                                         *)
(* What is abstracted:                                                     *)
(*   - One note family. A tail delivers at most one note (one handler) and *)
(*     the handler runs at EL0 until rt_sigreturn (inh). Discarded notes   *)
(*     and the uncaught/terminate paths are not this model's.              *)
(*   - A wait is one of two kinds, fixed when the thread enters it:        *)
(*     "uwait" unwinds for a caught note (caught_ok and note_interruptible *)
(*     in sleep_common/tsleep_common), "hwait" does not. The kind is       *)
(*     published under the thread's wait_lock; the designation of a thread *)
(*     is decided under the same lock, so EnterWait and Walk2 are atomic   *)
(*     with respect to each other.                                         *)
(*   - Walks are serialized (each holds the Proc's threads_lock). A walk   *)
(*     is two steps: Walk1 re-wakes the claim holders and picks a          *)
(*     candidate; Walk2 re-checks it under its wait_lock and designates    *)
(*     it. Threads move between the two; an exit cannot (its EXITING       *)
(*     commit takes threads_lock). Triggers that arrive before a walk      *)
(*     starts coalesce into that walk.                                     *)
(*   - Walk2's re-check reads the wait's kind exactly (it is published     *)
(*     under the wait_lock Walk2 holds) and the exit exactly (it commits   *)
(*     under threads_lock). The mask and the handler flag are the          *)
(*     thread's own, written without either lock, so they read exactly     *)
(*     only while the thread is blocked: an awake thread may be designated *)
(*     on a stale read of either. Such a designee drops the designation at *)
(*     its tail, or hands it off as it enters a wait.                      *)
(*   - A walk run by a release skips the releasing thread, whose tail has  *)
(*     just passed the note by; handing it straight back would recreate    *)
(*     what the release ended. The skip covers that walk and its re-picks, *)
(*     and a trigger that coalesces with it cancels it.                    *)
(*   - A thread that unwound holds its own claim to its tail. On the way  *)
(*     it may wait for its 9P flush, which flush(5) bounds by the server's *)
(*     answer (an assumption, stated as fairness, not checked here), and a *)
(*     call whose original reply beat the Rflush may wait once more        *)
(*     holding the claim -- where a claim that outlived its note blocks.   *)
(*   - Posts are bounded by MAX_POSTS. A call enters at most MAX_WAITS     *)
(*     waits and then returns: a call that waits forever is modelled by a  *)
(*     wait that never ends (any wait but the flush may last forever,      *)
(*     which is what a strand needs), not by an unbounded chain of them.   *)
(*                                                                         *)
(* Properties:                                                             *)
(*   OneClaimant -- at most one thread holds the family (a designation or  *)
(*       a claim): one caught note unwinds one sleeper.                    *)
(*   NoStrand    -- a note queued forever means no thread stays a          *)
(*       candidate to take it: while any thread could take it soon, some  *)
(*       thread eventually does.                                           *)
(*   NoStolenDirect -- a thread-directed note runs on its target alone.    *)
(*   NoDirectStrand -- a direct note pending forever means its target does *)
(*       not stay able to take it.                                         *)
(*                                                                         *)
(* Knobs, each a defect the design must not have:                          *)
(*   BUGGY_NO_HANDOFF         -- a designee keeps its designation into a   *)
(*       hold wait (NoStrand).                                             *)
(*   BUGGY_UNLOCKED_DESIGNATE -- Walk2 designates its pick without the     *)
(*       re-check, so a thread that entered a hold wait since Walk1 is     *)
(*       designated inside it (NoStrand).                                  *)
(*   BUGGY_NO_REKICK          -- walks do not wake a claim holder blocked  *)
(*       in an unwinding wait (NoStrand).                                  *)
(*   BUGGY_DOUBLE_DESIGNATE   -- Walk2 designates without the family being *)
(*       open (OneClaimant).                                               *)
(*   BUGGY_EXIT_KEEPS         -- an exiting designee keeps its             *)
(*       designation (NoStrand).                                           *)
(*   BUGGY_RELEASE_NO_WAKE    -- a release with the note still queued owes *)
(*       no walk (NoStrand).                                               *)
(*   BUGGY_IRQ_NO_NOTES       -- the IRQ tail has no notes leg (#964): a   *)
(*       designated EL0 spinner never takes the note (NoStrand).           *)
(*   BUGGY_STALE_KEEPS        -- a designee entering an unwinding wait     *)
(*       keeps a designation it cannot unwind for, masked or in a handler  *)
(*       (NoStrand).                                                       *)
(*   BUGGY_DIRECT_TO_QUEUE    -- a direct post goes on the process queue,  *)
(*       which is what tkill did until C4 (NoStolenDirect).                *)
(*   BUGGY_DIRECT_NEEDS_CLAIM -- a direct note unwinds its target only if  *)
(*       the family is open, so a peer's claim blocks it (NoDirectStrand). *)
(*   BUGGY_DIRECT_CLAIMS      -- a direct unwind takes the family's claim  *)
(*       although a peer may hold it (OneClaimant).                        *)
(*   BUGGY_DIRECT_NO_ROUSE    -- the post leaves a blocked target asleep   *)
(*       (NoDirectStrand).                                                 *)
(*   BUGGY_DIRECT_NO_OBSERVE  -- a thread entering a wait does not read    *)
(*       its word after registering (I-9; NoDirectStrand).                 *)
(*   BUGGY_FATAL_UNWINDS_ONLY -- a fatal direct note ends only an unwinding*)
(*       wait, as a caught one does (NoDirectStrand).                      *)
(*   BUGGY_DIRECT_STALE_MASK  -- the post skips the rouse on the target's  *)
(*       mask, and may read one the target has since cleared: it writes it *)
(*       with no lock and no fence, and reads an empty word before it      *)
(*       blocks (NoDirectStrand). wasMasked stands for the stale read.     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Threads, MAX_POSTS, MAX_WAITS, MAX_DIRECT, DIRECT_FATAL, NONE,
          BUGGY_NO_HANDOFF, BUGGY_UNLOCKED_DESIGNATE, BUGGY_NO_REKICK,
          BUGGY_DOUBLE_DESIGNATE, BUGGY_EXIT_KEEPS, BUGGY_RELEASE_NO_WAKE,
          BUGGY_IRQ_NO_NOTES, BUGGY_STALE_KEEPS,
          BUGGY_DIRECT_TO_QUEUE, BUGGY_DIRECT_NEEDS_CLAIM, BUGGY_DIRECT_CLAIMS,
          BUGGY_DIRECT_NO_ROUSE, BUGGY_DIRECT_NO_OBSERVE, BUGGY_FATAL_UNWINDS_ONLY,
          BUGGY_DIRECT_STALE_MASK

ASSUME NONE \notin Threads

States == {"user",     \* at EL0
           "pre",      \* in a syscall body: waits, returns, exits, or masks
           "uwait",    \* blocked in a wait a caught note unwinds
           "hwait",    \* blocked in a wait a caught note does not unwind
           "hentry",   \* entering a hold wait with a designation: hand-off owed
           "uentry",   \* entering an unwinding wait with a designation it
                       \* cannot take (masked, or in a handler): hand-off owed
           "unwound",  \* unwound for its claim; returning, maybe via the flush
           "flush",    \* the claimant's 9P flush wait (a hold wait that ends)
           "cont",     \* the reply beat the Rflush: the call carries on
           "deliver",  \* the EL0-return tail's notes leg
           "release",  \* the tail's claim release
           "exited"}

VARIABLES ts,        \* per-thread state
          masked,    \* the family is masked for the thread
          inh,       \* the thread is in a handler (N-3: no delivery to it)
          woken,     \* a wakeup is pending on the thread's wait
          des,       \* designees (the walk claimed on their behalf)
          own,       \* claimants (unwound for the family, or took it in a wait)
          q,         \* caught notes of the family queued
          posted,    \* notes posted so far
          walkOwed,  \* a walk is owed (a post, or a release with q > 0)
          wpick,     \* NONE, or the candidate Walk1 picked for Walk2
          nwait,     \* waits the thread's current call has entered
          wskip,     \* NONE, or the releaser the owed walk skips
          wcur,      \* the skip of the walk in flight (for its re-pick)
          dq,        \* a thread-directed note is pending on the thread's word
          dposted,   \* thread-directed notes posted so far
          dtgt,      \* ghost: NONE, or the target of a direct post the buggy
                     \* cfg put on the queue
          stolen,    \* ghost: a direct note ran on a thread it did not name
          wasMasked  \* ghost: a mask a stale read could still see (the buggy
                     \* cfg only; FALSE otherwise, so the counts do not move)

vars == <<ts, masked, inh, woken, des, own, q, posted, walkOwed, wpick, nwait,
          wskip, wcur, dq, dposted, dtgt, stolen, wasMasked>>

\* The fields of the direct extension, for actions it does not touch.
dvars == <<dq, dposted, dtgt, stolen, wasMasked>>

Claimed  == des \cup own
Open     == Claimed = {}
Awake(t) == ts[t] \in {"user", "pre", "deliver", "release", "unwound"}

\* A thread the walk may designate: it takes the note at a tail it is on its
\* way to, or by unwinding the wait it is blocked in.
Candidate(t) == ~masked[t] /\ ~inh[t] /\ (Awake(t) \/ ts[t] = "uwait")

\* The unwind test of sleep_common's register-then-observe and of its resume.
MayUnwind(t) == q > 0 /\ ~masked[t] /\ ~inh[t] /\ (t \in Claimed \/ Open)

\* A fresh unwind returns through the flush path; a claimant carrying on after
\* its flush returns straight away (its claim already taken).
Unwind(t) == /\ own'   = own \cup {t}
             /\ des'   = des \ {t}
             /\ ts'    = [ts EXCEPT ![t] = IF t \in own THEN "deliver" ELSE "unwound"]
             /\ woken' = [woken EXCEPT ![t] = FALSE]

\* The target's own note ends the wait it is entering or blocked in. A caught
\* one follows the caught gates: the mask, and no handler running. A fatal one
\* follows thread_die_pending, which asks the mask alone.
DirectEnds(t) == /\ dq[t] /\ ~masked[t]
                 /\ DIRECT_FATAL \/ ~inh[t]
                 /\ BUGGY_DIRECT_NEEDS_CLAIM => (t \in Claimed \/ Open)

\* A direct unwind takes no claim: nobody else could have taken the note. A
\* caught one returns through the flush path, a fatal one straight to the tail.
\* A designation it holds stays until the tail, where it always ends.
UnwindDirect(t) == /\ ts'    = [ts EXCEPT ![t] = IF DIRECT_FATAL THEN "deliver" ELSE "unwound"]
                   /\ woken' = [woken EXCEPT ![t] = FALSE]
                   /\ own'   = IF BUGGY_DIRECT_CLAIMS THEN own \cup {t} ELSE own
                   /\ des'   = des

\* A fatal note ends a hold wait too.
HoldEnds(t) == DIRECT_FATAL /\ ~BUGGY_FATAL_UNWINDS_ONLY /\ dq[t] /\ ~masked[t]

TypeOK == /\ ts \in [Threads -> States]
          /\ masked \in [Threads -> BOOLEAN]
          /\ inh \in [Threads -> BOOLEAN]
          /\ woken \in [Threads -> BOOLEAN]
          /\ des \subseteq Threads /\ own \subseteq Threads
          /\ q \in 0..(MAX_POSTS + MAX_DIRECT) /\ posted \in 0..MAX_POSTS
          /\ walkOwed \in BOOLEAN
          /\ wpick \in Threads \cup {NONE}
          /\ nwait \in [Threads -> 0..MAX_WAITS]
          /\ wskip \in Threads \cup {NONE} /\ wcur \in Threads \cup {NONE}
          /\ dq \in [Threads -> BOOLEAN]
          /\ dposted \in 0..MAX_DIRECT
          /\ dtgt \in Threads \cup {NONE}
          /\ stolen \in BOOLEAN
          /\ wasMasked \in [Threads -> BOOLEAN]

Init == /\ ts = [t \in Threads |-> "user"]
        /\ masked = [t \in Threads |-> FALSE]
        /\ inh = [t \in Threads |-> FALSE]
        /\ woken = [t \in Threads |-> FALSE]
        /\ des = {} /\ own = {}
        /\ q = 0 /\ posted = 0
        /\ walkOwed = FALSE /\ wpick = NONE
        /\ nwait = [t \in Threads |-> 0]
        /\ wskip = NONE /\ wcur = NONE
        /\ dq = [t \in Threads |-> FALSE]
        /\ dposted = 0 /\ dtgt = NONE /\ stolen = FALSE
        /\ wasMasked = [t \in Threads |-> FALSE]

-----------------------------------------------------------------------------
\* The environment: a child exits, a key is pressed.
Post == /\ posted < MAX_POSTS
        /\ q' = q + 1
        /\ posted' = posted + 1
        /\ walkOwed' = TRUE
        /\ wskip' = NONE
        /\ UNCHANGED <<ts, masked, inh, woken, des, own, wpick, nwait, wcur>>
        /\ UNCHANGED dvars

\* tkill. The post holds the Proc's threads_lock, so an exited target is seen
\* exactly and nothing is posted to it. It rouses the target under the target's
\* wait_lock, where the target's entry to a wait publishes the wait and reads
\* the word, so the two are atomic with respect to each other. Masked or not: the
\* mask is its owner's, written with no lock and no fence, so the post may not act
\* on what it reads of it. A masked sleeper re-reads its word and parks again.
PostDirect(t) == /\ dposted < MAX_DIRECT /\ ts[t] # "exited"
                 /\ dposted' = dposted + 1
                 /\ IF BUGGY_DIRECT_TO_QUEUE
                       THEN /\ q' = q + 1
                            /\ walkOwed' = TRUE
                            /\ wskip' = NONE
                            /\ dtgt' = t
                            /\ UNCHANGED <<dq, woken>>
                       ELSE /\ dq' = [dq EXCEPT ![t] = TRUE]
                            /\ woken' = IF BUGGY_DIRECT_NO_ROUSE
                                           \/ (BUGGY_DIRECT_STALE_MASK /\ (masked[t] \/ wasMasked[t]))
                                        THEN woken
                                        ELSE [woken EXCEPT ![t] =
                                                @ \/ ts[t] = "uwait"
                                                  \/ (ts[t] = "hwait" /\ DIRECT_FATAL
                                                      /\ ~BUGGY_FATAL_UNWINDS_ONLY)]
                            /\ UNCHANGED <<q, walkOwed, wskip, dtgt>>
                 /\ UNCHANGED <<ts, masked, inh, des, own, posted, wpick, nwait, wcur, stolen, wasMasked>>

\* The walk: re-wake the claim holders, then pick a candidate for an open
\* family, skipping the releaser when a release owes the walk.
Walk1 == /\ walkOwed /\ wpick = NONE
         /\ walkOwed' = FALSE
         /\ woken' = IF BUGGY_NO_REKICK THEN woken
                     ELSE [t \in Threads |->
                             woken[t] \/ (t \in Claimed /\ ts[t] = "uwait" /\ q > 0)]
         /\ LET cs == {t \in Threads : Candidate(t) /\ t # wskip}
            IN IF q > 0 /\ Open /\ cs # {}
                  THEN \E c \in cs : wpick' = c
                  ELSE wpick' = NONE
         /\ wcur' = wskip
         /\ wskip' = NONE
         /\ UNCHANGED <<ts, masked, inh, des, own, q, posted, nwait>>
         /\ UNCHANGED dvars

\* Walk2's re-check, under the pick's wait_lock. The mask and the handler flag
\* read exactly only while the pick is blocked (see the header).
Fresh(c) == /\ Awake(c) \/ ts[c] = "uwait"
            /\ ts[c] = "uwait" => ~masked[c] /\ ~inh[c]

\* Designate the pick: the CAS needs the family open and a note queued; a pick
\* that is no longer fresh owes a re-pick, which keeps the walk's skip.
Walk2 == /\ wpick \in Threads
         /\ LET c     == wpick
                ok    == (BUGGY_UNLOCKED_DESIGNATE \/ Fresh(c))
                         /\ (BUGGY_DOUBLE_DESIGNATE \/ Open) /\ q > 0
                retry == ~ok /\ Open /\ q > 0
            IN /\ des' = IF ok THEN des \cup {c} ELSE des
               /\ woken' = IF ok /\ ts[c] = "uwait"
                              THEN [woken EXCEPT ![c] = TRUE] ELSE woken
               /\ walkOwed' = (walkOwed \/ retry)
               /\ wskip' = IF ~retry THEN wskip
                           ELSE IF walkOwed THEN NONE ELSE wcur
         /\ wpick' = NONE
         /\ UNCHANGED <<ts, masked, inh, own, q, posted, nwait, wcur>>
         /\ UNCHANGED dvars

-----------------------------------------------------------------------------
\* A thread at EL0.
Syscall(t)   == /\ ts[t] = "user"
                /\ ts' = [ts EXCEPT ![t] = "pre"]
                /\ nwait' = [nwait EXCEPT ![t] = 0]
                /\ UNCHANGED <<masked, inh, woken, des, own, q, posted, walkOwed, wpick, wskip, wcur>>
                /\ UNCHANGED dvars

\* rt_sigreturn: the handler ends, through the syscall tail.
Sigreturn(t) == /\ ts[t] = "user" /\ inh[t]
                /\ inh' = [inh EXCEPT ![t] = FALSE]
                /\ ts' = [ts EXCEPT ![t] = "deliver"]
                /\ UNCHANGED <<masked, woken, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                /\ UNCHANGED dvars

\* A tick or a kick: the IRQ-from-EL0 tail.
Irq(t)       == /\ ts[t] = "user"
                /\ ts' = [ts EXCEPT ![t] = IF BUGGY_IRQ_NO_NOTES THEN "user" ELSE "deliver"]
                /\ UNCHANGED <<masked, inh, woken, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                /\ UNCHANGED dvars

\* A syscall body: a wait of either kind, a return, a mask change (which
\* returns through the tail), or the thread's exit.
\* A designee that cannot take the note in this wait -- the family is masked,
\* or a handler runs, which sleep_common publishes as a hold wait -- hands the
\* designation off before it blocks.
EnterUWait(t) == /\ ts[t] = "pre" /\ nwait[t] < MAX_WAITS
                 /\ nwait' = [nwait EXCEPT ![t] = @ + 1]
                 /\ IF t \in des /\ (masked[t] \/ inh[t]) /\ ~BUGGY_STALE_KEEPS
                       THEN /\ ts' = [ts EXCEPT ![t] = "uentry"]
                            /\ UNCHANGED <<woken, des, own>>
                    ELSE IF DirectEnds(t) /\ ~BUGGY_DIRECT_NO_OBSERVE
                       THEN UnwindDirect(t)
                    ELSE IF MayUnwind(t)
                       THEN Unwind(t)
                       ELSE /\ ts' = [ts EXCEPT ![t] = "uwait"]
                            /\ woken' = [woken EXCEPT ![t] = FALSE]
                            /\ UNCHANGED <<des, own>>
                 /\ UNCHANGED <<masked, inh, q, posted, walkOwed, wpick, wskip, wcur>>
                 /\ UNCHANGED dvars

EnterHWait(t) == /\ ts[t] = "pre" /\ nwait[t] < MAX_WAITS
                 /\ nwait' = [nwait EXCEPT ![t] = @ + 1]
                 /\ ts' = [ts EXCEPT ![t] = IF t \in des /\ ~BUGGY_NO_HANDOFF THEN "hentry"
                                            ELSE IF HoldEnds(t) /\ ~BUGGY_DIRECT_NO_OBSERVE
                                                 THEN "deliver" ELSE "hwait"]
                 /\ UNCHANGED <<masked, inh, woken, des, own, q, posted, walkOwed, wpick,
                                 wskip, wcur>>
                 /\ UNCHANGED dvars

Return(t)     == /\ ts[t] = "pre"
                 /\ ts' = [ts EXCEPT ![t] = "deliver"]
                 /\ UNCHANGED <<masked, inh, woken, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars

Mask(t)       == /\ ts[t] = "pre" /\ t \notin own
                 /\ masked' = [masked EXCEPT ![t] = ~masked[t]]
                 /\ wasMasked' = IF BUGGY_DIRECT_STALE_MASK
                                     THEN [wasMasked EXCEPT ![t] = @ \/ masked[t]]
                                     ELSE wasMasked
                 /\ ts' = [ts EXCEPT ![t] = "deliver"]
                 /\ UNCHANGED <<inh, woken, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED <<dq, dposted, dtgt, stolen>>

\* The EXITING commit, under the threads_lock (so never inside a walk). The
\* exit call is a new syscall, so no claim of its own is held. A direct note
\* still on its word goes with it.
Exit(t)       == /\ ts[t] = "pre" /\ t \notin own /\ wpick = NONE
                 /\ ts' = [ts EXCEPT ![t] = "exited"]
                 /\ IF BUGGY_EXIT_KEEPS
                       THEN UNCHANGED <<des, walkOwed, wskip>>
                       ELSE LET owes == t \in des /\ q > 0
                            IN /\ des' = des \ {t}
                               /\ walkOwed' = (walkOwed \/ owes)
                               /\ wskip' = IF owes THEN NONE ELSE wskip
                 /\ UNCHANGED <<masked, inh, woken, own, q, posted, wpick, nwait, wcur>>
                 /\ UNCHANGED dvars

PreStep(t) == EnterUWait(t) \/ EnterHWait(t) \/ Return(t) \/ Mask(t) \/ Exit(t)

\* The hand-off: the designation is dropped before the thread blocks, and a
\* walk is owed while the note is queued. The register-then-observe comes after
\* it, so a fatal direct note ends the wait here.
Handoff(t)    == /\ ts[t] \in {"hentry", "uentry"}
                 /\ des' = des \ {t}
                 /\ LET owes == t \in des /\ q > 0
                    IN /\ walkOwed' = (walkOwed \/ owes)
                       /\ wskip' = IF owes THEN NONE ELSE wskip
                 /\ ts' = [ts EXCEPT ![t] =
                             IF BUGGY_DIRECT_NO_OBSERVE THEN IF ts[t] = "hentry" THEN "hwait" ELSE "uwait"
                             ELSE IF ts[t] = "hentry"
                                  THEN IF HoldEnds(t) THEN "deliver" ELSE "hwait"
                                  ELSE IF DIRECT_FATAL /\ DirectEnds(t) THEN "deliver" ELSE "uwait"]
                 /\ UNCHANGED <<masked, inh, woken, own, q, posted, wpick, nwait, wcur>>
                 /\ UNCHANGED dvars

\* A blocked thread resumes: woken by a walk or a direct post (fair), or
\* spuriously. It unwinds or re-parks; DirectEnds and MayUnwind are the
\* post-sched() test.
Resume(t)     == /\ ts[t] = "uwait"
                 /\ IF DirectEnds(t)
                       THEN UnwindDirect(t)
                    ELSE IF MayUnwind(t)
                       THEN Unwind(t)
                       ELSE /\ woken' = [woken EXCEPT ![t] = FALSE]
                            /\ UNCHANGED <<ts, des, own>>
                 /\ UNCHANGED <<masked, inh, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars
WokenResume(t) == woken[t] /\ Resume(t)

\* A fatal direct post wakes a hold wait: thread_die_pending ends every wait.
ResumeHold(t) == /\ ts[t] = "hwait" /\ woken[t] /\ DIRECT_FATAL
                 /\ ts' = [ts EXCEPT ![t] = IF HoldEnds(t) THEN "deliver" ELSE "hwait"]
                 /\ woken' = [woken EXCEPT ![t] = FALSE]
                 /\ UNCHANGED <<masked, inh, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars

\* The wait's condition comes true: the call continues, or, for a claimant
\* carrying on after its flush, returns.
WaitDone(t)   == /\ ts[t] \in {"uwait", "hwait"}
                 /\ ts' = [ts EXCEPT ![t] = IF t \in own THEN "deliver" ELSE "pre"]
                 /\ woken' = [woken EXCEPT ![t] = FALSE]
                 /\ UNCHANGED <<masked, inh, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars

\* After the unwind: the call returns, or sends Tflush and waits for it.
Unwound(t)    == /\ ts[t] = "unwound"
                 /\ \E nx \in {"deliver", "flush"} : ts' = [ts EXCEPT ![t] = nx]
                 /\ UNCHANGED <<masked, inh, woken, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars
\* The Rflush came first (the call returns EINTR), or the original reply did
\* and the call carries on (a multi-RPC call's next RPC) holding its claim.
FlushDone(t)  == /\ ts[t] = "flush"
                 /\ \E nx \in {"deliver", "cont"} : ts' = [ts EXCEPT ![t] = nx]
                 /\ UNCHANGED <<masked, inh, woken, des, own, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars
\* The carried-on call's one further wait: its claim unwinds it at once while
\* the note is queued, and its word does while the note sits there; a claim
\* that outlived its note blocks in it.
ContWait(t)   == /\ ts[t] = "cont"
                 /\ IF DirectEnds(t)
                       THEN /\ ts' = [ts EXCEPT ![t] = "deliver"]
                            /\ woken' = [woken EXCEPT ![t] = FALSE]
                            /\ UNCHANGED <<des, own>>
                    ELSE IF MayUnwind(t)
                       THEN Unwind(t)
                       ELSE /\ ts' = [ts EXCEPT ![t] = "uwait"]
                            /\ woken' = [woken EXCEPT ![t] = FALSE]
                            /\ UNCHANGED <<des, own>>
                 /\ UNCHANGED <<masked, inh, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                 /\ UNCHANGED dvars

TakesDirect(t) == dq[t] /\ ~masked[t] /\ ~inh[t]

\* The tail's notes leg: at most one handler, and the thread's own note first.
Deliver(t)    == /\ ts[t] = "deliver"
                 /\ ~(DIRECT_FATAL /\ TakesDirect(t))
                 /\ IF TakesDirect(t)
                       THEN /\ dq' = [dq EXCEPT ![t] = FALSE]
                            /\ inh' = [inh EXCEPT ![t] = TRUE]
                            /\ UNCHANGED <<q, stolen>>
                    ELSE IF q > 0 /\ ~masked[t] /\ ~inh[t]
                       THEN /\ q' = q - 1
                            /\ inh' = [inh EXCEPT ![t] = TRUE]
                            /\ stolen' = (stolen \/ (dtgt # NONE /\ dtgt # t))
                            /\ UNCHANGED dq
                       ELSE UNCHANGED <<q, inh, dq, stolen>>
                 /\ ts' = [ts EXCEPT ![t] = "release"]
                 /\ UNCHANGED <<masked, woken, des, own, posted, walkOwed, wpick, nwait, wskip, wcur,
                                 dposted, dtgt, wasMasked>>

\* The default of a fatal direct note: exits_note, the group's one death. Its
\* claim on the group commits under the threads_lock, so never inside a walk.
DeliverFatal(t) == /\ ts[t] = "deliver" /\ DIRECT_FATAL /\ TakesDirect(t) /\ wpick = NONE
                   /\ ts' = [x \in Threads |-> "exited"]
                   /\ woken' = [x \in Threads |-> FALSE]
                   /\ des' = {}
                   /\ own' = {}
                   /\ UNCHANGED <<masked, inh, q, posted, walkOwed, wpick, nwait, wskip, wcur>>
                   /\ UNCHANGED dvars

\* The claim ends at the tail, delivered or not. The walk it owes skips it.
Release(t)    == /\ ts[t] = "release"
                 /\ des' = des \ {t}
                 /\ own' = own \ {t}
                 /\ LET owes == ~BUGGY_RELEASE_NO_WAKE /\ t \in Claimed /\ q > 0
                    IN /\ walkOwed' = (walkOwed \/ owes)
                       /\ wskip' = IF ~owes THEN wskip
                                   ELSE IF walkOwed THEN NONE ELSE t
                 /\ ts' = [ts EXCEPT ![t] = "user"]
                 /\ UNCHANGED <<masked, inh, woken, q, posted, wpick, nwait, wcur>>
                 /\ UNCHANGED dvars

Next == \/ Post \/ Walk1 \/ Walk2
        \/ \E t \in Threads :
              \/ PostDirect(t)
              \/ Syscall(t) \/ Sigreturn(t) \/ Irq(t) \/ PreStep(t)
              \/ Handoff(t) \/ Resume(t) \/ ResumeHold(t) \/ WaitDone(t)
              \/ Unwound(t) \/ FlushDone(t) \/ ContWait(t) \/ Deliver(t) \/ DeliverFatal(t)
              \/ Release(t)

Spec == Init /\ [][Next]_vars

\* Fair: the walks, the kernel's own steps, a woken thread's resume, the
\* flush wait, and the tick (Irq) for a thread at EL0. Not fair: a post, a
\* syscall, rt_sigreturn, a spurious resume, or any other wait ending.
Live == /\ Spec
        /\ WF_vars(Walk1) /\ WF_vars(Walk2)
        /\ \A t \in Threads :
              /\ WF_vars(Irq(t)) /\ WF_vars(PreStep(t)) /\ WF_vars(Handoff(t))
              /\ WF_vars(WokenResume(t)) /\ WF_vars(ResumeHold(t))
              /\ WF_vars(Unwound(t)) /\ WF_vars(FlushDone(t)) /\ WF_vars(ContWait(t))
              /\ WF_vars(Deliver(t)) /\ WF_vars(DeliverFatal(t)) /\ WF_vars(Release(t))

-----------------------------------------------------------------------------
OneClaimant == Cardinality(Claimed) <= 1

NoStolenDirect == ~stolen

\* For the liveness question a candidate is a thread at EL0 or blocked in an
\* unwinding wait, unmasked and outside a handler. A thread inside a syscall
\* body is left out: one that never leaves its call (a hold wait after a hold
\* wait, forever) takes no note on any design, and is no strand.
Ready(t) == ~masked[t] /\ ~inh[t] /\ ts[t] \in {"user", "uwait"}

NoStrand == <>[](q > 0) => <>[](\A t \in Threads : ~Ready(t))

\* The direct twin: the one thread that can take it, and for a fatal note a hold
\* wait counts, since it ends for one. A caught note rides a hold wait out.
DirectReady(t) == /\ ~masked[t] /\ ~inh[t]
                  /\ \/ ts[t] \in {"user", "uwait"}
                     \/ DIRECT_FATAL /\ ts[t] = "hwait"

NoDirectStrand == \A t \in Threads : <>[](dq[t]) => <>[](~DirectReady(t))

=============================================================================
