# Boot banner contract

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). The authority for consumers is the vault note `abi-boot-banner` (via `quaestor owner`).
> Section headings keep their original levels.

## Boot banner contract (kernel ABI with the development tooling)

Per `TOOLING.md §10`. Non-negotiable for the agentic loop to work.

The kernel prints this banner during boot. `boot_main()` (`kernel/main.c`) prints the multi-line header during late bring-up; the final `Thylacine boot OK` line is printed by `boot_mark_complete()` when **init (joey) signals `SYS_BOOT_COMPLETE`** -- after joey's boot-test asserts pass, just before it transitions to the persistent session supervisor (getty-loops `/sbin/login`). Since A-5a joey is the long-running init and does NOT exit on success, so the banner can no longer ride its reap. `SYS_BOOT_COMPLETE` is one-shot + gated on the caller being console-attached (so a spawned child cannot fake a premature banner -> a false PASS); a boot failure before the signal extincts in `joey_run` and the banner never prints.

```
Thylacine vX.Y-dev booting...
  arch: arm64
  cpus: N
  mem:  XXXX MiB
  dtb:  0xADDR
  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries (unconditional); PAC/BTI/LSE conditional (P1-H; Lazarus W1)
  features: PAC,BTI,LSE,CRC32 (CPU-implemented)
  kernel base: 0xADDR (KASLR offset 0xADDR)
Thylacine boot OK
```

The `hardening:` / `features:` lines are informational. **`kernel base:` is NOT** — that claim stood here until 2026-08-16 and was false: `tools/verify-kaslr.sh` parses `kernel base: 0xVA (KASLR offset 0xN, ...)` and is the **ROADMAP §4.2 exit-criterion gate for I-16**, i.e. that invariant's only runtime witness, and `tools/stall-watch.py`'s `KASLR_RE` parses the same line to symbolize a stalled guest. Their failure modes differ in the way that decides how bad a reword is: verify-kaslr fails **loud** (an unparsed offset makes every boot's offset the empty string, the distinct set collapses to 1, and the run misses its `>=0.7N` bar), while stall-watch fails **SILENT** (`if m:` with no else leaves `syms.slide` NULL and the watcher keeps running, having quietly lost the symbolization it exists to provide, exactly when a guest has stalled). So the binding set is three surfaces, not two. Since Lazarus W1 (`PORTABILITY.md §4`) the `hardening:` line lists the unconditional set and marks PAC/BTI/LSE runtime-conditional; the `features:` line reports what the running CPU implements.

A kernel **extinction** (ELE — Extinction Level Event; the thematic name for kernel panic) prints `EXTINCTION: <message>` as a recognizable prefix. Use `extinction(msg)` or `extinction_with_addr(msg, addr)` from `kernel/extinction.c`; `ASSERT_OR_DIE(expr, msg)` for assert-style checks. These strings are part of the kernel ABI with the development tooling, and changing one is a **format break** (§"Autonomy + escalation"): surface it, do not just sweep.

**Do NOT trust a hand-written co-update list here — this one was wrong in three different ways at once, and the third is the instructive one.** It named `tools/agent-protocol.md`, which was planned in Phase 1 and never written (removed 2026-08-15, main#244: an unfollowable member teaches the reader the whole list is advisory). It named `tools/run-vm.sh`, which matches **neither literal — zero hits, structurally**, because it is a QEMU *launcher* that assembles a command line and hands over an interactive UART; it never reads boot output and cannot break. An **inert** member does the same damage as a **fictional** one: a reader dutifully opens it, finds nothing to change, and concludes the rest is advisory too. And it omitted **fourteen** files that do match (`test.sh`, `smp-multiboot.sh`, `test-cross-reboot.sh`, `test-fault.sh`, `ci-idle-gate.sh`, `np3-bench.sh`, `verify-kaslr.sh`, `warp/boot-probe.sh`, and six `interactive/*.exp`), plus two comment-only mentions.

**Why it rotted, which generalizes past this list** (vault, 2026-08-16): it conflates two kinds of member. A **program** that matches the string breaks *silently and immediately*; a **document** that states it merely *becomes wrong*, and nothing fails. `CLAUDE.md` and `TOOLING.md §10` are the second kind. **A list whose members share no property has no property any member can be checked against** — which is exactly how a phantom and an inert member sat in it unremarked for the project's life. Note `tools/test-fault.sh` matches seven extinction MESSAGE bodies, not just the prefix, so rewording one for clarity makes a hardening gate report that the protection did not fire.

The authority is the vault's `abi-boot-banner` note and its `mirrors` set (R6-enforced at change time), reached the usual way — `quaestor owner <changed paths>` in the mandatory doc-update step. Consult it rather than any list transcribed here, this one included.

---
