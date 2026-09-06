---
id: chg-2026-09-06-cons-doc-absorb
type: chg
title: "absorb docs/reference/111-cons (I-27/I-9 pollable console): fold the #95 RX input-drop report into sub-kernel-cons"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-cons]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---

# docs/reference/111-cons.md -> ABSORBED (I-27/I-9 audit-trigger surface)

Absorbed the 1091-line console reference doc into a multi-redirect stub. The
owning dossier `sub-kernel-cons` (616 lines, guarded-by inv-i27/i9, updated
2026-09-06) is exhaustive and current -- verified atom-by-atom, it covers the
organizing fact (the IRQ producer's four deferred relays), the four rings, the
line discipline (echo-off password mask), the writer role + the banner-tear
finding, the deferred poll-wake (I-9, cons_poll.tla), the receive back-pressure
+ the holdback-strand P1, the control file + the login-passphrase mode-flip
disclosure, the winsize/beacon/serialsilent verbs, the renderer drain/feed, and
the extinction ring-lock. The /dev front-door -> sub-kernel-devdev; the banner
ABI -> abi-boot-banner.

ONE genuine residue, code-grounded and folded:

- **The #95 RX input-drop report was in no dossier body.** sub-kernel-cons
  covered the back-pressure concept and REFERENCED "the report" (a real drop
  "arms the report") without describing it. Folded the report mechanism: the five
  named counters (rx_bp_raw/rx_bp_flush refusals, rx_drop_line, the zero-witness
  rx_drop_ring, and rx_drop_modeflush), and critically rx_drop_modeflush as the
  mode-flush drop the mode-flip discipline does NOT cover (a consctl ICANON-clear
  delivers a half-assembled line the full ring drops -> the fragment tail lost,
  terminator arrives raw -> #95's truncated-command-runs shape, reachable by
  ordinary type-ahead not only a wedged reader), plus the boot-gated
  (boot_is_complete, cons.c:1170) drop_report_pending/drop_reported one-shot latch
  (cons.c:187-192). Also recorded the known-open hazard (MEMORY.md #95): the
  one-shot latch is spent by its own test, so a genuine post-boot drop after the
  test ran reports nothing.

91 -> 92 absorbed of 157. lint 0-fail.
