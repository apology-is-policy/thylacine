# Native vs ported userspace programs

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). Binding source: ARCHITECTURE.md 3.5 + UTOPIA-SHELL-DESIGN.md 3.
> Section headings keep their original levels.

## Native vs ported userspace programs (Plan 9 split)

Binding scripture under U-1 (the Utopia scripture commit): `docs/ARCHITECTURE.md §3.5` + `docs/UTOPIA-SHELL-DESIGN.md §3`. When adding a new userspace program, the decision rule is one question:

> Is this program authored within Thylacine, OR is it a port of foreign code that already expects POSIX?

- **Authored within Thylacine** → **native libthyla-rs**. The program builds against `usr/lib/libthyla-rs/` (no_std Rust, direct Thylacine syscalls). NO musl. NO Pouch boundary-line patches. Examples: `ut` (the shell), `libutopia`, the coreutils, corvus, the virtio-* drivers, the hello/probe binaries.
- **Ported foreign code** → **Pouch**. The program builds via the Pouch cross-compilation environment (musl + the `usr/lib/pouch/patches/*` boundary-line patches). Examples: stratumd, libsodium, Helix, future ports of ssh / git / python.
- **Authored within Thylacine, but wanting `std` + crates.io** → **first-party `std` Rust on Pouch** (the THIRD substrate; operator decision 2026-09-21, ARCH §3.5 as amended; `docs/RUST-STD-DESIGN.md` §9, on the aux track's branch until track R merges -- `docs/handoffs/041-rust-std-track-to-aux.md` is the brief on `main`). Target `aarch64-unknown-thylacine`, family `unix`, `std` over the Pouch libc — NOT over libthyla-rs. Sanctioned for NEW programs, not ports only; `libthyla-rs` stays the native default. The author chooses per the program's needs and says so in the commit. It inherits every Pouch limit whole.

The boundary determines the runtime substrate. The rationale mirrors Plan 9's `libc.h` (native) / APE (POSIX ported) split: native programs benefit from being Thylacine-shaped — smaller binaries, faster startup, no impedance mismatch, fewer patches to maintain — while ported programs get POSIX-shape via the pouch boundary-line, which is the right place to do the translation work once per surface rather than at every program's syscall site.

Operational implications:
- A new utility we're authoring → libthyla-rs. If a Rust ecosystem crate seems convenient but assumes std, prefer to hand-roll the no_std equivalent (or extend libthyla-rs to provide what's needed) over reaching for Pouch.
- A new ported dependency → Pouch. Pouch-patch growth is expected and audit-bearing; new POSIX surfaces touched by a port get their own patch under `usr/lib/pouch/patches/*` and follow the existing pouch audit discipline.
- A native program SPAWNING a ported program → fine (they're separate processes; the boundary is fd-level, not library-level). Example: `ut` (native) spawns `hx` (ported via Pouch).
- A native program LINKING a ported library → not part of v1.0. If the situation arises, escalate; we'd have to design a sysroot for the native target that re-exports musl shapes, which is a meaningful new direction.

`tools/build.sh` enforces the split: the Utopia workspace builds via the `aarch64-thylacine` Rust target (no_std on libthyla-rs); ports build via Pouch's sysroot. The two paths are clearly separated.

---
