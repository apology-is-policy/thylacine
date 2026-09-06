# 31 — Kernel-internal trivial Devs (P4-B) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-entry-stubs`). The
P4-B trivial-Dev wave (`cons`, `null`, `zero`, `random` + the `dev_simple_*`
helpers + bestiary registration). Its content splits by code-owner:

- the **`null` / `zero` / `full` leaf Devs, the `dev_simple_*` helpers, and the
  deterministic `dev_init` bestiary order**:

      vault/system/kernel/namespace/sub-kernel-dev.md

- the **`random` Dev + the RNDR mechanism** — the FEAT_RNG probe from
  `ID_AA64ISAR0_EL1` bits[63:60], the `PSTATE.NZCV` capture idiom (`cset` on
  `ne`, the 10-attempt transient-dry retry, the load-bearing `"cc"` clobber),
  and the seed-readiness gate:

      vault/system/kernel/devices/sub-kernel-content.md

- the **`cons` Dev** — the RX ring, the single-reader busy-guard, the blocking
  death-interruptible read, the Ctrl-C cooked-consume, and the BREAK→SAK
  trusted-path handoff:

      vault/system/kernel/console-gfx/sub-kernel-cons.md

- the **PL011 UART RX itself** — `uart_rx_init`, the RX-FIFO drain, and the
  `DR.BE` break split that feeds the SAK:

      vault/system/kernel/devices/sub-kernel-uart.md

**What this file got WRONG or MISSED by the time it was absorbed** (it is a P4-B
snapshot of an architecture that has since moved a long way):

- **`devcons.read` no longer "returns 0 at v1.0".** Its central caveat says the
  console reader is degenerate (immediate EOF); the RX path landed at A-4c-1 and
  the reader is fully live (blocking, single-reader-guarded, death-interruptible)
  — the dossiers describe the live path.
- **`random` is no longer RNDR-only, and the standalone `devrandom` read-path is
  superseded.** The ChaCha20 stir it "holds to a future sub-chunk" landed — RNDR
  is now one of three seed inputs (DTB seed + `CNTPCT` jitter + host virtio-rng),
  not the sole source — and `/dev/random` is reached through the `devdev` leaf
  over the CSPRNG, not through the standalone `devrandom` Dev (still registered
  for its boot seed, but no longer the read path).
- **`/dev/urandom` and `/dev/consctl`, both "held" here, have landed** — as
  `devdev` leaves — and `full` joined the trivial set. The bestiary is larger
  than the five this file lists.
- The `dc='r'` / ramfs collision "trip hazard" it flags was resolved long ago;
  the whole `dc`-character-space note is P4-era planning, not as-built.
