# 101 — Halls of Extinction (Tier-1 crash dump + Tier-2 symbolization) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-halls-doc-absorb`).
When the kernel dies, the Halls of Extinction capture what cannot be recovered
afterwards — registers, a frame-pointer backtrace, a stack window, the KASLR slide
— and push them out the UART before halting. Tier 1 (the dump) + Tier 2 (in-kernel
symbolization) landed; Tier 3 (persistence) is designed, not built. Its content
lives, code-verified and current, in:

- the **whole crash dump + symbolization** — `halls_dump` (the register block
  first, then the backtrace + stack hexdump), the three frame sources (explicit
  `ctx` / the per-CPU slot / a synthetic frame), the plausibility gate that makes
  the per-CPU slot usable, the bounded frame-pointer walk (depth cap + strict-increase
  cycle kill + the dying-path span ceiling), the EL0-frame-not-walked rule, the
  binary-search symbolization over the link-relative `halls_sym` table (offsets not
  absolute addresses — the load-bearing reason), and the PAC return-address
  stripping (reading the ID register directly, since the dump can fire before
  feature detection):

      vault/system/kernel/entry/sub-kernel-halls.md
      (which since the 04-extinction absorption also owns the extinction() entry)

- **Tier 3 persistence** (designed, not built):

      docs/HALLS-OF-EXTINCTION.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** `sub-kernel-halls` is the
  exhaustive home for the crash dump and symbolization; it also carries the
  live-thread twin (`halls_walk_kernel_frames`) the debugger reuses (whose safety
  argument is the *opposite* of the dying-path walk's — see its Caveats), which
  this reference doc predates.
