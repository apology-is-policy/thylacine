# 139 — tapestryd: the compositor + the orphaned-weave reaper (I-40) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-tapestryd-doc-absorb`).
The warden-bound persistent compositor — two virtio-gpu device halves, a 9P
server presenting the surface/pane tree, the pane-tree composition, the
interaction layer (chords / focus / move-zoom), menus, and the present half of
**I-40** (no torn scanout / surface-share integrity). Audit-bearing. Its content
lives, code-verified and current, in:

- the **whole compositor** — the process shape, the two device halves + the
  scanout path, the 9P server (`server.rs`), the surface lifecycle (the I-40
  present half: a weave stays backed + mapped-membership-immutable from first
  client map to retire, a present op brackets its `TRANSFER_TO_HOST_2D`, a weave
  retires only after quiesce + scanout-composition release — "scanout off before
  the resource dies"), the events, the G-6a pane-tree multi-surface composition,
  the G-6b resize protocol + weave generations, the G-6c interaction layer
  (chords / focus / strips / move-zoom / multi-rect / determinism), the H-3c
  menu grab + compositor-owned dismiss, and the idle throttle:

      vault/system/userspace/services/sub-tapestryd.md
      (guarded-by inv-i40/i5/i34/i1/i45/i9; the dossier is larger than this doc
       and carries the later H-4 and Warp/WSI GPU work on top of it)

- the **orphaned-weave reaper** (the kernel R2-F3 sweep, `kernel/weft.c`) — a
  kernel thread force-reclaims a framebuffer binding whose serving session has
  been dead past a grace period (the client's stale mapping unmapped cross-Proc,
  budget uncharged, pin dropped):

      vault/system/kernel/async/sub-kernel-weft.md   (the orphan reaper)

- the **gather grant** (libdriver + the warden) — how the warden confers the
  compositor's device-gather authority at spawn:

      vault/system/userspace/runtime/sub-libdriver-grant.md

- **libtapestry + tapestry-demo** — the client-side event-ring library and the
  demo:

      vault/system/userspace/runtime/sub-libtapestry.md

- the **formal model** — `specs/tapestry_present.tla` (the I-40 no-torn-scanout /
  surface-share integrity model, gated by `specs/check-tapestry.sh`).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The doc is a compositor-arc
  snapshot (G-3 / G-6a-c / H-3c menus / the gather grant / the reaper / the idle
  throttle); `sub-tapestryd` is well ahead of it, having grown the H-4 layout
  work and the Warp/WSI GPU present classes on top (the V-3/W-3 device-side
  surfaces documented across the tapestryd rows of `docs/AUDIT-TRIGGERS.md`).
  Every atom of this doc was verified present in the dossier.
- **The content is distributed** — the compositor to `sub-tapestryd`, the kernel
  orphan reaper to `sub-kernel-weft`, the gather grant to `sub-libdriver-grant`,
  the client library to `sub-libtapestry`, the model to `tapestry_present.tla`.
