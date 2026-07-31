# 132 — The Larder (guest-side 9P FS cache) [ABSORBED INTO THE VAULT]

This document was absorbed at the Larder sweep
(`chg-2026-07-31-larder-sweep`). Its content now lives, code-verified
and current, in the dossier:

    vault/system/kernel/ninep/sub-kernel-larder.md

(the mechanism — kernel/larder.c) and

    vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md

(the policy — every serve/populate/invalidate call site, the
write-behind engine, cached-open, the cacheability latch). The audit
history (L1f, task #25, task #29, B1, D44, term-2, term-4) lives as
adt-/fnd- Record notes; the open v1.x items as seam-larder-* notes; the
do-not-re-report preamble as vault/views/view-closed-sub-kernel-larder.md.

The dossier supersedes this file. Do not extend this stub -- extend the
dossier and its linked registry notes. This stub is deleted after the
vault migration's full-corpus verification pass (vault/meta/schema.md
section 10.6).
