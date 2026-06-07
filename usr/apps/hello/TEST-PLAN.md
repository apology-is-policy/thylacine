# TEST-PLAN: hello (A0 bootstrap)

Status: **authored, NOT executed.** The auxiliary track never boots QEMU;
this plan is the main agent's ready-to-run backlog. The verification done
here is `cargo build --release` (clean link) + ELF inspection only.

## What it is
The minimal native libthyla-rs binary: writes one line to the kernel
diagnostic UART (SYS_PUTS via `t_putstr`) and returns 0 -> `exits("ok")`.

## Build verification already performed (no boot)
- `cargo build --release` from `usr/apps/`: clean link (3.0 s cold).
- Artifact: `build/usr-rs/aarch64-unknown-none/release/hello`, 4904 B,
  ELF 64-bit aarch64, statically linked.
- `_start` @ 0x400000 (entry), `rs_main` @ 0x400018 -- identical offsets
  to the main-tree `usr/hello-rs` (toolchain parity confirmed).
- One PT_LOAD, flags `R E` (0x5). W^X-clean: no segment carries W+X.
  (No RW segment because the binary has no `.data`/`.bss` -- `ThylaAlloc`
  is zero-sized and the only string lives in `.rodata` inside the RX load.)

## Output channel
`t_putstr` -> `SYS_PUTS` -> kernel diagnostic UART. The executor observes
the line on the **serial console**, NOT on a redirectable fd-1 pipe. (Real
fd-1 stdout for coreutils is a separate surface; see DOC-GAP-REPORT G04.)

## Cases (for the in-VM executor)

| # | Setup / argv | Expected serial output | Expected exit |
|---|---|---|---|
| T1 | spawn `/hello`, no argv | line `hello from usr/apps/hello (native libthyla-rs, aux track)\n` appears on serial | status 0 -> parent `wait_pid` observes `exits("ok")` |
| T2 | spawn `/hello` under the shell `ut` (if integrated) | same line on console | shell reports exit 0 |

### Notes for the executor
- This binary takes NO argv and reads NO fd; it is purely a toolchain +
  spawn-path smoke test. If it prints its line and exits 0, the native
  auxiliary build/spawn pipeline is sound end to end.
- To ship it, add `hello` to the curated Rust-binary list in
  `tools/build.sh::build_ramfs` (the auxiliary track does NOT edit
  tools/; this is a note for the main agent).
