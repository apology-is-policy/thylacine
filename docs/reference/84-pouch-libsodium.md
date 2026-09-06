# 84 — pouch-libsodium: the first cross-compiled C library against pouch [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-pouch-libsodium-doc-absorb`).
Phase 6 sub-chunk 14: cross-compiles **libsodium** (Frank Denis's modern crypto
library, ISC) for `aarch64-thylacine` against the pouch sysroot — the milestone
proof that the pouch cross-compilation substrate handles a real C library. Its
content lives, code-verified and current, in:

- the **cross-compilation substrate** — the sysroot rebuild, the patch-apply loop,
  the build/link recipe (this is a build-recipe doc: "why no patch series", "why
  no `./configure`", the source list, the link line, the proving binary):

      vault/system/substrate/sub-substrate-build.md

- the **pouch boundary-line** libsodium validates it needs *none* of beyond what
  is already patched (no new syscall shim):

      vault/system/boundary/pouch-seam/sub-pouch-seam.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** This is a build-recipe /
  proof-of-concept doc, not a mechanism home: libsodium was the first C library
  cross-compiled against pouch, needing no boundary-line patch, so it demonstrates
  the substrate rather than adding one. The build substrate is
  `sub-substrate-build`'s; the boundary-line it rides is `sub-pouch-seam`'s.
