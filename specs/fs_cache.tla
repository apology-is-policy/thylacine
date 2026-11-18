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
(*   fs_cache_reuse.cfg            EnableReuse with staging and MaxGen 2,    *)
(*                                 every buggy flag FALSE (votes M31, M33,   *)
(*                                 M34): a deleted file's number goes to the *)
(*                                 next file created, every open is named by *)
(*                                 number and generation where its fid is    *)
(*                                 made, a refused run is recorded under its *)
(*                                 holder's name, and a deleted file's open  *)
(*                                 reads through the Larder.                 *)
(*                                 InvariantsReuse: InvariantsWb + NamesTrue *)
(*                                 + RecordsTrue + DeadStateSkipped +        *)
(*                                 NoMisdirectedLoss + NoMisdirectedSeen +   *)
(*                                 CacheOfLiveFile.                          *)
(*   fs_cache_reuse_told.cfg       the same, checking NeverToldReport too:   *)
(*                                 VIOLATED by design, the witness that an   *)
(*                                 fsync's report is told its own file's     *)
(*                                 loss while InvariantsReuse holds          *)
(*                                 (NoMisdirectedLoss is not held by a       *)
(*                                 report that never fires).                 *)
(*   fs_cache_reuse_told_leg.cfg   the same with NeverToldLeg: VIOLATED by   *)
(*                                 design, the witness that an fsync's leg   *)
(*                                 meets its own file's refused run and      *)
(*                                 answers with it.                          *)
(*   fs_cache_reuse_coexist.cfg    the same, checking NeverBothHold too:     *)
(*                                 VIOLATED by design, the witness that the  *)
(*                                 new file's open holds while the dead      *)
(*                                 holder's run is still held                *)
(*                                 (DeadStateSkipped is not held by a dead   *)
(*                                 holder that never stands beside a live    *)
(*                                 one).                                     *)
(*   fs_cache_reuse_served.cfg     the same, checking NeverServedReused too: *)
(*                                 VIOLATED by design, the witness that an   *)
(*                                 open of a reused number is served from    *)
(*                                 the Larder (NoWrongRead is not held by a  *)
(*                                 name check that serves nothing).          *)
(*   fs_cache_buggy_number_key.cfg BUGGY_NUMBER_KEY -- names compared by     *)
(*                                 number alone (the key before vote M31):   *)
(*                                 the dead holder refuses the new file's    *)
(*                                 open (DeadStateSkipped counterexample).   *)
(*   fs_cache_buggy_number_key_told.cfg                                      *)
(*                                 the same with DeadStateSkipped left out,  *)
(*                                 checking NoMisdirectedReport: an fsync's  *)
(*                                 report is told another incarnation's loss *)
(*                                 (counterexample; TLC's shortest: a        *)
(*                                 deleted file's open told the new file's   *)
(*                                 refused run).                             *)
(*   fs_cache_buggy_number_key_told_leg.cfg                                  *)
(*                                 the same checking NoMisdirectedLeg: an    *)
(*                                 fsync's leg meets another incarnation's   *)
(*                                 holder and answers with its refusal       *)
(*                                 (counterexample).                         *)
(*   fs_cache_buggy_unnamed_walk.cfg                                         *)
(*                                 BUGGY_UNNAMED_WALK -- a plain walk names  *)
(*                                 its open by number alone and that name    *)
(*                                 matches every generation (before vote     *)
(*                                 M33): DeadStateSkipped counterexample.    *)
(*   fs_cache_buggy_unnamed_walk_told.cfg                                    *)
(*                                 the same with DeadStateSkipped and        *)
(*                                 NoWrongRead left out (a dead open named   *)
(*                                 by number alone is served the new file's  *)
(*                                 entry, a counterexample as short):        *)
(*                                 NoMisdirectedLoss counterexample.         *)
(*   fs_cache_buggy_number_serve.cfg                                         *)
(*                                 BUGGY_NUMBER_SERVE -- the Larder serves   *)
(*                                 an entry to any open of its number        *)
(*                                 (before vote M34): a deleted file's open  *)
(*                                 is served the next file's (NoWrongRead    *)
(*                                 counterexample).                          *)
(*   fs_cache_reuse_unnamed.cfg                                              *)
(*                                 fs_cache_reuse.cfg on one file with       *)
(*                                 EnableUnnamed (audit round 10 of M15,     *)
(*                                 S-17 and F2; votes M33-M35a): an open's   *)
(*                                 Tgetattr may fail, leaving it with no     *)
(*                                 name: it is served nothing, registers     *)
(*                                 nothing, samples the current sequence,    *)
(*                                 takes no named holder's refusal on its    *)
(*                                 leg, reports by number and marks no       *)
(*                                 record told. InvariantsReuse.             *)
(*   fs_cache_reuse_unnamed_told.cfg                                         *)
(*                                 the same, checking NeverToldUnnamed too:  *)
(*                                 VIOLATED by design, the witness that an   *)
(*                                 open with no name is told its own file's  *)
(*                                 refused run through the record.           *)
(*   fs_cache_reuse_unnamed_residue.cfg                                      *)
(*                                 the same with NeverMisToldUnnamed:        *)
(*                                 VIOLATED by design, the cost vote M35     *)
(*                                 accepted: an open of the new file with no *)
(*                                 name is told the deleted file's later     *)
(*                                 loss by number.                           *)
(*   fs_cache_buggy_unnamed_serve.cfg                                        *)
(*                                 BUGGY_UNNAMED_SERVE -- an open with no    *)
(*                                 name is served by number (M34's unnamed   *)
(*                                 clause gone): NoWrongRead counterexample. *)
(*   fs_cache_buggy_unnamed_leg_told.cfg                                     *)
(*                                 BUGGY_UNNAMED_LEG_TOLD -- an open with no *)
(*                                 name answers with a named holder's        *)
(*                                 refusal from its leg (before vote M35):   *)
(*                                 NoMisdirectedLoss counterexample.         *)
(*   fs_cache_buggy_unnamed_marks_seen.cfg                                   *)
(*                                 BUGGY_UNNAMED_MARKS_SEEN -- an open with  *)
(*                                 no name marks the record its fsync        *)
(*                                 reports as told (before round 10 F2):     *)
(*                                 NoMisdirectedSeen counterexample.         *)
(*   fs_cache_reuse_external.cfg                                             *)
(*                                 fs_cache_reuse.cfg on one file with       *)
(*                                 EnableExternalReuse (round 10 F5):        *)
(*                                 another client's or session's create      *)
(*                                 reuses the number and the Larder keeps    *)
(*                                 the deleted file's entry. InvariantsReuse *)
(*                                 but CacheOfLiveFile, which the external   *)
(*                                 reuse breaks by design.                   *)
(*   fs_cache_buggy_serve_older.cfg                                          *)
(*                                 BUGGY_SERVE_OLDER -- the serve admits an  *)
(*                                 older generation's entry: the new file's  *)
(*                                 open is served the deleted file's         *)
(*                                 (NoWrongRead counterexample). Without an  *)
(*                                 external reuse no such entry stands, and  *)
(*                                 the flag passes.                          *)
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
(*   REUSE (votes M31 and M33-M35; audit rounds 8 to 10 of M15). Stratum's   *)
(*   qid.path is the inode number alone: it frees the number at the last     *)
(*   unlink, while opens of the file stand, and gives it to the next file    *)
(*   created (Reuse). A model file is a NUMBER (Files) with its INCARNATIONS *)
(*   (gen). Every open is bound to the incarnation its fid was made on (inc) *)
(*   and is named where its fid is made (Mint, LARDER-DESIGN 12.6), by the   *)
(*   generation, which the model's server always reports. Every holder, pin  *)
(*   and lost-run record is compared by number and generation (Same:         *)
(*   wb_same). State of another generation is a deleted file's: a leg skips  *)
(*   it and it refuses no registration (DeadStateSkipped). A refused run is  *)
(*   recorded under its holder's own name (FlushRefused: a dead holder's     *)
(*   always, as its fid can no longer land it, and a live holder's when the  *)
(*   server refuses it), and an fsync is told only a loss under a name Same  *)
(*   as its own, whether through the record (Fsync) or as its own leg's      *)
(*   refusal (FsyncLeg): NoMisdirectedLoss. A dead open's read reaches the   *)
(*   Larder by number, as dev9p_read and stat_native look it up (Read).      *)
(*   Every entry carries the incarnation whose content it holds and the name *)
(*   its serve compares (cache.einc, cache.ename), and is served only to an  *)
(*   open named by that name or by the number alone (Serves, vote M34).      *)
(*   Serving an open another incarnation's entry is a wrong read             *)
(*   (NoWrongRead): a deleted file's open is never served the next file's    *)
(*   bytes or attributes. NamesTrue and RecordsTrue hold by construction     *)
(*   here, as Mint names an open by gen[f] and a refusal records inc[f][o].  *)
(*   The obligation they stand for is the impl's: that a plain walk's        *)
(*   Tgetattr asks about the walk's own new fid                              *)
(*   (dev9p.m33_plain_walk_names_the_file), and that Stratum answers a fid   *)
(*   whose number was reused with ESTALE rather than the new file's          *)
(*   generation (h_getattr). A dead open's writes, but for its run's flush   *)
(*   and a write of it in flight at the reuse, act on a file the number no   *)
(*   longer names and are beneath the model. An open with no name (its       *)
(*   Tgetattr failed: EnableUnnamed, audit round 10 of M15, S-17) is served  *)
(*   nothing (Serves), registers and stages nothing, samples the current     *)
(*   sequence, takes no refusal from a holder named by a generation on its   *)
(*   leg (FsyncLeg, vote M35), reports by number (the "unnamed" arm, the     *)
(*   cost M35 accepted: NoMisdirectedLoss admits it and NeverMisToldUnnamed  *)
(*   shows it reachable), marks no record told (NoMisdirectedSeen, round 10  *)
(*   F2), and its pin goes by number (Unpinned under Same, vote M35a). Its   *)
(*   cfgs run one file: every rule for it is within one number, and two      *)
(*   files multiply the state space past a practical run. The same rules are *)
(*   pinned at runtime (dev9p.m33_unnamed_open_writes_through,               *)
(*   m33_the_name_does_not_move,                                             *)
(*   m34_unnamed_file_misses_m34a_unnamed_dir_by_number,                     *)
(*   m35_unnamed_leg_takes_no_named_refusal,                                 *)
(*   m35a_unnamed_change_stops_the_holder,                                   *)
(*   m35_unnamed_report_marks_no_record_told). An external reuse             *)
(*   (ExternalReuse, round 10 F5) leaves the deleted file's entry in the     *)
(*   Larder, where the guest's own reuse drops it: CacheOfLiveFile is        *)
(*   definitional for the latter, and fs_cache_reuse_external.cfg checks     *)
(*   NoWrongRead without it. Outside the model, pinned by runtime tests      *)
(*   instead: a strict session's open, which asks no Tgetattr, is unnamed    *)
(*   too but samples by number (wb_open_sample); an unnamed leg visits every *)
(*   holder at its number (registration is not modelled apart from staging,  *)
(*   and PeersClear already asks every Same holder clear:                    *)
(*   dev9p.s15_unnamed_leg_visits_every_holder,                              *)
(*   s15_leg_visits_past_its_batch, s15_leg_resumes_after_a_closer); an      *)
(*   unnamed directory handle is served by number (vote M34a); a server that *)
(*   reports no generation keeps number-only names (vote M31's allowance --  *)
(*   m33_plain_walk_names_the_file); the record table's capacity (an evicted *)
(*   record tells less, never another file's loss); and a loss racing an     *)
(*   open's sample (told to it, as if the open came first).                  *)
(*                                                                           *)
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
(*   A reuse is the guest's own. Staging asserts a single writer, so the     *)
(*   unlink and the create come through another open of the same client: the *)
(*   create's invalidate of the child (dev9p_create) drops the number's      *)
(*   cache entry, and the new file's content is a fresh token. The interval  *)
(*   in which the number names no file is not observable here: no open can   *)
(*   be made to it, and an open made before the unlink keeps its name. A     *)
(*   dead open stages nothing in the model: a run it staged after the unlink *)
(*   is, to every comparison, the run of a holder that was dead at the       *)
(*   reuse.                                                                  *)
(*                                                                           *)
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
    BUGGY_NO_PIN,           \* BOOLEAN -- TRUE: a write-through's leg pins
                            \*   nothing, so another open may start staging
                            \*   while the write is in flight (votes M24/M24a;
                            \*   audit round 5 of M15, F1).
    EnableReuse,            \* BOOLEAN -- TRUE: a deleted file's number goes to
                            \*   the next file created (Reuse); opens are made
                            \*   and named (Mint); a refused run is recorded
                            \*   under its holder's name (FlushRefused) and
                            \*   fsync reports records (Fsync) and its own
                            \*   leg's refusal (FsyncLeg). Votes M31 and M33.
    MaxGen,                 \* TLC bound on a number's incarnations.
    BUGGY_NUMBER_KEY,       \* BOOLEAN -- TRUE: names are compared by number
                            \*   alone (the key before vote M31).
    BUGGY_UNNAMED_WALK,     \* BOOLEAN -- TRUE: a plain walk may name its open
                            \*   by number alone, a name that matches every
                            \*   generation (before vote M33).
    BUGGY_NUMBER_SERVE,     \* BOOLEAN -- TRUE: the Larder serves an entry to
                            \*   any open of its number, whatever the open's
                            \*   name (before vote M34).
    EnableUnnamed,          \* BOOLEAN -- TRUE: an open's Tgetattr may fail, so
                            \*   the open has no name (DEV9P_WB_FAILED): it
                            \*   registers nothing, is served nothing, and its
                            \*   legs, pin and report go by number (votes M33,
                            \*   M34, M35, M35a).
    BUGGY_UNNAMED_SERVE,    \* BOOLEAN -- TRUE: the Larder serves an open with
                            \*   no name by number (M34's unnamed clause gone).
    BUGGY_UNNAMED_LEG_TOLD, \* BOOLEAN -- TRUE: an open with no name takes the
                            \*   refusal of a holder named by a generation on
                            \*   its fsync's leg (before vote M35).
    BUGGY_UNNAMED_MARKS_SEEN, \* BOOLEAN -- TRUE: an open with no name marks the
                            \*   record its fsync reports as told (before audit
                            \*   round 10 of M15, F2).
    EnableExternalReuse,    \* BOOLEAN -- TRUE: another client or session reuses
                            \*   a number, and this guest's Larder keeps its
                            \*   entry (audit round 10 of M15, F5).
    BUGGY_SERVE_OLDER       \* BOOLEAN -- TRUE: the Larder serves an open named
                            \*   by a generation an older one's entry too.

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
       /\ EnableReuse        \in BOOLEAN
       /\ MaxGen             \in Nat
       /\ BUGGY_NUMBER_KEY   \in BOOLEAN
       /\ BUGGY_UNNAMED_WALK \in BOOLEAN
       /\ BUGGY_NUMBER_SERVE \in BOOLEAN
       /\ EnableUnnamed          \in BOOLEAN
       /\ BUGGY_UNNAMED_SERVE    \in BOOLEAN
       /\ BUGGY_UNNAMED_LEG_TOLD \in BOOLEAN
       /\ BUGGY_UNNAMED_MARKS_SEEN \in BOOLEAN
       /\ EnableExternalReuse    \in BOOLEAN
       /\ BUGGY_SERVE_OLDER      \in BOOLEAN
       \* Holders, pins and records are kept only where something stages.
       /\ (EnableReuse => EnableStaging)
       /\ ((BUGGY_NUMBER_KEY \/ BUGGY_UNNAMED_WALK \/ BUGGY_NUMBER_SERVE) => EnableReuse)
       /\ (EnableUnnamed => EnableReuse)
       /\ ((BUGGY_UNNAMED_SERVE \/ BUGGY_UNNAMED_LEG_TOLD \/ BUGGY_UNNAMED_MARKS_SEEN)
           => EnableUnnamed)
       /\ ((EnableExternalReuse \/ BUGGY_SERVE_OLDER) => EnableReuse)

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
    cache,       \* [Files -> [valid: BOOLEAN, cvers: 0..MaxCvers, einc: 0..MaxGen,
                 \*   ename: Names]] -- the Larder entry per file. `cvers` is the
                 \*   content-token the entry will serve, `einc` the incarnation
                 \*   whose content it is, `ename` the name its serve compares
                 \*   (vote M34: an own page's installing writer's, an
                 \*   attribute's generation); meaningful only while `valid`.
    validated,   \* [Files -> BOOLEAN] -- f's cached entry passed the most recent
                 \*   open-time revalidation and no write has happened to f since
                 \*   (the close-to-open "known fresh" mark). Set by Open; cleared
                 \*   by any write to f and by Evict.
    bad_read,    \* BOOLEAN -- monotonic history flag: TRUE once any Read has
                 \*   served a token that did NOT equal the file's current content
                 \*   at the moment of the read. NoWrongRead == ~bad_read.
    wpend,       \* [Files -> [Opens -> BOOLEAN]] -- a write-through by the open
                 \*   whose leg has run and whose wire op has not landed: the pin
                 \*   (LARDER-DESIGN 12.6, votes M24/M24a).
    gen,         \* [Files -> 0..MaxGen] -- the generation of the file the number
                 \*   names now (Stratum's si_gen); Reuse bumps it.
    inc,         \* [Files -> [Opens -> 0..MaxGen]] -- the incarnation the open's
                 \*   fid was made on: the truth its name is judged against.
    name,        \* [Files -> [Opens -> OpenNames]] -- the name the open was given
                 \*   where its fid was made (dev9p_priv.wb_name, wb_gen), fixed for
                 \*   its life (round 8 F2): a generation, NoGen, or Unnamed.
    errseq,      \* [Files -> [Opens -> Nat]] -- the open's sample (wb_errseq).
    rec,         \* [Files -> [Names -> record]] -- the client's lost-run record per
                 \*   name (p9_wb_losses): its sequence (0: none), whether an fsync
                 \*   was told of it, and the incarnation whose run it was.
    lseq,        \* Nat -- the client's record sequence (wb_loss.seq).
    bad_tell,    \* SUBSET Arms -- history: the arms through which an fsync was
                 \*   told a loss of a file other than its own (its report, its
                 \*   leg, the report of an open with no name).
                 \*   NoMisdirectedLoss admits only the last (vote M35).
    tells,       \* SUBSET Arms -- history: the arms through which an fsync was
                 \*   told any loss (the witnesses' non-vacuity).
    bad_seen     \* BOOLEAN -- history: an fsync marked a record of another
                 \*   incarnation told (NoMisdirectedSeen == ~bad_seen).

rvars == <<gen, inc, name, errseq, rec, lseq, bad_tell, tells, bad_seen>>
vars  == <<svr_cvers, guest_cvers, stage, cache, validated, bad_read, wpend,
           gen, inc, name, errseq, rec, lseq, bad_tell, tells, bad_seen>>

\* How an fsync is told a loss: its report of the record, its own leg's refusal,
\* or the report of an open with no name, which goes by number (vote M35).
Arms == {"report", "leg", "unnamed"}

\* A name with no generation: the number alone (wb_same's !has_gen).
NoGen == MaxGen + 1
Names == 0..NoGen
\* An open with no name (DEV9P_WB_FAILED: its Tgetattr failed). Never a record's
\* or an entry's name: it registers nothing, so it holds and installs nothing.
Unnamed   == MaxGen + 2
OpenNames == Names \cup {Unnamed}

\* One file (wb_same, vote M31): the same number -- names are compared only
\* within one number here -- and the same generation where both names carry
\* one; an open with no name carries none (wb_key_of). BUGGY_NUMBER_KEY compares
\* the number alone.
Same(a, b) == BUGGY_NUMBER_KEY \/ a \in {NoGen, Unnamed} \/ b \in {NoGen, Unnamed} \/ a = b

\* The open is of the file its number names now. Without Reuse every open is.
Live(f, o) == inc[f][o] = gen[f]

\* The Larder serves an entry to an open (vote M34; larder.c own_serves,
\* attr_serves): one named by the number alone takes any entry, one named by a
\* generation only an entry under that generation. A page read from the server is
\* checked by version instead, and Stratum starts a reused number's version past
\* the old file's, so its test is this one. An open with no name is served
\* nothing (wb_who_of's UNNAMED): it asks the server, which answers a deleted
\* file ESTALE. BUGGY_NUMBER_SERVE serves by number; BUGGY_UNNAMED_SERVE serves an
\* open with no name by number. BUGGY_SERVE_OLDER serves an open named by a
\* generation an older generation's entry too, which only an external reuse
\* leaves in place (audit round 10 of M15, F5).
Serves(n, e) == \/ BUGGY_NUMBER_SERVE
                \/ n = Unnamed /\ BUGGY_UNNAMED_SERVE
                \/ n # Unnamed /\ (n = NoGen \/ n = e \/ (BUGGY_SERVE_OLDER /\ n > e))

\* The records Same as name n, the newest of them, and an open's sample under n
\* (wb_loss_find_locked, wb_loss_sample): the newest's sequence once some open
\* was told of it, the one before while none was, so an open made after a loss
\* nobody heard is told too; 0 with no record. An open with no name samples the
\* current sequence (wb_open_sample's FAILED arm): it is told only later losses.
Matching(f, n) == {k \in Names : rec[f][k].seq # 0 /\ Same(n, k)}
Newest(f, n)   == CHOOSE k \in Matching(f, n) :
                      \A j \in Matching(f, n) : rec[f][j].seq <= rec[f][k].seq
Sample(f, n)   == IF n = Unnamed THEN lseq
                  ELSE IF Matching(f, n) = {} THEN 0
                  ELSE IF rec[f][Newest(f, n)].seen THEN rec[f][Newest(f, n)].seq
                       ELSE rec[f][Newest(f, n)].seq - 1

ValidSet == {f \in Files : cache[f].valid}

\* The opens holding a stage of incarnation g of f; the holders of the file the
\* number names now, and whether it has one.
HoldersOf(f, g) == {o \in Opens : stage[f][o] # 0 /\ inc[f][o] = g}
Holders(f)   == HoldersOf(f, gen[f])
AnyStaged(f) == Holders(f) # {}
\* No open other than o holds a stage of o's file: under a name Same as o's. A
\* holder of another generation is a deleted file's, and the legs skip it.
PeersClear(f, o) ==
    \A p \in Opens \ {o} : Same(name[f][o], name[f][p]) => stage[f][p] = 0
\* No write-through of o's file is in flight through an open other than o: no
\* pin of another open stands under a Same name. BUGGY_NO_PIN ignores the pins.
Unpinned(f, o) ==
    BUGGY_NO_PIN \/ \A p \in Opens \ {o} : Same(name[f][o], name[f][p]) => ~wpend[f][p]

TypeOk ==
    /\ svr_cvers \in [Files -> 0..MaxCvers]
    /\ guest_cvers \in [Files -> 0..MaxCvers]
    /\ stage \in [Files -> [Opens -> 0..MaxCvers]]
    /\ cache \in [Files -> [valid : BOOLEAN, cvers : 0..MaxCvers, einc : 0..MaxGen,
                            ename : Names]]
    /\ validated \in [Files -> BOOLEAN]
    /\ bad_read \in BOOLEAN
    /\ wpend \in [Files -> [Opens -> BOOLEAN]]
    /\ gen \in [Files -> 0..MaxGen]
    /\ inc \in [Files -> [Opens -> 0..MaxGen]]
    /\ name \in [Files -> [Opens -> OpenNames]]
    /\ errseq \in [Files -> [Opens -> Nat]]
    /\ rec \in [Files -> [Names -> [seq : Nat, seen : BOOLEAN, inc : 0..MaxGen]]]
    /\ lseq \in Nat
    /\ bad_tell \in SUBSET Arms
    /\ tells \in SUBSET Arms
    /\ bad_seen \in BOOLEAN

(***************************************************************************)
(* Cold start: the server holds every file at content-version 0, the       *)
(* Larder is empty, nothing validated, no bad read yet.                     *)
(***************************************************************************)
Init ==
    /\ svr_cvers   = [f \in Files |-> 0]
    /\ guest_cvers = [f \in Files |-> 0]
    /\ stage       = [f \in Files |-> [o \in Opens |-> 0]]
    /\ cache     = [f \in Files |-> [valid |-> FALSE, cvers |-> 0, einc |-> 0, ename |-> 0]]
    /\ validated = [f \in Files |-> FALSE]
    /\ bad_read  = FALSE
    /\ wpend     = [f \in Files |-> [o \in Opens |-> FALSE]]
    /\ gen       = [f \in Files |-> 0]
    /\ inc       = [f \in Files |-> [o \in Opens |-> 0]]
    /\ name      = [f \in Files |-> [o \in Opens |-> 0]]
    /\ errseq    = [f \in Files |-> [o \in Opens |-> 0]]
    /\ rec       = [f \in Files |-> [k \in Names |-> [seq |-> 0, seen |-> FALSE, inc |-> 0]]]
    /\ lseq      = 0
    /\ bad_tell  = {}
    /\ tells     = {}
    /\ bad_seen  = FALSE

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
Refetch(f, fresh, ei, en) ==
    \* install f valid@fresh, incarnation ei's under name en, respecting Capacity
    \* (evict a victim if full and f is not already occupying a slot).
    \/ /\ cache[f].valid \/ Cardinality(ValidSet) < Capacity
       /\ cache'     = [cache EXCEPT ![f] = [valid |-> TRUE, cvers |-> fresh,
                                              einc |-> ei, ename |-> en]]
       /\ validated' = [validated EXCEPT ![f] = TRUE]
    \/ /\ ~cache[f].valid /\ Cardinality(ValidSet) >= Capacity
       /\ \E v \in ValidSet \ {f} :
             /\ cache'     = [cache EXCEPT ![v].valid = FALSE,
                                           ![f] = [valid |-> TRUE, cvers |-> fresh,
                                                   einc |-> ei, ename |-> en]]
             /\ validated' = [validated EXCEPT ![v] = FALSE, ![f] = TRUE]

Open(f) ==
    /\ LET fresh  == svr_cvers[f]
           is_hit == cache[f].valid /\ (BUGGY_STALE_SERVE \/ cache[f].cvers = fresh)
       IN \/ /\ is_hit                        \* validated hit: keep the entry
             /\ cache'     = cache
             /\ validated' = [validated EXCEPT ![f] = TRUE]
          \/ /\ ~is_hit                        \* miss or stale: drop + refetch
             \* the reply's attributes carry the generation (walk_attrs, Tgetattr)
             /\ Refetch(f, fresh, gen[f], gen[f])
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, bad_read, wpend>>
    /\ UNCHANGED rvars

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
    /\ UNCHANGED rvars

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
(* A dead open's read reaches the cache arm too (audit round 9 of M15, F1):  *)
(* dev9p_read and stat_native look the Larder up by number. The serve admits *)
(* an entry only under the open's name (Serves, vote M34). An open served    *)
(* another incarnation's entry has read another file, whatever the token:    *)
(* its own fid would have answered ESTALE. BUGGY_NUMBER_SERVE serves by      *)
(* number alone. The staged-overlay arm is the open's own run, its own       *)
(* file's bytes: it cannot cross files, and judging a dead holder's read of  *)
(* it would need the deleted file's content, which is not modelled.          *)
\* The staged-overlay arm: the holder's own read within its run.
ReadStaged(f, o) ==
    /\ stage[f][o] # 0
    /\ Live(f, o)
    /\ bad_read' = (bad_read \/ stage[f][o] # guest_cvers[f])
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, wpend>>
    /\ UNCHANGED rvars

\* The cache arm. CORRECT only when no open holds a stage of f under o's name (the
\* impl's overlay-wins split, and the observing legs' flush).
ReadCache(f, o) ==
    /\ (stage[f][o] = 0 \/ BUGGY_READ_SKIPS_STAGED)
    /\ (PeersClear(f, o) \/ BUGGY_PEER_SKIPS_STAGED)
    /\ cache[f].valid
    /\ Serves(name[f][o], cache[f].ename)
    /\ bad_read' = (bad_read \/ cache[f].einc # inc[f][o]
                             \/ (Live(f, o) /\ cache[f].cvers # guest_cvers[f]))
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, wpend>>
    /\ UNCHANGED rvars

Read(f, o) == ReadStaged(f, o) \/ ReadCache(f, o)

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
    /\ Live(f, o)        \* a dead open's write reaches a file no longer named
    /\ stage[f][o] = 0   \* the impl flushes its own run before a write-through
    /\ (PeersClear(f, o) \/ BUGGY_PEER_WRITE_UNORDERED)
    /\ wpend' = [wpend EXCEPT ![f][o] = TRUE]
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, bad_read>>
    /\ UNCHANGED rvars

OwnWriteLand(f, o) ==
    /\ wpend[f][o]
    /\ Live(f, o)        \* one in flight at the reuse ends in OwnWriteDead
    /\ guest_cvers[f] < MaxCvers
    /\ svr_cvers'   = [svr_cvers   EXCEPT ![f] = guest_cvers[f] + 1]
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ validated'  = [validated EXCEPT ![f] = FALSE]
    /\ IF BUGGY_NO_OWN_INVALIDATE
         THEN cache' = cache
         ELSE cache' = [cache EXCEPT ![f].valid = FALSE]
    /\ wpend' = [wpend EXCEPT ![f][o] = FALSE]
    /\ UNCHANGED <<stage, bad_read>>
    /\ UNCHANGED rvars

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
    /\ UNCHANGED rvars

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
    /\ UNCHANGED rvars

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
    /\ Live(f, o)
    /\ name[f][o] # Unnamed  \* an open with no name registers nothing (wb_named)
    /\ PeersClear(f, o)
    /\ Unpinned(f, o)
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ stage'       = [stage EXCEPT ![f][o] = guest_cvers[f] + 1]
    /\ UNCHANGED <<svr_cvers, cache, validated, bad_read, wpend>>
    /\ UNCHANGED rvars

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
(* (bad_read fires via the cache arm). A refused run is FlushRefused.        *)
(***************************************************************************)
FlushClose(f, o) ==
    /\ stage[f][o] # 0
    /\ Live(f, o)        \* a dead holder's run is always refused
    \* The run lands the token it holds -- the newest content only while the
    \* changing legs order every other write after it.
    /\ IF BUGGY_LOST_STAGE
         THEN svr_cvers' = svr_cvers
         ELSE svr_cvers' = [svr_cvers EXCEPT ![f] = stage[f][o]]
    /\ stage'     = [stage EXCEPT ![f][o] = 0]
    /\ UNCHANGED <<guest_cvers, bad_read, wpend>>
    /\ UNCHANGED rvars
    \* G1 (term-4, the write-populate): on the landed arm, install the flushed
    \* content as the CURRENT token -- semantically "as if an Open refetched
    \* immediately after the flush", so it reuses Refetch verbatim (capacity-
    \* honest: evicts a victim when full). The correct pairing installs ONLY
    \* when the flush landed; BUGGY_POPULATE_UNFLUSHED installs on the lost
    \* arm too, making the cache claim content the server does not hold.
    \* The own pages carry the holder's registered name (vote M34).
    /\ IF EnableFlushPopulate /\ (~BUGGY_LOST_STAGE \/ BUGGY_POPULATE_UNFLUSHED)
       THEN Refetch(f, stage[f][o], inc[f][o], name[f][o])
       ELSE /\ cache'     = [cache EXCEPT ![f].valid = FALSE]
            /\ validated' = [validated EXCEPT ![f] = FALSE]

(***************************************************************************)
(* Reuse(f) -- Stratum frees number f at the file's last unlink, while its   *)
(* opens stand, and gives it to the next file created: the number names a    *)
(* new incarnation (gen bumps). The create is the guest's own (staging's     *)
(* single-writer premise), so its invalidate of the child (dev9p_create)     *)
(* drops the number's cache entry, and the new file's content is a fresh     *)
(* token. Nothing lands: a holder of the old file keeps its run, now a dead  *)
(* holder's; a write of it in flight stays in flight; every open of the      *)
(* old file keeps its name.                                                  *)
(***************************************************************************)
Reuse(f) ==
    /\ EnableReuse
    /\ gen[f] < MaxGen
    /\ guest_cvers[f] < MaxCvers
    /\ gen'         = [gen EXCEPT ![f] = gen[f] + 1]
    /\ svr_cvers'   = [svr_cvers   EXCEPT ![f] = guest_cvers[f] + 1]
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ cache'       = [cache EXCEPT ![f].valid = FALSE]
    /\ validated'   = [validated EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<stage, bad_read, wpend, inc, name, errseq, rec, lseq, bad_tell, tells,
                   bad_seen>>

(***************************************************************************)
(* ExternalReuse(f) -- another client, or another session of this guest    *)
(* (each session has its own Larder), deletes the file and creates the     *)
(* next: the number names a new incarnation, as in Reuse, but no create of *)
(* this session's drops the entry (a 9P server pushes no invalidations),   *)
(* so the Larder keeps the deleted file's. `validated` clears, as for      *)
(* ExternalWrite. A new open of the number is then judged against an older *)
(* incarnation's entry: the serve's other direction (audit round 10 of     *)
(* M15, F5). The other client's create is modelled; its writes are not.    *)
(***************************************************************************)
ExternalReuse(f) ==
    /\ EnableExternalReuse
    /\ gen[f] < MaxGen
    /\ guest_cvers[f] < MaxCvers
    /\ gen'         = [gen EXCEPT ![f] = gen[f] + 1]
    /\ svr_cvers'   = [svr_cvers   EXCEPT ![f] = guest_cvers[f] + 1]
    /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
    /\ validated'   = [validated EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<stage, cache, bad_read, wpend, inc, name, errseq, rec, lseq,
                   bad_tell, tells, bad_seen>>

(***************************************************************************)
(* Mint(f, o) -- an open of the file number f names now is made in slot o,   *)
(* the slot's earlier open closed (its run gone, no write of it in flight).  *)
(* Its name is fixed where its fid is made (vote M33, LARDER-DESIGN 12.6):   *)
(* the walk asking attributes learns the generation from its Twalkgetattr,   *)
(* a plain walk asks it with one Tgetattr on the new fid, a clone that       *)
(* walks no name copies its source's, a create asks it, a cached open        *)
(* takes the Larder's. It samples the records under that name                *)
(* (wb_open_sample). BUGGY_UNNAMED_WALK lets a plain walk name it by number  *)
(* alone (before vote M33). EnableUnnamed lets the Tgetattr fail: the open   *)
(* then has no name (FAILED) and samples the current sequence.               *)
(***************************************************************************)
MintNames(f) == (IF BUGGY_UNNAMED_WALK THEN {gen[f], NoGen} ELSE {gen[f]})
                \cup (IF EnableUnnamed THEN {Unnamed} ELSE {})

Mint(f, o) ==
    /\ EnableReuse
    /\ stage[f][o] = 0
    /\ ~wpend[f][o]
    /\ \E n \in MintNames(f) :
          /\ inc'    = [inc EXCEPT ![f][o] = gen[f]]
          /\ name'   = [name EXCEPT ![f][o] = n]
          /\ errseq' = [errseq EXCEPT ![f][o] = Sample(f, n)]
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, bad_read, wpend,
                   gen, rec, lseq, bad_tell, tells, bad_seen>>

(***************************************************************************)
(* FlushRefused(f, o) -- a holder's run is flushed (its close, its fsync,    *)
(* the closer, or another open's leg) and refused: a dead holder's always,   *)
(* as its fid answers ESTALE, and a live holder's when the server refuses a  *)
(* chunk of it (the refused arm of FlushClose, audit round 9 of M15, F7). It *)
(* is recorded under the holder's own name (wb_flush_locked's refused arm,   *)
(* wb_loss_record; the closer's job records a run it gives up the same way), *)
(* and the number's cache entry drops, as on every failed flush; the new     *)
(* file's goes with it, which is sound: a drop serves nothing. A dead        *)
(* holder's run lands nothing on the file the number names now. A live       *)
(* holder's may land in part, so the file then holds content no token yet    *)
(* names (a fresh one) and the rest of the run is lost: an fsync of the file *)
(* is told of it through the record.                                         *)
(***************************************************************************)
\* p's run refused, its record made told or not.
Refuse(f, p, told) ==
    /\ stage[f][p] # 0
    /\ IF Live(f, p)
         THEN /\ guest_cvers[f] < MaxCvers
              /\ svr_cvers'   = [svr_cvers   EXCEPT ![f] = guest_cvers[f] + 1]
              /\ guest_cvers' = [guest_cvers EXCEPT ![f] = guest_cvers[f] + 1]
         ELSE UNCHANGED <<svr_cvers, guest_cvers>>
    /\ stage'     = [stage EXCEPT ![f][p] = 0]
    /\ cache'     = [cache EXCEPT ![f].valid = FALSE]
    /\ validated' = [validated EXCEPT ![f] = FALSE]
    /\ rec'       = [rec EXCEPT ![f][name[f][p]] =
                        [seq |-> lseq + 1, seen |-> told, inc |-> inc[f][p]]]
    /\ lseq'      = lseq + 1

FlushRefused(f, o) ==
    /\ Refuse(f, o, FALSE)
    /\ UNCHANGED <<bad_read, wpend, gen, inc, name, errseq, bad_tell, tells, bad_seen>>

(***************************************************************************)
(* FsyncLeg(f, o, p) -- an fsync through open o, whose leg lands the run of  *)
(* holder p first (wb_holder_flush at dev9p_fsync), meets p's refusal: it is *)
(* recorded under p's name, and the report that follows tells o of it, so o  *)
(* answers with it (vote M25) and its sample moves past it. A named open's   *)
(* leg reaches only holders under a Same name. An open with no name takes    *)
(* no refusal from a holder named by a generation (vote M35): that leg is    *)
(* p's FlushRefused, and the open hears of it only through the record        *)
(* (Fsync); BUGGY_UNNAMED_LEG_TOLD answers with it. bad_tell records an      *)
(* answer that was another file's loss. A leg that lands p's run is p's      *)
(* FlushClose.                                                               *)
(***************************************************************************)
FsyncLeg(f, o, p) ==
    /\ EnableReuse
    /\ p # o
    /\ Same(name[f][o], name[f][p])
    /\ (name[f][o] # Unnamed \/ name[f][p] = NoGen \/ BUGGY_UNNAMED_LEG_TOLD)
    /\ Refuse(f, p, TRUE)
    /\ errseq'   = [errseq EXCEPT ![f][o] = lseq + 1]
    /\ tells'    = tells \cup {"leg"}
    /\ bad_tell' = IF inc[f][p] # inc[f][o] THEN bad_tell \cup {"leg"} ELSE bad_tell
    /\ bad_seen' = (bad_seen \/ inc[f][p] # inc[f][o])
    /\ UNCHANGED <<bad_read, wpend, gen, inc, name>>

(***************************************************************************)
(* OwnWriteDead(f, o) -- a write-through in flight at the reuse ends:        *)
(* its fid answers ESTALE, to the writer. Nothing lands, nothing is          *)
(* recorded, and its pin drops.                                              *)
(***************************************************************************)
OwnWriteDead(f, o) ==
    /\ wpend[f][o]
    /\ ~Live(f, o)
    /\ wpend' = [wpend EXCEPT ![f][o] = FALSE]
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, bad_read,
                   gen, inc, name, errseq, rec, lseq, bad_tell, tells, bad_seen>>

(***************************************************************************)
(* Fsync(f, o) -- an fsync through open o reports the newest record Same as  *)
(* its name whose sequence is past its sample, once (wb_loss_report, at      *)
(* dev9p_fsync and the ring's Dev.fsync_report). Its write-back first is     *)
(* the holder's FlushClose or FlushRefused, or its own leg's (FsyncLeg).     *)
(* bad_tell records a report of a loss that was not the open's own file's.   *)
(* An open with no name reports by number, the "unnamed" arm: a deleted      *)
(* file's later loss may reach it, the cost vote M35 accepted. It marks no *)
(* record told (audit round 10 of M15, F2): the record may be another      *)
(* incarnation's, and a record marked told is sampled past by its own      *)
(* file's later opens (Sample), which would never hear it.                 *)
(* BUGGY_UNNAMED_MARKS_SEEN marks it; bad_seen records a record marked     *)
(* told by an fsync of another incarnation.                                *)
(***************************************************************************)
ReportArm(f, o) == IF name[f][o] = Unnamed THEN "unnamed" ELSE "report"

Fsync(f, o) ==
    /\ EnableReuse
    /\ Matching(f, name[f][o]) # {}
    /\ LET k     == Newest(f, name[f][o])
           marks == name[f][o] # Unnamed \/ BUGGY_UNNAMED_MARKS_SEEN
       IN /\ rec[f][k].seq > errseq[f][o]
          /\ errseq'   = [errseq EXCEPT ![f][o] = rec[f][k].seq]
          /\ rec'      = IF marks THEN [rec EXCEPT ![f][k].seen = TRUE] ELSE rec
          /\ tells'    = tells \cup {ReportArm(f, o)}
          /\ bad_tell' = IF rec[f][k].inc # inc[f][o] THEN bad_tell \cup {ReportArm(f, o)}
                                                    ELSE bad_tell
          /\ bad_seen' = (bad_seen \/ (marks /\ rec[f][k].inc # inc[f][o]))
    /\ UNCHANGED <<svr_cvers, guest_cvers, stage, cache, validated, bad_read, wpend,
                   gen, inc, name, lseq>>

Next ==
    \E f \in Files :
        \/ Open(f)
        \/ OpenSkipped(f)
        \/ ExternalWrite(f)
        \/ Evict(f)
        \/ Reuse(f)
        \/ ExternalReuse(f)
        \/ \E o \in Opens :
              \/ Read(f, o)
              \/ OwnWriteBegin(f, o)
              \/ OwnWriteLand(f, o)
              \/ StageWrite(f, o)
              \/ FlushClose(f, o)
              \/ Mint(f, o)
              \/ FlushRefused(f, o)
              \/ OwnWriteDead(f, o)
              \/ Fsync(f, o)
              \/ \E p \in Opens : FsyncLeg(f, o, p)

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

\* OneHolder (vote M23) -- at most one open holds a stage of a file at a time:
\* of each incarnation of a number (a dead holder stands beside the new file's).
OneHolder == \A f \in Files, g \in 0..MaxGen : Cardinality(HoldersOf(f, g)) <= 1

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

\* NamesTrue (vote M33) -- an open's name is its own file's generation, or none.
\* RecordsTrue (vote M31) -- a record under a generation is that incarnation's.
\* Both hold by construction here (Mint names by gen[f], a refusal records
\* inc[f][o]); they keep the buggy flags honest. The impl's obligation they stand
\* for is pinned by dev9p.m33_plain_walk_names_the_file (the Tgetattr goes on the
\* walk's own new fid) and by Stratum's h_getattr (a fid whose number was reused
\* answers ESTALE, never the new file's generation).
NamesTrue == \A f \in Files, o \in Opens : name[f][o] \in {inc[f][o], NoGen, Unnamed}
RecordsTrue == \A f \in Files, k \in 0..MaxGen : rec[f][k].seq # 0 => rec[f][k].inc = k

\* DeadStateSkipped (votes M31, M33) -- state of another generation is a deleted
\* file's: no dead open's stage or pin stands under a name Same as a live open's,
\* so no leg of the new file lands it or waits on it, and it refuses no
\* registration (LARDER-DESIGN 12.6). BUGGY_NUMBER_KEY and BUGGY_UNNAMED_WALK
\* violate it. An open with no name is outside it by vote M35: its legs land
\* every run at its number, a deleted file's too, and its pin goes by number, so
\* a deleted file's unnamed write in flight holds off staging until its ESTALE.
DeadStateSkipped ==
    \A f \in Files, o \in Opens : Live(f, o) /\ name[f][o] # Unnamed =>
        \A p \in Opens \ {o} :
            \/ Live(f, p) \/ ~Same(name[f][o], name[f][p])
            \/ stage[f][p] = 0 /\ (~wpend[f][p] \/ name[f][p] = Unnamed)

\* NoMisdirectedLoss (votes M31, M33, M35) -- no fsync is ever told a loss of a
\* file other than its own, through its report or its own leg. BUGGY_NUMBER_KEY
\* and BUGGY_UNNAMED_WALK violate it; the two arms are checked apart by the told
\* cfgs. The "unnamed" arm is vote M35's accepted cost (an open with no name
\* reports by number), shown reachable by NeverMisToldUnnamed.
NoMisdirectedLoss   == bad_tell \subseteq {"unnamed"}
NoMisdirectedReport == "report" \notin bad_tell
NoMisdirectedLeg    == "leg" \notin bad_tell

\* NoMisdirectedSeen (audit round 10 of M15, F2) -- no record is marked told by
\* an fsync of another incarnation: the mark says the record's own file heard
\* the loss, and that file's later opens sample past it (Sample). TLC's shortest
\* counterexample marks a deleted file's record, which takes no later opens; the
\* same mark made by a deleted file's open on the live file's record hides the
\* loss from the live file's next opens. An open with no name marks nothing;
\* BUGGY_UNNAMED_MARKS_SEEN violates it.
NoMisdirectedSeen   == ~bad_seen

\* CacheOfLiveFile (vote M34) -- every entry the Larder holds is of the file its
\* number names now: the guest's create drops the number's entries before the new
\* file's install (dev9p_create), so a dead open served an entry is always served
\* another incarnation's, which Read judges a wrong read. It is definitional for
\* the guest's own reuse (Reuse drops the entry, and every install is the live
\* incarnation's), standing for the create's invalidate, which is pinned by
\* dev9p.create_invalidates_reused_child and
\* dev9p.create_invalidates_reused_child_pages (audit round 10 of M15, F5). An
\* external reuse leaves the deleted file's entry in place, so
\* fs_cache_reuse_external.cfg checks NoWrongRead without it: there the serve's
\* name check alone keeps the new file's opens off the older entry.
CacheOfLiveFile == \A f \in Files : cache[f].valid => cache[f].einc = gen[f]

InvariantsReuse ==
    /\ InvariantsWb
    /\ NamesTrue
    /\ RecordsTrue
    /\ DeadStateSkipped
    /\ NoMisdirectedLoss
    /\ NoMisdirectedSeen
    /\ CacheOfLiveFile

\* Witnesses, VIOLATED by design (fs_cache_reuse_told.cfg, _told_leg.cfg,
\* _coexist.cfg, _served.cfg, _unnamed_told.cfg, _unnamed_residue.cfg): each
\* counterexample is a reachable state in which InvariantsReuse holds.
\* NeverToldReport, NeverToldLeg -- no fsync is told any loss through that arm:
\* the counterexample is the arm telling its own file's loss, so
\* NoMisdirectedLoss is not held by an arm that never tells.
NeverToldReport == "report" \notin tells
NeverToldLeg    == "leg" \notin tells
\* NeverToldUnnamed -- no open with no name is told a loss: the counterexample is
\* one told its own file's refused run through the record (a loss reaches it).
NeverToldUnnamed    == "unnamed" \notin tells
\* NeverMisToldUnnamed -- no open with no name is told another file's loss: the
\* counterexample is the cost vote M35 accepted, a deleted file's later loss
\* told by number.
NeverMisToldUnnamed == "unnamed" \notin bad_tell
\* NeverServedReused -- no open of a reused number can be served from the Larder:
\* the counterexample is the new file's open served, so NoWrongRead is not held
\* by a name check that serves nothing.
NeverServedReused == \A f \in Files, o \in Opens : gen[f] > 0 => ~ENABLED ReadCache(f, o)
\* NeverBothHold -- no two incarnations of a number hold at once: its
\* counterexample is the new file's open holding beside the dead holder, so
\* DeadStateSkipped is not held by a dead holder never standing beside a live one.
NeverBothHold ==
    \A f \in Files, g, h \in 0..MaxGen :
        (HoldersOf(f, g) # {} /\ HoldersOf(f, h) # {}) => g = h

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
