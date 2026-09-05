# 02 — DTB parser [ABSORBED INTO THE VAULT]

This document was absorbed at the boot sweep (`chg-2026-08-02-boot-sweep`).
Its content now lives, code-verified and current, in the dossier:

    vault/system/kernel/boot/sub-kernel-dtb.md

(the `volatile` four-byte read and the Device-memory alignment rule that
forces it, the token walker, the two lookup styles and why name-matched
lookups may use a flag while node-matched ones may not, the per-depth
accumulator stack, the depth cap's actual behaviour, relocation into a
kernel-owned buffer and its place in the boot ordering, and the
offset-addressed tree-walk surface that exposes the tree to userspace).

**What this file got WRONG by the time it was absorbed** (the reason the
dossiers are written from the code): it says "I-15 is satisfied by USE of the
parser's results, not by the parser itself". That is the opposite of how the
invariant is enforced — the parser being the *sole* reader of the blob is the
mechanism; a second parser, or one hardcoded address elsewhere, is exactly
what the invariant forbids. `vault/invariants/inv-i15.md` states it the other
way round, with the parser as the enforcement home.

Also stale: the scope is P1-B, so two substantial later additions are absent —
the relocation of the blob into a kernel buffer (without which the identity
map could not be retired) and the entire offset-addressed tree-walk API that
lets a device expose the tree to userspace, which is the only part of this
file that faces a trust boundary.

**One thing this file got RIGHT that the code comment gets wrong**, which is
worth recording because it inverts the usual direction: this document says a
tree nested deeper than the cap is "silently skipped beyond the cap", which is
accurate. The comment at the cap in `lib/dtb.c` says such a tree would
"panic, not silently corrupt" — right about the safety, wrong about the
mechanism. The stale document is the accurate one on that point.

The invariant lives at `vault/invariants/inv-i15.md`. The open debt is
`seam-dtb-blob-internally-trusted` (no task; the blob is firmware-supplied and
the parser validates its callers rather than its input). Design scripture is
unchanged: `ARCHITECTURE.md section 22.2`, the devicetree specification v0.4.
