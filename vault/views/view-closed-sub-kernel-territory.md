---
id: view-closed-sub-kernel-territory
type: view
title: "Do-not-re-report preamble — sub-kernel-territory"
query: closed:sub-kernel-territory
---
# Do-not-re-report preamble — sub-kernel-territory

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-territory`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
10 closed findings on [[sub-kernel-territory]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-66b-r1-f2]] [P3] A truncated mount line concatenated into the binds: line (fixed) — Fixed: each mount line renders ATOMICALLY — snapshot the offset, rewind
- [[fnd-66b-r1-f3]] [P3] The read-buffer headroom comment assumed short names (fixed) — Fixed as documentation: reworded to say 512 bytes holds the common
- [[fnd-ls4-r1-f1]] [P3] A deep cwd plus a long relative path is rejected though it would resolve (fixed) — Fixed as documentation — the combined-length bound recorded as a known
- [[fnd-ls4-r1-f2]] [P3] chdir resolves the path lexically, then stalk re-clamps what is already clean (fixed) — Fixed as a comment at the call site, NOT by removing the redundancy.
- [[fnd-ls4-r1-f3]] [P3] source_is_valid is a tautology (documented) — Documented, no action — pre-existing and out of the LS-4 scope that
- [[fnd-shed-r1-f1]] [P1] chroot / pivot INTO A UNION DIRECTORY sheds the union's own live entries; every root-based resolution then fails (fixed) — Fixed, scripture first: ARCH 9.6.10 names the point's instance as a second seed; `reach[]` is `PGRP_MAX_MOUNTS + 2` with an overflow extinction and a test landing exactly on the bound. The snap is set once before the Spoor is published and freed with it, so the read under `ns_lock` needs only the root ref the caller holds. `spoor.h`'s "ONLY `spoor_readdir_run` consults it" -- the stale comment that hid the dependency -- corrected, and [[sub-kernel-spoor]] gained the field its struct listing never had. "What does the resolver consult at the base without walking to it" is prosecution item (8) of the audit row.
- [[fnd-shed-r1-f2]] [P2] ShedLosesNothing is a tautology: the spec cannot detect a wrong reachability rule (fixed) — Fixed by a rewrite: the ground truth is an operational WALKER (start in the root's tree and, for a union root, the point's; cross; per-walker restamp; never up) that shares NO operator with the closure the rule computes; `WalkerWithinClosure` pins the truth from below. Same-tree binds and swaps are explored. Three new buggy cfgs (`no_union_seed` = F1, `undeclared_per_walker`, `dotdot_escapes` = the I-28 premise as an executable dependency). One limit is stated in the module: a truth closure that is too LARGE only weakens the completeness half, and nothing pins it from above.
- [[fnd-shed-r2-f1]] [P2] a union handle is a latent capability on the COVERED directory at its mount point: dissolve the union and '/' or '.' hands it over (fixed) — Fixed as suggested, as one sentence of scripture (ARCH 9.6.10). The base-set site probes `mount_member_at(point, 0)`; the zero-component site enforces the rule as a POST-condition of the cross, so a peer Thread's `unmount` opens no window between a check and the cross. `STALK_MOUNT` still keys the point. The first version of the fix was itself wrong for an OPENED union handle and was caught by its own kernel test before any gate: see [[fnd-b0self-r1-f1]].
- [[fnd-shed-r2-f2]] [P2] the shed closure's seed obligation is recorded only on the shed's side, where the next resolver change will never look -- and TLC cannot see a seed missing from both rule and truth (fixed) — Fixed on every surface named, and the rule stated once: a new base-time consult in the resolver is a new seed in the shed, and no spec can notice one missing from both sides.
- [[fnd-stube2-r1-f5]] [P3] Territory asserted its size but not its field offsets (fixed) — Fixed: per-offset asserts on `root_spoor` (24), `binds` (32), and
<!-- generated:end -->
