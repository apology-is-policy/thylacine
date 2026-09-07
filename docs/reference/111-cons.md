# 111 — /dev/cons: the pollable console + line discipline (I-27 / I-9) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-cons-doc-absorb`).
The kernel console — one physical UART presented as `/dev/cons`, with a line
discipline, a control file, a window size, and (since the compositor) a mirror of
its output and an injection point for its input. It is the machine's last-resort
interface and the trusted path, which is why a device that would be a hundred
lines is sixteen hundred. Audit-bearing; enforces **I-27** (trusted path) +
**I-9** (no lost wake across the interrupt-deferral). Its content lives,
code-verified and current, in:

- the **whole console** — the organizing fact (an IRQ producer that may do none
  of the work, so four deferred relays via the manager kthread), the four rings,
  the five-flag line discipline (echo-off as a hard password-mask guarantee), the
  transmit ring + the **writer role** (and the year-long writer-set gap that tore
  `Thylacine boot OK` and reported a healthy guest as a boot failure), the
  deferred poll-wake (I-9, `cons_poll.tla` — register-then-observe at two levels),
  the receive back-pressure (refuse-not-drop, the one-byte holdback and its
  strand-at-one-exit P1) and the **#95 RX input-drop report** (folded at this
  absorption), the control file's parse-all-then-apply grammar + the mode-flip
  ordering rule (the login-passphrase disclosure), the winsize iff-changed post,
  the `beacon`/`serialsilent` verbs, the renderer drain/feed (the hardwired-false
  line-condition), and the extinction crash-emitter holding the ring lock to
  `_torpor`:

      vault/system/kernel/console-gfx/sub-kernel-cons.md   (guarded-by inv-i27/i9)

- the **`/dev/cons` namespace front-door** — the I-27 gate-at-namespace-open that
  makes the path walk and the `SYS_CONSOLE_OPEN` syscall two front doors to one
  implementation:

      vault/system/kernel/console-gfx/sub-kernel-devdev.md

- the **boot-banner ABI** — `Thylacine boot OK` and the extinction strings the
  tooling greps (the writer role protects the banner line):

      vault/system/boundary/registries/abi-boot-banner.md   (a PIN)

- the **formal model** — `specs/cons_poll.tla` (the deferred-wake no-lost-wake).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The #95 RX input-drop report was in no dossier body — folded at absorption.**
  `sub-kernel-cons` covered the receive back-pressure concept and *referenced*
  "the report" (a real drop "arms the report") without describing it. The report
  mechanism — the five named counters (`rx_bp_raw` / `rx_bp_flush` refusals,
  `rx_drop_line`, the zero-witness `rx_drop_ring`, and `rx_drop_modeflush`), the
  boot-gated `drop_report_pending`/`drop_reported` one-shot latch, and
  `rx_drop_modeflush` as the mode-flush drop the mode-flip discipline does not
  cover (the #95 truncated-command shape, reachable by ordinary type-ahead) — is
  now folded into `sub-kernel-cons`, along with the known-open hazard that the
  one-shot latch is spent by its own test (`cons.c:1170`, `:187-192`).
- **The file's own opening comment is stale** (a write-only console whose reads
  return EOF and whose control file is "held until a later phase" — all long
  since landed); `sub-kernel-cons`'s Caveats already record this.
- **The content is distributed** — the console to `sub-kernel-cons`, the `/dev`
  front-door to `sub-kernel-devdev`, the banner ABI to `abi-boot-banner`, the
  model to `cons_poll.tla`.
