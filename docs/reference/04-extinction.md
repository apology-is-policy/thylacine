# 04 — Extinction (kernel ELE) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-extinction-doc-absorb`).
When the kernel hits an unrecoverable condition it calls `extinction(msg)` (or
`extinction_with_addr(msg, addr)`), which prints `EXTINCTION: <msg>` to the UART
and halts forever — the boot's lineage is over, only a fresh boot continues. The
thematic name is intentional (the thylacine, declared extinct in 1936; a dead
boot's lineage is extinct). Its content lives, code-verified and current, in:

- the **extinction entry + the crash dump + the halt** — `extinction()` /
  `extinction_with_addr` (the ELE entry, `kernel/extinction.c`, **folded here at
  this absorption** — it was previously unowned), the `EXTINCTION:` tooling-ABI
  marker (matched literally by `tools/test.sh` and the agentic loop),
  `ASSERT_OR_DIE`, the recursive-extinction suppression, the `_torpor` halt, the
  `halls_dump` crash dump it drives, and the owed-`IPI_HALT` line-tearing seam
  (#243):

      vault/system/kernel/entry/sub-kernel-halls.md
      (title: "Halls of Extinction — the crash dump and the live-thread backtrace")

- the **crash-emitter console serialization** — the extinction path takes the
  console ring lock and holds it to `_torpor` (never the parking writer role — a
  dying machine must not block):

      vault/system/kernel/console-gfx/sub-kernel-cons.md

- the **`EXTINCTION:` prefix as a tooling ABI** — one of the boot/agentic-loop
  contract strings:

      vault/system/boundary/registries/abi-boot-banner.md   (a PIN)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **`kernel/extinction.c` was unowned — folded at absorption.** `sub-kernel-halls`
  described the dump and *referenced* the extinction path (the `EXTINCTION:` line,
  the halt) but its `code:` claimed only `halls.c`; the `extinction()` entry, the
  ABI marker, the recursion guard, and the halt were in no dossier. Now folded
  into `sub-kernel-halls`, which claims `extinction.c` + `extinction.h`.
- **The extinction-line tearing is a known-open bug (#243).** The crash emitter
  serializes its own output, but `IPI_HALT` is owed, so a peer CPU's `uart_puts`
  can still tear the `EXTINCTION:` marker on SMP — recorded as a seam in
  `sub-kernel-halls`.
