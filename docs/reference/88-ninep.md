# 88 — t::ninep (libthyla-rs 9P codec) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-ninep-codec-absorb`).
This is the **userspace** `libthyla_rs::ninep` serving codec (not the kernel 9P
client — that is [[sub-kernel-ninep-client]], cited here only as the wire
reference). Its content now lives, code-verified and current, in the dossier:

    vault/system/userspace/runtime/sub-libthyla-rs.md

(the "The 9P serving codec" section: the `no_std`/`no_alloc` server-side codec
that carries no session state / fid table / tag allocation, the codec invariants
— pack/unpack identity, no over-read/over-write, the back-patched size,
`parse_twalk`'s double bound — and the load-bearing distinction that the 9P
*session* invariants I-10/I-11 are **server-state** enforced above the codec, not
codec invariants. The wire constants, message-type numbers, and struct layouts
are the code's, in `usr/lib/libthyla-rs/src/ninep.rs`.)

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- Its Status line still reads "landed at U-2h-ninep (commit `*(pending)*`)" — a
  hash placeholder that was never filled. The dossier's Provenance carries the
  real lineage via chg notes.
- It is largely an **API + constant reference** (the `P9_T*` message-type
  numbers, the pack/unpack signatures, the `Qid`/`Header` wire layouts) — all of
  which are the code's to pin (`ninep.rs`), which is why the dossier states the
  invariants and points at the source rather than reproducing the tables (a
  duplicated constant is a constant that rots).
- The client-side codec (T-builders + R-parsers) it lists as "deferred" remains
  deferred — a mechanical mirror held for a real consumer; the dossier records
  that as a live seam, not absorbed content.
