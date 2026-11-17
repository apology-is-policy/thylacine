---- MODULE fs_cache ----
(***************************************************************************)
(* Thylacine LARDER -- the guest-side 9P FS cache, close-to-open coherence  *)
(* keyed on a true content-version (`cvers`).  LARDER-DESIGN.md; ARCH        *)
(* section 28 invariant I-38.                                                *)
(*                                                                         *)
(* The Larder lives on the per-session `p9_client` (shared by every         *)
(* Proc/thread resolving through the mount via the #841 elected reader). It *)
(* caches attrs, name-lookups, and data pages so a repeated stat/walk/read  *)
(* is served locally instead of re-hunted over 9P (measured: 56-90% of a    *)
(* go-build's FS ops are redundant). Coherence is close-to-open, the Plan 9 *)
(* `cfs` model: a cached entry is trusted while the file's content-version  *)
(* `cvers` (Stratum `si_cvers`, surfaced as `qid.version` -- L1a) is         *)
(* unchanged; the version is re-checked at open (a getattr / the POUNCE     *)
(* walk_attrs carries it for free); a mismatch drops + refetches. The       *)
(* guest's OWN writes invalidate write-through, so a Proc sees its own      *)
(* writes with strong consistency.                                          *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   Two coherence disciplines, each with its own bug counterexample:       *)
(*                                                                         *)
(*   (1) The OPEN-time REVALIDATION gate (`Open`). At the close-to-open      *)
(*       point the guest fetches the current `cvers` and compares it to the *)
(*       cached entry's; on a mismatch it MUST drop + refetch, never keep   *)
(*       the stale entry. This is the load-bearing gate that bounds an      *)
(*       out-of-band (external) writer's staleness to a single episode      *)
(*       (NoStalePastRevalidation). BUGGY_STALE_SERVE removes the `cvers`   *)
(*       comparison -- the serve-without-cvers-check bug the scripture names *)
(*       -- so a stale entry survives the open and is served (wrong read).  *)
(*                                                                         *)
(*   (2) The OWN-WRITE WRITE-THROUGH INVALIDATION (`OwnWrite`). A guest      *)
(*       write bumps the file's `cvers` and drops the cached entry, so a    *)
(*       subsequent read -- which does NOT re-fetch `cvers` within an open  *)
(*       episode -- cannot serve the pre-write value. In the single-writer  *)
(*       regime (EnableExternalWriter=FALSE: the guest is the only writer,  *)
(*       the serve-your-own-FS-to-one-guest case the scripture calls        *)
(*       single-writer-sound) this makes NoWrongRead ABSOLUTE: a served     *)
(*       value never lags the current content. BUGGY_NO_OWN_INVALIDATE      *)
(*       bumps `cvers` without dropping the entry, so a read serves the     *)
(*       guest's own stale bytes (NoWrongRead counterexample).              *)
(*                                                                         *)
(*   The load-bearing SMP property (LARDER-DESIGN.md section 7) -- an        *)
(*   invalidate racing a serve on the shared client -- is the interleaving  *)
(*   of the atomic `Read` (serve), `OwnWrite`/`Evict` (invalidate), and     *)
(*   `Open` (revalidate) actions: TLC explores every order. Safety holds    *)
(*   under all of them BECAUSE the open revalidates and the own-write       *)
(*   invalidates; drop either discipline (the two buggy flags) and a bad    *)
(*   interleaving reaches a wrong read.                                     *)
(*                                                                         *)
(* THE BUGS THIS PINS                                                       *)
(*                                                                         *)
(*   BUGGY_STALE_SERVE -- `Open` treats any valid entry as a hit and skips  *)
(*     the `cvers == current` comparison. After an external write bumps the *)
(*     file's `cvers` (the cached entry is now stale-but-valid), an open     *)
(*     that should drop + refetch instead keeps the entry and marks it      *)
(*     validated; a subsequent read serves the stale content-token          *)
(*     (NoStalePastRevalidation counterexample). The fix is the             *)
(*     compare-before-trust order: an open trusts a cached entry only when  *)
(*     its `cvers` equals the freshly-fetched one, else refetches.          *)
(*                                                                         *)
(*   BUGGY_NO_OWN_INVALIDATE -- `OwnWrite` bumps the server `cvers` but does *)
(*     NOT drop the cached entry. Because reads within an open episode do    *)
(*     not re-fetch `cvers`, a read after the write serves the pre-write    *)
(*     content-token: the guest fails to see its own write (NoWrongRead     *)
(*     counterexample, single-writer). The fix is write-through            *)
(*     invalidation: an own write drops the file's cached entry so the next *)
(*     read misses and refetches the post-write content.                    *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)    *)
(*                                                                         *)
(*   fs_cache.cfg                 single-writer, both buggy flags FALSE --   *)
(*                                 the canonical green run. NoWrongRead is   *)
(*                                 ABSOLUTE (a served value never lags the   *)
(*                                 current content); all safety invariants   *)
(*                                 hold.                                     *)
(*   fs_cache_external.cfg        EnableExternalWriter=TRUE, buggy flags     *)
(*                                 FALSE -- exercises the open revalidation  *)
(*                                 gate against an out-of-band writer.       *)
(*                                 NoWrongRead is NOT checked (an external   *)
(*                                 write within an open episode is served    *)
(*                                 stale until the next open -- the accepted *)
(*                                 close-to-open window, LARDER-DESIGN 4/11);*)
(*                                 NoStalePastRevalidation (the gate) holds. *)
(*   fs_cache_liveness.cfg        Spec_Live -- WriteEventuallyVisible: after *)
(*                                 a write the new content is eventually     *)
(*                                 served (the stale entry is dropped and    *)
(*                                 refetched).                               *)
(*   fs_cache_buggy_stale_serve.cfg                                          *)
(*                                 BUGGY_STALE_SERVE + external writer --    *)
(*                                 NoStalePastRevalidation counterexample:   *)
(*                                 an open keeps a stale entry validated.    *)
(*   fs_cache_buggy_no_invalidate.cfg                                        *)
(*                                 BUGGY_NO_OWN_INVALIDATE, single-writer -- *)
(*                                 NoWrongRead counterexample: a read serves *)
(*                                 the guest's own stale write.              *)
(*   fs_cache_wb.cfg              EnableStaging=TRUE, single-writer, buggy   *)
(*                                 flags FALSE -- the F1 WRITE-BEHIND leg    *)
(*                                 (LARDER-DESIGN 12, Senate-voted           *)
(*                                 2026-07-11): StageWrite advances the      *)
(*                                 guest's LOGICAL content ahead of the      *)
(*                                 server; a Read must serve the staged      *)
(*                                 overlay; FlushClose lands it. NoWrongRead *)
(*                                 + NoLostStage hold.                       *)
(*   fs_cache_buggy_skip_staged.cfg                                          *)
(*                                 BUGGY_READ_SKIPS_STAGED -- a read serves  *)
(*                                 the cache/server token while a stage is   *)
(*                                 present (the overlay-miss class):         *)
(*                                 NoWrongRead counterexample.               *)
(*   fs_cache_buggy_lost_stage.cfg                                           *)
(*                                 BUGGY_LOST_STAGE -- FlushClose clears the *)
(*                                 stage WITHOUT landing it at the server    *)
(*                                 (the lost-write class -- a dropped close  *)
(*                                 flush): NoLostStage counterexample.       *)
(*   fs_cache_buggy_peer_skips_staged.cfg                                    *)
(*                                 BUGGY_PEER_SKIPS_STAGED (vote M23) -- a   *)
(*                                 read through an open with no stage of its *)
(*                                 own serves the cache while ANOTHER open   *)
(*                                 holds a stage (audit round 4 of M15, F1): *)
(*                                 NoWrongRead counterexample.               *)
(*   fs_cache_buggy_peer_unordered.cfg                                       *)
(*                                 BUGGY_PEER_WRITE_UNORDERED (vote M23) --  *)
(*                                 a write-through by one open lands while   *)
(*                                 another open's OLDER stage is held: the   *)
(*                                 holder's own read serves the older token  *)
(*                                 (NoWrongRead counterexample); its flush   *)
(*                                 puts the older content back (NoLostStage, *)
(*                                 with NoWrongRead left out).               *)
(*   fs_cache_buggy_unpinned.cfg                                             *)
(*                                 BUGGY_NO_PIN (votes M24/M24a) -- a stage  *)
(*                                 starts while another open's write-through *)
(*                                 is in flight (its leg passed, its wire op *)
(*                                 not yet landed): the write then lands     *)
(*                                 over the newer stage (audit round 5 of    *)
(*                                 M15, F1). InvariantsWb counterexample.    *)
(*   fs_cache_wb_nopop.cfg        fs_cache_wb without the write-populate:    *)
(*                                 the green control one variable from the   *)
(*                                 peer cfgs, which run without it.          *)
(*   fs_cache_buggy_peer_unordered_lost.cfg                                  *)
(*                                 BUGGY_PEER_WRITE_UNORDERED with           *)
(*                                 NoWrongRead left out: the holder's flush  *)
(*                                 puts the older content back (NoLostStage  *)
(*                                 counterexample).                          *)
(*   fs_cache_buggy_skip_keeps_entry.cfg                                     *)
(*                                 BUGGY_SKIP_KEEPS_ENTRY + external writer  *)
(*                                 -- an open whose populate the gen guard   *)
(*                                 discards keeps the entry from before it   *)
(*                                 and trusts it (audit round 4 of M15, F4): *)
(*                                 NoStalePastRevalidation counterexample.   *)
(*                                                                         *)
(*   The staging cfgs run two opens (Opens = {o1, o2}), the rest one. Vote  *)
(*   M23 (LARDER-DESIGN 12.6): a file's held bytes are the CLIENT's, so a    *)
(*   read through another open must not miss them (the observing legs: an    *)
(*   open's or a stat's populate is judged by the Read that serves it) and a *)
(*   write through another open lands after them (the changing legs). Votes *)
(*   M24/M24a: a write-through is two steps, its leg (OwnWriteBegin) and its *)
(*   wire op with the invalidate (OwnWriteLand), and the leg pins the file   *)
(*   so no stage starts in between. The fidless cached open's own leg is     *)
(*   beneath the model (its per-open snapshot is not modelled); a runtime    *)
(*   test pins it. So is the own install's gen guard (13): FlushClose lands  *)
(*   and installs in one step, so no write lands in between here; in the     *)
(*   impl the holder's own disjoint append lands during its flush (12.2),    *)
(*   and the guard, pinned by a runtime test, covers what lands there.       *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                     *)
(*                                                                         *)
(*   Content as a version-token. Because `cvers` bumps on EVERY content     *)
(*   mutation (L1a inode.tla `ContentMutate`), two states with the same     *)
(*   `cvers` for a file have the same content. So the content a fresh read  *)
(*   of `f` returns is modeled as the token `svr_cvers[f]`, and a cache      *)
(*   entry stores the token it captured; a served token equals the current  *)
(*   content iff it equals `svr_cvers[f]`. This is the whole coherence       *)
(*   argument in one variable and needs no separate content model. A served *)
(*   token is judged correct AT THE MOMENT of the read (the `bad_read`       *)
(*   history flag): a read that was correct when it happened is not made     *)
(*   wrong by a later write.                                                 *)
(*                                                                         *)
(*   Populate sources carry a TRUE content-version. `Open` populates from a *)
(*   getattr / walk_attrs reply, whose `qid.version` is `si_cvers` (L1a-2). *)
(*   The Larder is NEVER populated from a `readdir` qid: Rreaddir's          *)
(*   qid.version is a link-time `si_gen` snapshot (the dirent record stores  *)
(*   no content-version), so a readdir-sourced version would read backwards *)
(*   against a getattr-sourced `si_cvers` for the same inode. v1.0 does not  *)
(*   cache directory listings (LARDER-DESIGN 11); the guest-side rule "never *)
(*   populate from a readdir qid" is the L1c-e realization of that -- and    *)
(*   the ground-truth-correct disposition of the L1a-2 audit F1 seam (a      *)
(*   readdir content-version needs a per-child revalidation mechanism, a     *)
(*   v1.x item, not a dirent-format snapshot). This spec models only the     *)
(*   content-version-bearing populate.                                      *)
(*                                                                         *)
(*   A populate may be discarded. The impl fetches, then installs under the *)
(*   gen guard, which skips a fill an invalidation event of the file may     *)
(*   have overtaken -- or any fill, once the window outruns the event ring   *)
(*   (larder.h G4). Why a skip happens is beneath this model; that one can   *)
(*   happen at any open, and what becomes of the entry then, is OpenSkipped. *)
(*                                                                         *)
(*   Open revalidates; reads within an episode do not. `Open` is the sole   *)
(*   `cvers`-check point (close-to-open); `Read` serves a valid entry        *)
(*   without re-checking -- exactly why the two disciplines (open-time       *)
(*   revalidation AND own-write invalidation) are BOTH required and are      *)
(*   modeled as distinct actions with distinct bugs. A metadata stat that    *)
(*   both revalidates and serves (the POUNCE fused walk_attrs) is `Open`     *)
(*   then `Read`; TLC explores the split (adversarial) interleaving that     *)
(*   subsumes the fused one.                                                 *)
(*                                                                         *)
(*   Which sub-cache uses which discipline (L1c/L1d as-built). The ATTR and *)
(*   PAGE sub-caches use BOTH disciplines: each entry is validated by its    *)
(*   OWN file's `cvers`, so `Open` (revalidate) + `OwnWrite` (invalidate)    *)
(*   both apply, and the external-writer case is bounded by the open gate.   *)
(*   The DENTRY sub-cache (the `(parent,name)->child` name-binding, positive *)
(*   OR negative) uses ONLY `Read` + `OwnWrite` -- NO `Open`/`cvers` gate.   *)
(*   A name-binding is a fact about the PARENT directory's dirent set, and   *)
(*   Stratum surfaces no directory-content version that tracks a dirent      *)
(*   change (ground truth `src/fs/fs.c`: a child create/unlink touches only  *)
(*   the separate dirent index, never `stm_inode_set` on the parent, so the  *)
(*   parent `si_cvers` does not bump -- only rename stamps it). So a dentry  *)
(*   cannot be cvers-revalidated; its sole coherence is own-write            *)
(*   invalidation (a guest create/rename/unlink drops the parent's cached    *)
(*   dentries). This is exactly the `fs_cache.cfg` single-writer regime      *)
(*   restricted to {Read, OwnWrite, Evict}: NoWrongRead is absolute (a       *)
(*   valid dentry is always current because own-writes invalidate) and the   *)
(*   external-writer bound is LRU eviction, not the open gate. No new action *)
(*   is needed -- the dentry cache is a sub-regime of this model.            *)
(*                                                                         *)
(*   Bounded cache. A fixed `Capacity` (< |Files| so eviction fires) with an *)
(*   inline victim eviction on a refetch models the I-32 LRU-capped Larder;  *)
(*   `Bounded` asserts the valid-entry count never exceeds it. Eviction      *)
(*   POLICY (LRU) is not a correctness property, so the victim is chosen     *)
(*   nondeterministically.                                                   *)
(*                                                                         *)
(*   Page-lifetime safety (a page freed by an evictor while a reader copies  *)
(*   it out, LARDER-DESIGN 7) is a lifetime obligation the impl discharges   *)
(*   by copying under the lock; it is below this spec's abstraction (the     *)
(*   serve is atomic here) and is prosecuted by the L1f audit.               *)
(*                                                                         *)
(* See LARDER-DESIGN.md; ARCHITECTURE.md section 28 I-38 (+ I-28/I-32/I-10/  *)
(* I-11 composition); the Stratum companion `inode.tla` (the `si_cvers`      *)
(* ContentMutate model); net_poll.tla / poll.tla (the shared-client         *)
(* register/observe + hook-lifetime siblings); kernel/9p_client.c (the       *)
(* Larder owner), kernel/dev9p.c (the serve/populate/invalidate hooks --     *)
(* L1c-e).                                                                   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Files,                  \* the set of file ids the model tracks.
    Capacity,               \* max simultaneously-cached (valid) entries;
                            \*   < Cardinality(Files) so eviction is exercised.
    MaxCvers,               \* TLC bound on the per-file content-version.
    EnableExternalWriter,   \* BOOLEAN -- TRUE: model an out-of-band (non-guest)
                            \*   writer that bumps a file's cvers WITHOUT
                            \*   invalidating the guest cache (the coherence
                            \*   hazard the open-time revalidation defends
                            \*   against). FALSE: single-writer (the guest is
                            \*   the only writer -- NoWrongRead is then absolute).
    BUGGY_STALE_SERVE,      \* BOOLEAN -- TRUE: Open treats any valid entry as a
                            \*   hit and skips the cvers==current comparison, so a
                            \*   stale entry survives revalidation and is served.
    BUGGY_NO_OWN_INVALIDATE, \* BOOLEAN -- TRUE: OwnWrite bumps cvers but does NOT
                            \*   drop the cached entry, so a read serves the
                            \*   guest's own stale (pre-write) content.
    EnableStaging,          \* BOOLEAN -- TRUE: the F1 write-behind actions
                            \*   (StageWrite / FlushClose) are enabled. FALSE
                            \*   keeps the pre-F1 write-through model exactly
                            \*   (guest_cvers == svr_cvers always), so the
                            \*   original cfgs are semantically unchanged.
    BUGGY_READ_SKIPS_STAGED,\* BOOLEAN -- TRUE: a Read with a stage present may
                            \*   serve the cached (server-lagging) token instead
                            \*   of the staged overlay -- the overlay-miss bug.
    BUGGY_LOST_STAGE,       \* BOOLEAN -- TRUE: FlushClose clears the stage
                            \*   WITHOUT landing the staged content at the
                            \*   server -- the lost-write bug (a dropped close
                            \*   flush: close/death path that neither flushes
                            \*   nor deliberately write-throughs).
    EnableFlushPopulate,    \* BOOLEAN -- TRUE (G1, term-4): FlushClose INSTALLS
                            \*   the landed content as the current cache token
                            \*   (the write-populate) instead of invalidating.
                            \*   The impl realization: own-flag pages installed
                            \*   from the frozen wb run after every chunk's
                            \*   Rwrite confirms; the serve accepts own pages
                            \*   without the cvers gate (sound ONLY under the
                            \*   staging single-writer premise below).
    BUGGY_POPULATE_UNFLUSHED, \* BOOLEAN -- TRUE: the populate fires even though
                            \*   the flush did NOT land (the failed/lost-flush
                            \*   pairing) -- the cache then claims content the
                            \*   server does not hold (CacheNeverAhead red).
                            \*   The impl rule it pins: install ONLY on the
                            \*   err==0 full-land arm of wb_flush_locked.
    Opens,                  \* the opens of a file on the client (one dev9p_priv
                            \*   each); a stage belongs to the open that made it.
    BUGGY_PEER_SKIPS_STAGED, \* BOOLEAN -- TRUE: a Read by an open with no stage
                            \*   of its own may serve the cache while ANOTHER
                            \*   open holds a stage: the observing legs of vote
                            \*   M23 missing (audit round 4 of M15, F1).
    BUGGY_PEER_WRITE_UNORDERED, \* BOOLEAN -- TRUE: a write-through by one open
                            \*   may land while another open's stage is held:
                            \*   the changing legs of vote M23 missing, so the
                            \*   older stage lands after the newer write.
    BUGGY_SKIP_KEEPS_ENTRY, \* BOOLEAN -- TRUE: an open whose populate the gen
                            \*   guard discards keeps the entry from before it,
                            \*   which a stat through the open then serves
                            \*   (audit round 4 of M15, F4).
    BUGGY_NO_PIN            \* BOOLEAN -- TRUE: a write-through's leg pins
                            \*   nothing, so another open may start staging
                            \*   while the write is in flight (votes M24/M24a;
                            \*   audit round 5 of M15, F1).

ASSUME /\ Capacity \in Nat
       /\ MaxCvers \in Nat /\ MaxCvers >= 1
       /\ EnableExternalWriter    \in BOOLEAN
       /\ BUGGY_STALE_SERVE       \in BOOLEAN
       /\ BUGGY_NO_OWN_INVALIDATE \in BOOLEAN
       /\ EnableStaging           \in BOOLEAN
       /\ BUGGY_READ_SKIPS_STAGED \in BOOLEAN
       /\ BUGGY_LOST_STAGE        \in BOOLEAN
       /\ EnableFlushPopulate     \in BOOLEAN
       /\ BUGGY_POPULATE_UNFLUSHED \in BOOLEAN
       \* The loose (B1) premise: staging asserts single-writer.
       /\ (EnableStaging => ~EnableExternalWriter)
       \* G1 is a flush-path feature; the buggy pairing needs a lost flush.
       /\ (EnableFlushPopulate => EnableStaging)
       /\ (BUGGY_POPULATE_UNFLUSHED => (EnableFlushPopulate /\ BUGGY_LOST_STAGE))
       /\ Opens # {}
       /\ BUGGY_PEER_SKIPS_STAGED    \in BOOLEAN
       /\ BUGGY_PEER_WRITE_UNORDERED \in BOOLEAN
       \* The peer legs exist only where something stages.
       /\ ((BUGGY_PEER_SKIPS_STAGED \/ BUGGY_PEER_WRITE_UNORDERED) => EnableStaging)
       /\ BUGGY_SKIP_KEEPS_ENTRY \in BOOLEAN
       /\ BUGGY_NO_PIN \in BOOLEAN
       /\ (BUGGY_NO_PIN => EnableStaging)

VARIABLES
    svr_cvers,   \* [Files -> 0..MaxCvers] -- the server (Stratum) content-
                 \*   version per file. Bumps on OwnWrite / ExternalWrite. The
                 \*   token svr_cvers[f] IS the content a fresh read of f returns.
    guest_cvers, \* [Files -> 0..MaxCvers] -- the file's LOGICAL content version
                 \*   (what a correct same-guest read must return). Write-through
                 \*   keeps it equal to svr_cvers; the F1 write-behind lets it run
                 \*   AHEAD while a stage is outstanding (guest > svr <=> staged,
                 \*   on the correct path). FlushClose lands svr := guest.
    stage,       \* [Files -> [Opens -> 0..MaxCvers]] -- the content token an
                 \*   open's unflushed staged run holds (dev9p_priv.wb), 0 for
                 \*   none (every staged token is >= 1). Set by StageWrite;
                 \*   cleared by FlushClose, which lands THIS token.
    cache,       \* [Files -> [valid: BOOLEAN, cvers: 0..MaxCvers]] -- the Larder
                 \*   entry per file. `cvers` is the content-token the entry will
                 \*   serve; meaningful only while `valid`.
    validated,   \* [Files -> BOOLEAN] -- f's cached entry passed the most recent
                 \*   open-time revalidation and no write has happened to f since
                 \*   (the close-to-open "known fresh" mark). Set by Open; cleared
                 \*   by any write to f and by Evict.
    bad_read,    \* BOOLEAN -- monotonic history flag: TRUE once any Read has
                 \*   served a token that did NOT equal the file's current content
                 \*   at the moment of the read. NoWrongRead == ~bad_read.
    wpend        \* [Files -> [Opens -> BOOLEAN]] -- a write-through by the open
                 \*   whose leg has run and whose wire op has not landed: the pin
                 \*   (LARDER-DESIGN 12.6, votes M24/M24a).

vars == <<svr_cvers, guest_cvers, stage, cache, validated, bad_read, wpend>>

ValidSet == {f \in Files : cache[f].valid}

\* The opens holding a stage of f, and whether any does.
Holders(f)   == {o \in Opens : stage[f][o] # 0}
AnyStaged(f) == Holders(f) # {}
\* No open other than o holds a stage of f.
PeersClear(f, o) == \A p \in Opens \ {o} : stage[f][p] = 0
\* No write-through of f is in flight through an open other than o: no pin of
\* another open stands. BUGGY_NO_PIN ignores the pins.
Unpinned(f, o) == BUGGY_NO_PIN \/ \A p \in Opens \ {o} : ~wpend[f][p]

TypeOk ==
    /\ svr_cvers \in [Files -> 0..MaxCvers]
    /\ guest_cvers \in [Files -> 0..MaxCvers]
    /\ stage \in [Files -> [Opens -> 0..MaxCvers]]
    /\ cache \in [Files -> [valid : BOOLEAN, cvers : 0..MaxCvers]]
    /\ validated \in [Files -> BOOLEAN]
    /\ bad_read \in BOOLEAN
    /\ wpend \in [Files -> [Opens -> BOOLEAN]]

(***************************************************************************)
(* Cold start: the server holds every file at content-version 0, the       *)
(* Larder is empty, nothing validated, no bad read yet.                     *)
(***************************************************************************)
Init ==
    /\ svr_cvers   = [f \in Files |-> 0]
    /\ guest_cvers = [f \in Files |-> 0]
    /\ stage       = [f \in Files |-> [o \in Opens |-> 0]]
    /\ cache     = [f \in Files |-> [valid |-> FALSE, cvers |-> 0]]
    /\ validated = [f \in Files |-> FALSE]
    /\ bad_read  = FALSE
    /\ wpend     = [f \in Files |-> [o \in Opens |-> FALSE]]

(***************************************************************************)
(* Open(f) -- the close-to-open REVALIDATION + populate point. The guest    *)
(* fetches f's current cvers (the getattr / POUNCE walk_attrs reply). The    *)
(* CORRECT gate trusts the cached entry only when it is valid AND its cvers  *)
(* equals the freshly-fetched one; otherwise it refetches (installs the      *)
(* fresh content, evicting a victim if the cache is full). Either way the    *)
(* entry ends fresh and is marked validated. Open does NOT serve (that is    *)
(* Read) -- it is the metadata revalidation that a subsequent read trusts.   *)
(*                                                                         *)
(* BUGGY_STALE_SERVE drops the `cache[f].cvers = fresh` conjunct: any valid  *)
(* entry counts as a hit, so a stale entry is kept and marked validated      *)
(* without refetching -- the serve-without-cvers-check bug.                  *)
(***************************************************************************)
Refetch(f, fresh) ==
    \* install f valid@fresh, respecting Capacity (evict a victim if full and
    \* f is not already occupying a slot).
    \/ /\ cache[f].valid \/ Cardinality(ValidSet) < Capacity
       /\ cache'     = [cache EXCEPT ![f] = [valid |-> TRUE, cvers |-> fresh]]
       /\ validated' = [validated EXCEPT ![f] = TRUE]
    \/ /\ ~cache[f].valid /\ Cardinality(ValidSet) >= Capacity
       /\ \E v \in ValidSet \ {f} :
             /\ cache'     = [cache EXCEPT ![v].valid = FALSE,
                                           ![f] = [valid |-> TRUE, cvers |-> fresh]]
             /\ validated' = [validated EXCEPT ![v] = FALSE, ![f] = TRUE]

Open(f) ==
    /\ LET fresh  == svr_cvers[f]
           is_hit == cache[f].valid /\ (BUGGY_STALE_SERVE \/ cache[f].cvers = fresh)
       IN \/ /\ is_hit                        \* validated hit: keep the entry
             /\ cache'     = cache
             /\ validated' = [validated EXCEPT ![f] = TRUE]
          \/ /\ ~is_hit                        \* miss or stale: drop + refetch
             /\ Refetch(f, fresh)
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, bad_read, wpend>>

(***************************************************************************)
(* OpenSkipped(f) -- an Open whose populate the gen guard discards (larder.c  *)
(* larder_attr_install): the fetched value may be older than an invalidation  *)
(* event this client saw, so it is not installed. The CORRECT path drops f's  *)
(* entry with it -- the entry may be older than this open, and nothing may be *)
(* trusted past an open's revalidation. BUGGY_SKIP_KEEPS_ENTRY keeps it and   *)
(* the open trusts it (the attr serve asks no version): a stale entry passes  *)
(* for revalidated. Enabled at any time, as the ring can overflow on another  *)
(* file's events alone.                                                       *)
(***************************************************************************)
OpenSkipped(f) ==
    /\ IF BUGGY_SKIP_KEEPS_ENTRY
         THEN /\ cache'     = cache
              /\ validated' = [validated EXCEPT ![f] = cache[f].valid]
         ELSE /\ cache'     = [cache EXCEPT ![f].valid = FALSE]
              /\ validated' = [validated EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, bad_read, wpend>>

(***************************************************************************)
(* Read(f) -- serve f from a valid cached entry, WITHOUT re-fetching cvers   *)
(* (the whole point of the cache: a read within an open episode is local).   *)
(* The served content-token is the entry's cvers. `bad_read` records whether *)
(* the served token matched the file's CURRENT content at the moment of the  *)
(* read: in the single-writer regime a valid entry is always fresh           *)
(* (own-writes invalidate), so the served token equals the current content   *)
(* and bad_read never sets -- NoWrongRead. With an external writer the entry  *)
(* may be stale-within-the-episode (served until the next Open revalidates -- *)
(* the accepted close-to-open window), which does set bad_read -- hence       *)
(* NoWrongRead is checked only in the single-writer cfgs.                     *)
(***************************************************************************)
(* The correctness judge is guest_cvers -- the LOGICAL content at the moment  *)
(* of the read. Without staging guest_cvers == svr_cvers (write-through), so   *)
(* the original judge is unchanged. With a stage present the CORRECT read      *)
(* serves the staged overlay (the newest guest content while the changing     *)
(* legs keep it so); BUGGY_READ_SKIPS_STAGED lets the cache arm fire anyway,   *)
(* serving a server-lagging token -- the overlay-miss bug. A read by open o    *)
(* with no stage of its own serves the cache only when no OTHER open holds a   *)
(* stage either: vote M23's observing legs flush the holder first (LARDER-     *)
(* DESIGN 12.6). BUGGY_PEER_SKIPS_STAGED drops that condition -- audit round 4 *)
(* of M15, F1.                                                                *)
Read(f, o) ==
    \/ \* the staged-overlay arm: the holder's own read within its run.
       /\ stage[f][o] # 0
       /\ bad_read' = (bad_read \/ stage[f][o] # guest_cvers[f])
       /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, wpend>>
    \/ \* the cache arm. CORRECT only when no open holds a stage of f (the
       \* impl's overlay-wins split, and the observing legs' flush).
       /\ (stage[f][o] = 0 \/ BUGGY_READ_SKIPS_STAGED)
       /\ (PeersClear(f, o) \/ BUGGY_PEER_SKIPS_STAGED)
       /\ cache[f].valid
       /\ bad_read' = (bad_read \/ cache[f].cvers # guest_cvers[f])
       /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, wpend>>

(***************************************************************************)
(* OwnWrite(f) -- the guest writes f through, in two steps: OwnWriteBegin is  *)
(* the leg, OwnWriteLand the wire op and the invalidate. Land bumps the       *)
(* content-version and, in the CORRECT path, invalidates write-through (drops *)
(* the cached entry) so the next read misses + refetches the post-write       *)
(* content. Clears `validated`.                                              *)
(*                                                                         *)
(* BUGGY_NO_OWN_INVALIDATE bumps the version but leaves the entry valid at    *)
(* the OLD cvers, so a subsequent Read serves the pre-write content -- the    *)
(* guest fails to see its own write.                                         *)
(*                                                                         *)
(* The leg (vote M23's changing legs) lands another open's run first and     *)
(* stops its holder staging; BUGGY_PEER_WRITE_UNORDERED skips it. The leg     *)
(* also pins the file until the land (votes M24/M24a): StageWrite waits out   *)
(* every other open's pin. A write-through by the holder itself pins too, in  *)
(* the model and, since vote M26, in the impl: its wire op runs outside its   *)
(* wb_lock, and its entry can leave the registry while the op is in flight.   *)
(***************************************************************************)
OwnWriteBegin(f, o) ==
    /\ ~wpend[f][o]      \* one write-through of f per open at a time
    /\ stage[f][o] = 0   \* the impl flushes its own run before a write-through
    /\ (PeersClear(f, o) \/ BUGGY_PEER_WRITE_UNORDERED)
    /\ wpend' = [wpend EXCEPT ![f][o] = TRUE]
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, bad_read>>

OwnWriteLand(f, o) ==
    /\ wpend[f][o]
    /\ guest_cvers[f] < MaxCvers
    /\ svr_cvers'   = [svr_cvers   EXCEPT ![f] = guest_cvers[f] + 1]
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ validated'  = [validated EXCEPT ![f] = FALSE]
    /\ IF BUGGY_NO_OWN_INVALIDATE
         THEN cache' = cache
         ELSE cache' = [cache EXCEPT ![f].valid = FALSE]
    /\ wpend' = [wpend EXCEPT ![f][o] = FALSE]
    /\ UNCHANGED <<stage, bad_read>>

(***************************************************************************)
(* ExternalWrite(f) -- an out-of-band (non-guest) Stratum mutation bumps f's *)
(* content-version. The guest cache is NOT told (a 9P server pushes no        *)
(* invalidations), so a valid entry becomes stale-but-valid. `validated` is   *)
(* cleared: the entry is no longer known-fresh, and only the next Open's       *)
(* revalidation will catch the staleness. This is the hazard the open gate    *)
(* defends against; disabled in the single-writer regime.                     *)
(***************************************************************************)
ExternalWrite(f) ==
    /\ EnableExternalWriter
    /\ guest_cvers[f] < MaxCvers
    /\ svr_cvers'   = [svr_cvers   EXCEPT ![f] = guest_cvers[f] + 1]
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ validated'  = [validated EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<stage, cache, bad_read, wpend>>

(***************************************************************************)
(* Evict(f) -- LRU/capacity eviction of a valid entry (the bounded page/     *)
(* attr/dentry pool). Drops the entry; clears `validated`. Always available   *)
(* on a valid entry -- the nondeterministic eviction that keeps the Larder    *)
(* bounded.                                                                   *)
(***************************************************************************)
Evict(f) ==
    /\ cache[f].valid
    /\ cache'     = [cache EXCEPT ![f].valid = FALSE]
    /\ validated' = [validated EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, bad_read, wpend>>

(***************************************************************************)
(* StageWrite(f) -- the F1 write-behind: a pure-append write lands in the    *)
(* per-open-file staging run. The guest's LOGICAL content advances; the       *)
(* server does NOT (no wire op). The shared-cache invalidate moves to the     *)
(* flush (per-write -> per-flush), so the cache is untouched here; the        *)
(* staged-overlay arm of Read keeps same-priv reads correct in the interim.   *)
(* One holder (LARDER-DESIGN 12.6): an open stages only while no other open    *)
(* holds a stage of the file -- in the impl a second open is never eligible,   *)
(* and its writes are changing legs that land the holder's run first. And no   *)
(* stage starts while another open's write-through is in flight (the pin,      *)
(* votes M24/M24a): in the impl the changing leg dropped the holder's anchor   *)
(* and an open that would become the holder is refused while the pin stands.  *)
(***************************************************************************)
StageWrite(f, o) ==
    /\ EnableStaging
    /\ guest_cvers[f] < MaxCvers
    /\ ~wpend[f][o]
    /\ PeersClear(f, o)
    /\ Unpinned(f, o)
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ stage'       = [stage EXCEPT ![f][o] = guest_cvers[f] + 1]
    /\ UNCHANGED <<svr_cvers, cache, validated, bad_read, wpend>>

(***************************************************************************)
(* FlushClose(f) -- the flush (close / fsync / cap / non-append pre-flush):   *)
(* the staged run lands at the server as msize-max Twrites; the shared attr   *)
(* + page entries invalidate (the per-flush invalidate); `validated` clears.  *)
(* This is the close-to-open anchor: after it, any open (any Proc) reads the  *)
(* flushed content.                                                           *)
(*                                                                           *)
(* BUGGY_LOST_STAGE clears the stage WITHOUT landing it (svr unchanged): the  *)
(* lost-write bug -- a close/death path that neither flushes nor deliberately *)
(* write-throughs. NoLostStage catches it structurally (staged=FALSE yet      *)
(* guest > svr); a subsequent Open+Read also serves the pre-write content     *)
(* (bad_read fires via the cache arm).                                        *)
(***************************************************************************)
FlushClose(f, o) ==
    /\ stage[f][o] # 0
    \* The run lands the token it holds -- the newest content only while the
    \* changing legs order every other write after it.
    /\ IF BUGGY_LOST_STAGE
         THEN svr_cvers' = svr_cvers
         ELSE svr_cvers' = [svr_cvers EXCEPT ![f] = stage[f][o]]
    /\ stage'     = [stage EXCEPT ![f][o] = 0]
    /\ UNCHANGED <<guest_cvers, bad_read, wpend>>
    \* G1 (term-4, the write-populate): on the landed arm, install the flushed
    \* content as the CURRENT token -- semantically "as if an Open refetched
    \* immediately after the flush", so it reuses Refetch verbatim (capacity-
    \* honest: evicts a victim when full). The correct pairing installs ONLY
    \* when the flush landed; BUGGY_POPULATE_UNFLUSHED installs on the lost
    \* arm too, making the cache claim content the server does not hold.
    /\ IF EnableFlushPopulate /\ (~BUGGY_LOST_STAGE \/ BUGGY_POPULATE_UNFLUSHED)
       THEN Refetch(f, stage[f][o])
       ELSE /\ cache'     = [cache EXCEPT ![f].valid = FALSE]
            /\ validated' = [validated EXCEPT ![f] = FALSE]

Next ==
    \E f \in Files :
        \/ Open(f)
        \/ OpenSkipped(f)
        \/ ExternalWrite(f)
        \/ Evict(f)
        \/ \E o \in Opens :
              \/ Read(f, o)
              \/ OwnWriteBegin(f, o)
              \/ OwnWriteLand(f, o)
              \/ StageWrite(f, o)
              \/ FlushClose(f, o)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* CacheNeverAhead -- a cached content-version never exceeds the server's: the
\* Larder can lag (stale-behind) but can never fabricate a future version. A
\* modeling-integrity check (populate/refetch always take svr_cvers[f]).
CacheNeverAhead == \A f \in Files : cache[f].valid => cache[f].cvers <= svr_cvers[f]

\* ServerNeverAhead (staging integrity) -- the server never runs ahead of the
\* guest's logical content: svr <= guest always (write-through keeps them
\* equal; a stage advances guest; a flush lands svr := guest).
ServerNeverAhead == \A f \in Files : svr_cvers[f] <= guest_cvers[f]

\* NoLostStage (F1, the lost-write class) -- whenever no stage is outstanding,
\* the server holds the guest's full logical content. Every path OUT of the
\* staged state must land the bytes (flush) -- a close/death path that clears
\* the stage without flushing (BUGGY_LOST_STAGE) violates this immediately.
NoLostStage == \A f \in Files : ~AnyStaged(f) => guest_cvers[f] = svr_cvers[f]

\* OneHolder (vote M23) -- at most one open holds a stage of a file at a time.
OneHolder == \A f \in Files : Cardinality(Holders(f)) <= 1

\* Bounded (I-32 / I-38) -- the Larder never holds more than Capacity valid
\* entries. Non-vacuous: Capacity < |Files|, so a refetch must evict.
Bounded == Cardinality(ValidSet) <= Capacity

\* NoStalePastRevalidation (I-38, the close-to-open gate) -- an entry the open
\* gate marked `validated` (passed revalidation, no write since) is fresh: its
\* cvers equals the server's. This is what bounds an external writer's staleness
\* to a single episode -- the next Open catches the mismatch and refetches.
\* BUGGY_STALE_SERVE violates it: an open marks a stale entry validated.
NoStalePastRevalidation ==
    \A f \in Files : validated[f] => (cache[f].valid /\ cache[f].cvers = svr_cvers[f])

\* NoWrongRead (I-38, single-writer) -- no read ever served content that lagged
\* the file's current version at the moment of the read. ABSOLUTE only in the
\* single-writer regime (EnableExternalWriter=FALSE): own-writes invalidate, so a
\* valid entry is always fresh and Read serves the current content.
\* BUGGY_NO_OWN_INVALIDATE violates it: a read serves the guest's own stale
\* write. (Not checked with an external writer, where the accepted within-episode
\* window lets a served value legitimately lag until the next Open --
\* LARDER-DESIGN 4/11.)
NoWrongRead == ~bad_read

\* SafetyCore -- the invariants that hold in EVERY regime (single- or
\* multi-writer): type-correctness, boundedness, cache integrity, and the
\* revalidation gate. NoWrongRead is added on top in the single-writer cfgs.
SafetyCore ==
    /\ TypeOk
    /\ CacheNeverAhead
    /\ ServerNeverAhead
    /\ Bounded
    /\ NoStalePastRevalidation

Invariants ==
    /\ SafetyCore
    /\ NoWrongRead

\* The staging cfgs check the full set incl. the lost-write class.
InvariantsWb ==
    /\ SafetyCore
    /\ NoWrongRead
    /\ NoLostStage
    /\ OneHolder

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* WriteEventuallyVisible -- after any write leaves f's cache stale (a valid  *)
(* entry whose cvers lags the server), the staleness does not persist         *)
(* forever: eventually f's entry is fresh-or-empty (the open gate refetches,  *)
(* or eviction drops it). This is "a fresh write is eventually visible" -- the *)
(* new content reaches a reader through the revalidation. Under weak fairness  *)
(* on Open and Evict. Non-vacuous: the external writer makes Stale(f)          *)
(* reachable; the bounded MaxCvers makes writes finite, so the final stale     *)
(* entry is resolved and stays resolved.                                       *)
(***************************************************************************)
Stale(f) == cache[f].valid /\ cache[f].cvers # svr_cvers[f]

WriteEventuallyVisible ==
    \A f \in Files : Stale(f) ~> (~cache[f].valid \/ cache[f].cvers = svr_cvers[f])

Liveness ==
    /\ \A f \in Files : WF_vars(Open(f))
    /\ \A f \in Files : WF_vars(Evict(f))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
