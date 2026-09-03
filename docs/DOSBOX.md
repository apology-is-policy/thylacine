# DOSBox on Thylacine -- the DOS-emulation arc (design)

> Status: DESIGN (scoping, 2026-09-03). Arc prefix **DX**. Design-first: this
> scripture lands before code; each sub-chunk implements against it, audited
> where invariant-bearing. Owner: aux track.

## 1. Goal + why it belongs

Port **DOSBox** so Thylacine runs the vast library of DOS applications and games.
This is a flagship of Thylacine's **emulation strength** -- the same conviction
behind VIVARIUM (unmodified Linux binaries), the planned x86 translation layer,
and the planned Wine path. VIVARIUM answers "run Linux binaries"; DOSBox answers
"run DOS software," a whole platform otherwise extinct on modern hardware. It is
also a real, fun, visible payoff: a graphical, interactive retro-computing target
that exercises the compositor + input + JIT end to end.

Positioning: DOSBox is a **port** (a GPL app running on Thylacine), not a new
kernel mechanism -- EXCEPT its dynamic recompiler, which becomes a genuine
demonstration of **JIT-as-a-capability (I-42 / CAP_JIT)**: an x86->ARM64 JIT
riding Thylacine's capability-gated, W^X-preserving code emission. That angle is
the novel, load-bearing part of the arc, and (see 6) DOSBox turns out to be a
*cleaner* fit for the as-built JIT surface than a general dynamic binary
translator would be.

## 2. Licensing -- CLEAR

- DOSBox (all major variants: original, Staging, DOSBox-X) is **GPL-2.0-or-later**.
- Thylacine is **GPL v3**. GPL-2.0-**or-later** is COMPATIBLE with GPLv3 (the
  "or later" clause lets the code be taken under v3). GPL-2.0-**only** would be
  incompatible -- DOSBox is not that.
- A standalone app (separate process) = mere aggregation, no combination question.
  A Pouch build linking Thylacine GPLv3 libs = combined work under GPLv3, fine
  (musl is MIT; only the Thylacine boundary-line patches are GPLv3). SDL2 is zlib.
- The one real check at vendor time: grep the DOSBox tree for any VENDORED
  GPL-2.0-**only** or GPLv3-incompatible third-party component (rare).

## 3. Source target -- DOSBox Staging

- **DOSBox Staging** (modern C++17/20, **SDL2** 2.0.2+, actively maintained,
  cleanest tree). Its x86 core is host-arch-independent. THE target.
- NOT classic DOSBox (SDL 1.2): Thylacine's backend is SDL2, and SDL2 has no
  fbdev path -- SDL1.2 would be a bad fit.
- DOSBox-X (SDL2, GPLv2-or-later, feature-rich) is the alternative if its extras
  are wanted; Staging is the default for a first port (smaller, cleaner). (An
  operator call -- see the arc-open decisions.)

## 4. Build path -- Pouch (native port), NOT libt-native, NOT viv

- **Pouch (musl + boundary-line) is the route.** Ported foreign POSIX C++ ->
  Pouch is scripture (ARCH 3.5), and it is the PROVEN path: SDL2 + the C++
  runtime already exist on Pouch (see 5).
- **NOT native (POSIX->libt):** Thylacine's native path is Rust no_std / C libt
  with NO native C++ runtime. DOSBox is ~100K LOC of C++ (exceptions, RTTI, STL,
  threads); a native port would mean re-authoring it AND building a native C++
  std, AND re-porting SDL. Wrong fit.
- **NOT viv (prebuilt Linux binary):** the vivarium-graphics arc (W4) is entirely
  unbuilt and its Wayland/AF_UNIX stage is deferred post-v1.0 (Mycelium). A stock
  Linux DOSBox cannot reach the display under viv today. (Networking under viv
  works; graphics does not.)

## 5. Architecture -- how it maps (the hard parts already exist)

DOSBox = **"TyrQuake, upgraded C -> C++."** Every load-bearing dependency is
built and gate-passing in-tree:

- **SDL2 + `SDL_thylacine`** (`third_party/SDL2` 2.32.10, zlib; backend at
  `usr/ports/sdl2/thylacine/`): video renders zero-copy to a **Tapestry weave**
  (`thyla_tap.c`, plain C over 9P to `/srv/tapestry` -- no Rust dep); present is
  one blocking `tpresent` write, tear-free. Input: a pthread parks on the
  tapestry event fid, evdev keycodes -> SDL scancodes, relative + absolute mouse.
  PROVEN by **TyrQuake** (969 frames to the real scanout, CI green).
- **Pouch C++ runtime**: static libc++/libc++abi/libunwind over musl (Clade CL-2),
  prover-passing (throw/catch, RTTI, std::thread, STL, iostreams, std::filesystem).
  GATE: requires the LLVM fork clang present (build.sh skips C++ otherwise).
- **The port idiom** (TyrQuake template, `docs/reference/143-tyrquake.md`): vendor
  pruned-pristine + a boundary-line patch series + a curated object-list build in
  `tools/build.sh` (mirror `build_tyrquake()`/`build_sdl2()`) + null-sound +
  probable stack/heap sizing (TyrQuake forced EXEC_USER_STACK_SIZE 256K->1M).
- **Placement**: DOSBox mints a tapestry surface; Halcyon/tapestryd place it as a
  **pane** (a tile). No new client API. Software renderer -> the proven software
  weave path (NOT the GL/Vulkan/Warp path; DOSBox needs no GPU accel).

## 6. The CAP_JIT dynarec (I-42) -- the load-bearing sub-chunk

DOSBox's dynamic core translates x86 basic blocks to ARM64 at runtime, executes
them immediately, and re-emits on self-modifying DOS code. On ARM this needs
write-then-execute, which strict W^X (I-12) forbids -- except through **CAP_JIT
(I-42)**, which is **AS-BUILT + proven** (CL-7k, kernel `1f0e66c0` + userspace
`5633d056`; a Rust wrapper, an in-guest E2E prover, a real LLVM ORC mapper, and
llvmpipe rendering GL 4.6 on-device all ride it).

**The mechanism -- dual-mapping, not an RW->RX flip.** A code Burrow
(`BURROW_TYPE_CODE`) maps one set of physical pages at TWO virtual addresses in
one Proc: **RW at `writer_va`, RX at `exec_va`**, each a separate VMA with fixed
prot. No PTE is ever W-and-X, so **I-12 holds at page granularity unchanged, not
relaxed**. `SYS_JIT_CREATE(len, out)` installs both aliases atomically ->
`{writer_va, exec_va}` (16-byte `struct t_jit_region`). Emit = **plain stores
through `writer_va` (NOT a syscall)**. Publish = **`SYS_ICACHE_SYNC(va, len)`**
(D-cache clean + I-cache invalidate; no permission change -- the exec alias is RX
from creation). Execute = branch `exec_va + off`. Un-emitted pages are zero =
AArch64 `UDF #0`, so an unpublished region traps rather than running residue.
Syscalls: `SYS_JIT_CREATE`=101 (CAP_JIT-gated), `SYS_JIT_DESTROY`=102 (ungated),
`SYS_ICACHE_SYNC`=103 (range-check-gated). `JIT_REGION_MAX` = 64 MiB. Wrapper:
`libthyla_rs::jit::CodeRegion` + raw `t_jit_*`.

**Why DOSBox is a CLEAN fit -- cleaner than a general DBT, needs NO kernel change:**
- **Incremental emit is free.** One big region (up to 64 MiB, far larger than
  DOSBox's few-MB code cache); bump-allocate blocks; pay **one `SYS_ICACHE_SYNC`
  per committed block**. Create/destroy amortize to ~zero.
- **SMC / re-emit is the mechanism.** Re-publishing IS invalidation (no separate
  inval syscall; the `jit-prover` re-emit leg proves it). Block-linking (patch a
  tail branch) = write the patch through the writer alias + `publish_range` over
  the patched bytes.
- **Same-thread emit-then-execute.** DOSBox emits and runs a block on the SAME
  thread, so the only cross-PE hazard (F2: a broadcast-ISB is a documented
  contract, bites only when emit-thread != execute-thread) is fully covered by
  the calling-PE ISB. No broadcast-ISB variant needed.
- **Software SMC detection.** DOSBox detects self-modifying DOS code in software
  (its own emulated-MMU write handlers), NOT via host page-write-protection +
  resumable faults -- so the designed-only "resumable faults" JIT caveat (not
  built at v1.0) does NOT block DOSBox.

**The integration work (DX-4):**
- Adapt DOSBox's ARM64 dynarec backend to **emit at `writer_va+off`** and use
  **`exec_va+off`** for the block entry pointer + any absolute code address it
  embeds (block-link targets, jump tables). Intra-block PC-relative branches are
  alias-agnostic (offset-identical in both aliases); only absolute code addresses
  need the writer->exec translation. This is exactly the ORC `DualMapMemoryMapper`
  `writerFor` split (`usr/ports/llvm/patches/0007-*` -- the C++ template).
- One `SYS_ICACHE_SYNC` per published block (the one irreducible cost -- the
  architecture requires a sync between write and fetch; batch where blocks emit
  together).
- Emit `bti c` at each indirect-branch-reached block entry; be PAC-aware on
  hardened silicon.
- **Acquire CAP_JIT at startup** via the corvus `jit` clearance -- elevation-only,
  stripped at every fork (no spawn-time grant); template `thyla_capjit.h` /
  `libthyla_rs::cap`. The user needs `jit`-clearance eligibility.

**Fallback: `core=normal`** (DOSBox's pure interpreter, no codegen) needs none of
this, is W^X-clean, needs no kernel change, and is the DX-2 first-light target.
DX-4 (the dynarec) is the performance follow-on.

**Audit surface:** DX-4 is a CAP_JIT consumer touching I-42 + I-12 (W^X-adjacent
-> prosecute hard); adds an AUDIT-TRIGGERS row. The KERNEL surface is unchanged
(no new syscall) -- the audit prosecutes the DOSBox-side integration's correct
USE of the proven surface: the writer->exec address translation, the per-block
publish, and the code-cache lifecycle. DX-4 gets a short focused design pass
against the ORC template at implementation time.

## 7. Sound -- fully stubbed (v1.0 non-goal)

Audio is a hard v1.0 non-goal (no virtio-sound driver; `VISION.md`). DOSBox is
sound-centric (PC speaker, SB16, AdLib/OPL, GUS, MIDI) -- ALL of it compiles out
to a null mixer, exactly as TyrQuake shipped `-nosound` (and had to NULL-guard
the no-sound path). This is the single biggest behavioral haircut; it is
precedented and clean. A future audio server + virtio-sound (post-v1.0) is what
would light sound up.

## 8. Arc structure (sub-chunks)

- **DX-0** -- this scripture (the arc scope). Lands as a scripture commit, no code.
- **DX-1** -- vendor DOSBox Staging pruned-pristine (`usr/ports/dosbox-staging/`)
  + license grep; get it to COMPILE + LINK via Pouch (libc++ + libSDL2.a +
  SDL_thylacine), `core=normal`, sound stubbed. Exit: a static ET_EXEC that links.
- **DX-2** -- FIRST LIGHT: stage into ramfs, boot in a tile, wire the file-I/O
  boundary-line (mount a host folder as a DOS drive), reach `Z:\>`, run a simple
  DOS program rendering in a Tapestry pane. Exit: a DOS program runs + an LS-CI
  gate asserts frames on the scanout (`ls-gfx-dosbox`, the `ls-gfx-quake` pattern).
- **DX-3** -- sound fully stubbed/hardened; input polish (keyboard + mouse for DOS
  games); config/autoexec; larger real programs.
- **DX-4** -- the **CAP_JIT dynarec** (I-42): wire the dynamic core to emit through
  CAP_JIT (the writer/exec split per 6); self-modifying-code via re-publish.
  AUDIT-BEARING (its own design pass against the ORC template + the focused
  audit). Exit: `core=dynamic` correct + measurably faster than `core=normal`;
  I-42/I-12 prosecuted clean.
- **DX-5** -- arc close: a recognizable DOS GAME runs end-to-end; focused audit;
  reference doc + user-manual entry; AUDIT-TRIGGERS row for the DX-4 surface.

## 9. Invariant / audit surface

- **I-42 (JIT-as-a-capability)** + **I-12 (W^X)**: the DX-4 dynarec is the surface.
  It must never make a page writable+executable (the kernel already guarantees
  this by construction -- the two aliases are distinct PTEs of one PA; DX-4 must
  simply USE the surface correctly), must publish via `SYS_ICACHE_SYNC`, and gets
  a focused audit. Adds an AUDIT-TRIGGERS row (a CAP_JIT consumer).
- DX-1..DX-3 are a userspace port (no new invariant); the audit floor is the
  suite + the LS-CI gate. New musl boundary-line patches follow the pouch audit
  discipline.

## 10. Risks

- **CAP_JIT fit -- RESOLVED (clean).** The as-built dual-map surface covers a
  single-process, same-thread, software-SMC dynarec entirely; no kernel extension
  needed. The one real cost is **one `SYS_ICACHE_SYNC` per emitted block**
  (irreducible; mitigate by batching co-emitted blocks). Was the biggest unknown;
  the JIT-mechanics review closed it.
- **C++ build friction**: the LLVM-fork gate (build.sh skips C++ without it);
  DOSBox uses meson/autotools -- we do a curated object build (manual for a big
  tree, TyrQuake precedent). The largest labor item.
- **File-I/O boundary-line**: DOSBox does heavy path-based I/O (disk images,
  drive mounts) -- expect new musl patches beyond Quake's.
- **SDL usage mismatch**: SDL_thylacine was proven for Quake's usage; DOSBox uses
  SDL differently (8bpp/palette modes, surface vs texture, mode changes) -- may
  surface backend gaps to fill.
- **Memory sizing** (stack/heap) -- TyrQuake precedent.

## 11. Naming (thematic -- propose, don't impose)

Keep the **DOSBox** name (a foreign port keeps its identity, like TyrQuake).
Candidate thematic name for the Thylacine-side DOS-emulation capability / the
emulated-machine tile, for the operator to weigh: **Cryptid** -- the
cryptozoology / Lazarus-species angle in the naming sources (software long
thought dead, sighted alive on Thylacine). Or leave it plain. Chunk prefix **DX**.

## 12. Exit criteria ("done")

DOSBox Staging runs on Thylacine as a Tapestry pane; mounts host folders as DOS
drives; runs DOS programs AND games (text-mode + VGA graphics); `core=dynamic`
via CAP_JIT for real speed (with `core=normal` as the always-works floor); audio
cleanly stubbed; the DX-4 CAP_JIT integration audited; reference + manual docs
landed. The DOS library is open to the user.
