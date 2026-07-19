---- MODULE reader_frame ----
(***************************************************************************)
(* Thylacine frame-atomic reader-recv death-unwind (#90, ARCH 8.8.1.1).    *)
(* The #811 universal death-interruptible sleep (death_wake.tla) unwinds    *)
(* EVERY rendez sleeper immediately when its Proc's group_exit_msg is       *)
(* observed. That is correct for every sleeper EXCEPT one: the elected 9P   *)
(* reader (the #841 mountio reader), whose recv drains a byte STREAM shared *)
(* across every Proc that mounts through the client. If a dying reader      *)
(* unwinds MID-FRAME -- with some chunks of the current 9P frame already    *)
(* pulled from the transport ring -- those consumed bytes are discarded,    *)
(* and the SURVIVOR that takes over the reader role reads the abandoned     *)
(* frame's TAIL as a new header: the shared stream DESYNCS (shared-session  *)
(* death / the task-#50 corruption class). This is the exact hazard the     *)
(* shipped 8c-3 debug-STOP block-through already closes for the stop path   *)
(* (stop_no_park + stop_unwinds, kernel/9p_client.c::reader_recv_frame);    *)
(* the DEATH path -- gated by the thread_die_pending die-check BELOW the     *)
(* stop detour in sleep()/tsleep() -- was left with the immediate #811       *)
(* unwind (the pre-existing #841/#811 latent the 8c-3 close named #90).      *)
(*                                                                         *)
(* THE FIX (block-through, mirroring the shipped 8c-3 debug-STOP path).     *)
(* The die-check becomes FRAME-ATOMIC for the reader: it unwinds a dying    *)
(* reader ONLY at a frame boundary (no chunk of the current frame consumed) *)
(* and BLOCKS THROUGH mid-frame -- the reader keeps receiving until the     *)
(* frame completes (a boundary again), then unwinds. Block-through is       *)
(* bounded by the trusted server's whole-frame delivery (CF-3 B): the       *)
(* server sends complete frames, so a mid-frame reader reaches the next     *)
(* boundary in bounded time. (An UNTRUSTED / hung server that sends a       *)
(* partial frame then stalls is the v1.x liveness seam, OUTSIDE this model; *)
(* every v1.0 9P server is a trusted local Proc.) The impl reuses the       *)
(* existing stop_no_park + stop_unwinds latches (both already = (got==0)),  *)
(* so the die-check guard widens from unconditional to                      *)
(* !stop_no_park || stop_unwinds -- no new Thread field.                     *)
(*                                                                         *)
(* WHAT THIS MODELS. A reader receives ONE frame of N chunks, one chunk at  *)
(* a time (ReceiveChunk -- the trusted server's fair delivery). `got` is    *)
(* the chunk count consumed of the current frame, 0..N; got \in {0, N} is a *)
(* boundary (0 = frame not started / N = frame complete -> the stream       *)
(* read-offset sits at the next frame's header), got \in 1..N-1 is          *)
(* mid-frame. An async death (Die) publishes `dying` ONCE, at any point.    *)
(* The die-check (Unwind) fires per the frame-atomic guard. The single      *)
(* frame models "the stream idles after the current frame"; a busy stream   *)
(* simply defers the unwind to the next idle boundary -- still frame-atomic.*)
(*                                                                         *)
(* THE BUG CLASS -- BUGGY_UNWIND_MIDFRAME. TRUE reproduces the pre-#90      *)
(* immediate #811 unwind: the die-check fires at ANY got. A death at        *)
(* got \in 1..N-1 unwinds mid-frame -> the consumed chunks are discarded -> *)
(* `desynced` is set (the shared stream is corrupt for the survivor). The   *)
(* buggy cfg makes NoDesync (and UnwindAtBoundary) fail -- the executable   *)
(* counterexample the frame-atomic guard closes.                            *)
(*                                                                         *)
(* `desynced` abstracts the survivor's stream corruption: a mid-frame       *)
(* discard leaves the stream read-offset partway into a frame, so the next  *)
(* reader reads a tail-as-header. NoDesync == ~desynced is the shared-      *)
(* stream integrity invariant (I-9 NARROWED for the reader recv, ARCH       *)
(* 8.8.1.1). The fix never LOSES the death (the reader still unwinds --      *)
(* EventuallyUnwinds -- just at the next boundary), so I-9's                 *)
(* no-lost-death-wake for this sleeper is preserved; only its TIMING is      *)
(* deferred to a boundary.                                                   *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    N,                     \* chunks per 9P frame (>= 2 so a mid-frame exists)
    BUGGY_UNWIND_MIDFRAME  \* TRUE = the pre-#90 immediate unwind; FALSE = the fix

ASSUME N \in Nat /\ N >= 2

VARIABLES
    pc,        \* the reader: "reading" (in the recv loop) or "unwound" (died, terminal)
    got,       \* chunks of the current frame consumed, 0..N (0 or N = a boundary)
    dying,     \* group_exit_msg published for the reader's Proc (BOOLEAN, set once)
    desynced   \* the shared stream corrupted by a mid-frame discard (BOOLEAN, set once)

vars == <<pc, got, dying, desynced>>

AtBoundary(g) == g = 0 \/ g = N

TypeOk ==
    /\ pc \in {"reading", "unwound"}
    /\ got \in 0..N
    /\ dying \in BOOLEAN
    /\ desynced \in BOOLEAN

Init ==
    /\ pc = "reading"
    /\ got = 0
    /\ dying = FALSE
    /\ desynced = FALSE

(* The trusted server delivers the next chunk of the frame; the reader      *)
(* consumes it (got advances). Enabled while the reader is reading and the  *)
(* frame is not yet complete. FAIR (CF-3 B whole-frame delivery) -- this is *)
(* what makes block-through terminate (EventuallyUnwinds). Note: NOT gated  *)
(* on ~dying -- a dying reader BLOCKS THROUGH by continuing to receive.     *)
ReceiveChunk ==
    /\ pc = "reading"
    /\ got < N
    /\ got' = got + 1
    /\ UNCHANGED <<pc, dying, desynced>>

(* The async death: group termination publishes the reader's                *)
(* group_exit_msg ONCE. It can arrive at any reader state / any got. UNFAIR *)
(* (the adversary -- death may or may not come; the liveness below is        *)
(* conditional on it).                                                       *)
Die ==
    /\ ~dying
    /\ dying' = TRUE
    /\ UNCHANGED <<pc, got, desynced>>

(* The die-check (thread_die_pending in sleep()/tsleep(), below the stop     *)
(* detour). FIXED (#90): frame-atomic -- fire ONLY at a boundary            *)
(* (got \in {0, N}); mid-frame it is DISABLED, so the reader blocks through  *)
(* via ReceiveChunk. BUGGY: fire at ANY got (the immediate #811 unwind). A   *)
(* mid-frame unwind discards the consumed chunks -> sets `desynced`.         *)
(* (Impl: the guard is !stop_no_park || stop_unwinds; here stop_no_park is   *)
(* always set -- this IS the reader recv -- so the guard reduces to          *)
(* stop_unwinds == (got==0), generalized to the got==N end-boundary the      *)
(* real code reaches by resetting got to 0 for the next frame.)              *)
Unwind ==
    /\ pc = "reading"
    /\ dying
    /\ BUGGY_UNWIND_MIDFRAME \/ AtBoundary(got)
    /\ pc' = "unwound"
    /\ desynced' = (desynced \/ ~AtBoundary(got))
    /\ UNCHANGED <<got, dying>>

Next ==
    \/ ReceiveChunk
    \/ Die
    \/ Unwind

(* WF on ReceiveChunk = the trusted server eventually delivers each chunk    *)
(* (block-through cannot hang mid-frame). WF on Unwind = the die-check       *)
(* eventually fires once it is enabled at a boundary. Die is UNFAIR.         *)
Fairness ==
    /\ WF_vars(ReceiveChunk)
    /\ WF_vars(Unwind)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(* THE crux (I-9 narrowed for the reader recv, ARCH 8.8.1.1): the shared    *)
(* byte stream is never desynced -- a dying reader never discards a         *)
(* partially-consumed frame, so a survivor never reads a tail-as-header.    *)
(* Holds with the frame-atomic guard; the buggy cfg violates it.            *)
NoDesync == ~desynced

(* The sharper mechanism form: the reader reaches the "unwound" terminal    *)
(* ONLY at a frame boundary (got \in {0, N}). Equivalent to NoDesync (a     *)
(* mid-frame unwind is exactly what sets desynced) but stated on `got`.     *)
UnwindAtBoundary == (pc = "unwound") => AtBoundary(got)

Safety ==
    /\ TypeOk
    /\ NoDesync
    /\ UnwindAtBoundary

(* Liveness -- the FIX's obligation, not the bug's: block-through must not   *)
(* HANG a dying reader mid-frame. Once death arrives, the reader eventually  *)
(* unwinds (dies) -- the trusted server's fair chunk delivery drives a       *)
(* mid-frame reader to the next boundary, where the die-check fires. (The    *)
(* untrusted / hung-server partial-frame stall is the v1.x seam outside this *)
(* model.) The buggy cfg also satisfies this -- it is the SAFETY that        *)
(* distinguishes the fix; the liveness proves the fix introduces no hang.    *)
EventuallyUnwinds == dying ~> (pc = "unwound")

====
