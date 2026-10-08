---- MODULE reader_frame ----
(***************************************************************************)
(* Thylacine: the elected 9P reader's recv unwinds at ANY byte (#90,        *)
(* ARCH 8.8.1.1, rewritten for the seam-90 close, 2026-10-06).              *)
(*                                                                         *)
(* The elected reader (the #841 mountio reader) drains a byte stream        *)
(* shared by every Proc that mounts through the client. An async event --   *)
(* its Proc dying, a stop, a caught note -- can reach it anywhere inside a  *)
(* frame, including while it waits for bytes the server has not sent. The   *)
(* reader unwinds at once and the reader role passes on. The frame's bytes  *)
(* are the CLIENT's, not the reader's: the reader reads into the client's   *)
(* buffer, resumes at the client's count (c->rx_got) and leaves that count  *)
(* where it stopped, so the next reader resumes the frame. Plan 9 devmnt    *)
(* keeps the partial message in m->q across an interrupted mntrpcread;      *)
(* Linux trans_fd keeps it in the connection (m->rc.offset).                *)
(*                                                                         *)
(* WHAT THIS MODELS. One frame of N chunks. The server sends chunks         *)
(* (`sent`) and may stop at any point for good: Send carries NO fairness in *)
(* Spec, the case a hostile server forces (any process can serve a mount    *)
(* over pipes). `pos` is how many chunks have been taken off the wire, `rx` *)
(* is the client's resume count. Two readers: A starts holding the role and *)
(* is the one the async event (`interrupted`) reaches; B waits on the same  *)
(* session and takes the role when it is free. Death, stop and caught note  *)
(* leave the recv the same way (they differ only in what A does after:      *)
(* die, park and re-elect, or flush), so one event stands for all three.    *)
(* A reads a chunk only if the server sent it; a frame is delivered when    *)
(* its last chunk is read with the client's count in step with the wire.   *)
(*                                                                         *)
(* PROPERTIES. NoDesync: no reader ever parses from a count that disagrees  *)
(* with the wire (a tail read as a header -- the task-#50 class).           *)
(* ResumePoint: until delivery, the client's count IS the wire position.    *)
(* EventuallyUnwinds (Spec, no server fairness): an interrupted reader      *)
(* leaves its recv even if the server never sends again -- the seam-90      *)
(* hang cannot happen. FrameDelivered (FairServerSpec, the server sends     *)
(* eventually): the frame reaches its reader although A left mid-frame --   *)
(* the survivor resumed it.                                                 *)
(*                                                                         *)
(* THE BUG CLASSES.                                                         *)
(*  BUGGY_DISCARD: an unwind resets the client's count (the pre-loom-mc     *)
(*   reader, whose count was local to its frame read). The survivor parses  *)
(*   from 0 while the wire is mid-frame -> NoDesync and ResumePoint fail    *)
(*   (reader_frame_buggy.cfg). This is the hazard the 2026-07-19 rule       *)
(*   (block-through) existed to avoid.                                      *)
(*  BUGGY_BLOCK_THROUGH: the superseded rule -- an interrupted reader       *)
(*   unwinds only at a frame boundary and otherwise waits for the rest.     *)
(*   Safe, but with a server that stops mid-frame the reader never leaves:  *)
(*   EventuallyUnwinds fails (reader_frame_blockthrough.cfg) -- the vault's *)
(*   seam-90-hung-server, as a counterexample.                              *)
(*                                                                         *)
(* Outside the model: the transport's own recv, taken to return either the  *)
(* bytes it copied or nothing (each recv sleeps only before it copies --    *)
(* srvconn_client_recv, the pipe read); tags and the dying op's flush       *)
(* (9p_client.tla, I-10); more than one frame (a frame boundary resets the  *)
(* count, and the next frame is this one again).                            *)
(* The srvconn reading role (ch->reading) is taken to be released on every  *)
(* recv exit (chan_role_release): one left held would strand the next       *)
(* reader in chan_role_acquire, a hang ElectB cannot show.                  *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    N,                    \* chunks per 9P frame (>= 2 so a mid-frame exists)
    BUGGY_DISCARD,        \* TRUE = an unwind discards the client's partial frame
    BUGGY_BLOCK_THROUGH   \* TRUE = the superseded rule: unwind only at a boundary

ASSUME N \in Nat /\ N >= 2
ASSUME BUGGY_DISCARD \in BOOLEAN /\ BUGGY_BLOCK_THROUGH \in BOOLEAN

VARIABLES
    sent,         \* chunks of the frame the server has put on the wire, 0..N
    pos,          \* chunks taken off the wire, 0..N
    rx,           \* the client's resume count (c->rx_got), 0..N-1
    role,         \* who holds the reader role: "A", "B" or "none"
    pcA,          \* A: "reading" (in its recv, holding the role) or "unwound"
    pcB,          \* B: "waiting", "reading" (holds the role) or "done"
    interrupted,  \* the async event has reached A (set once)
    delivered,    \* the frame was read whole, in step with the wire
    desynced      \* some reader parsed from a count the wire disagrees with

vars == <<sent, pos, rx, role, pcA, pcB, interrupted, delivered, desynced>>

TypeOk ==
    /\ sent \in 0..N
    /\ pos \in 0..N
    /\ rx \in 0..(N - 1)
    /\ role \in {"A", "B", "none"}
    /\ pcA \in {"reading", "unwound"}
    /\ pcB \in {"waiting", "reading", "done"}
    /\ interrupted \in BOOLEAN
    /\ delivered \in BOOLEAN
    /\ desynced \in BOOLEAN

Init ==
    /\ sent = 0
    /\ pos = 0
    /\ rx = 0
    /\ role = "A"
    /\ pcA = "reading"
    /\ pcB = "waiting"
    /\ interrupted = FALSE
    /\ delivered = FALSE
    /\ desynced = FALSE

(* The server puts the next chunk on the wire. No fairness in Spec: it may  *)
(* stop sending at any point, mid-frame included, for good.                 *)
Send ==
    /\ sent < N
    /\ sent' = sent + 1
    /\ UNCHANGED <<pos, rx, role, pcA, pcB, interrupted, delivered, desynced>>

(* The role holder X reads one chunk the server has sent. It parses from the *)
(* client's count rx; if that disagrees with the wire (pos), it reads a tail *)
(* as a header. The last chunk of a frame read in step delivers the frame    *)
(* (B's reply, whoever reads it) and the count returns to the boundary.      *)
Read(X) ==
    /\ role = X
    /\ IF X = "A" THEN pcA = "reading" ELSE pcB = "reading"
    /\ pos < sent
    /\ pos' = pos + 1
    /\ desynced' = (desynced \/ rx # pos)
    /\ IF rx + 1 = N
          THEN /\ rx' = 0
               /\ delivered' = TRUE
               /\ pcB' = IF pcB = "reading" \/ pcB = "waiting" THEN "done" ELSE pcB
               /\ role' = IF X = "B" THEN "none" ELSE role
          ELSE /\ rx' = rx + 1
               /\ UNCHANGED <<delivered, pcB, role>>
    /\ UNCHANGED <<sent, pcA, interrupted>>

(* The async event reaches A (death, stop or caught note), at any point. *)
Interrupt ==
    /\ ~interrupted
    /\ interrupted' = TRUE
    /\ UNCHANGED <<sent, pos, rx, role, pcA, pcB, delivered, desynced>>

(* A leaves its recv: the role is released (and handed on), and the client  *)
(* keeps the partial frame. The rule: at ANY byte, waiting for the server or *)
(* not. BUGGY_BLOCK_THROUGH allows it only at a boundary (rx = 0);           *)
(* BUGGY_DISCARD loses the client's count.                                   *)
UnwindA ==
    /\ pcA = "reading"
    /\ interrupted
    /\ (~BUGGY_BLOCK_THROUGH \/ rx = 0)
    /\ pcA' = "unwound"
    /\ role' = "none"
    /\ rx' = IF BUGGY_DISCARD THEN 0 ELSE rx
    /\ UNCHANGED <<sent, pos, pcB, interrupted, delivered, desynced>>

(* B takes the free role (the handoff's designee, or its own election). *)
ElectB ==
    /\ role = "none"
    /\ pcB = "waiting"
    /\ role' = "B"
    /\ pcB' = "reading"
    /\ UNCHANGED <<sent, pos, rx, pcA, interrupted, delivered, desynced>>

Next ==
    \/ Send
    \/ Read("A")
    \/ Read("B")
    \/ Interrupt
    \/ UnwindA
    \/ ElectB

(* The readers are fair: an enabled read, unwind or election happens. The   *)
(* server is not: Send has no fairness here. Interrupt is the adversary's.  *)
ReaderFairness ==
    /\ WF_vars(Read("A"))
    /\ WF_vars(Read("B"))
    /\ WF_vars(UnwindA)
    /\ WF_vars(ElectB)

Spec == Init /\ [][Next]_vars /\ ReaderFairness

(* The server eventually sends every chunk. *)
FairServerSpec == Spec /\ WF_vars(Send)

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(* No reader ever parses from a count the wire disagrees with. *)
NoDesync == ~desynced

(* Until the frame is delivered, the client's resume count is exactly the   *)
(* wire position: every reader exit left the partial frame for the next.    *)
ResumePoint == delivered \/ rx = pos

Safety ==
    /\ TypeOk
    /\ NoDesync
    /\ ResumePoint

(* An interrupted reader leaves its recv -- with NO fairness on the server.  *)
(* This is the seam-90 close: a server that stops cannot hold the reader.    *)
EventuallyUnwinds == interrupted ~> (pcA = "unwound")

(* Under a server that does send, the frame is delivered even when A leaves *)
(* mid-frame: the survivor resumes it from the client's count.              *)
FrameDelivered == <>delivered

====
