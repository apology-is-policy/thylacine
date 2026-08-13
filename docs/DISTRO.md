# DISTRO — a stock Alpine rootfs runs `/bin/sh`

Status: DESIGN (scripture; both forks user-voted 2026-08-05). The ladder D-1..D-close
below is the plan of record — the earlier "D-0..D-close" label in task #166 was a
placeholder that named no design; this document is what it stood for.

Related scripture: `docs/VIVARIUM.md` (the container substrate this runs inside),
`docs/LINEAGE.md` (fork/execve — the L-6c gate is this arc's floor),
`docs/STALK-DESIGN.md` (the resolver D-1 extends),
`docs/EXEC-LOAD-DESIGN.md` / REVENANT (the file-backed paging D-3 generalizes).

---

## 1. The goal, and what "stock" binds

**A STOCK Alpine aarch64 minirootfs — the unmodified upstream tarball — is
installed as a vivarium bundle and `viv run` executes `/bin/sh -c 'echo ...'`
through it.**

"Stock" is load-bearing: nothing inside the rootfs may be patched, relinked,
flattened, or substituted. Every mechanism below must serve the binaries and
the loader Alpine actually ships, as shipped. That constraint is what makes
stock musl's own source the specification for half of this arc (§3.3).

The L-6c gate (LINEAGE) already runs busybox in a vivarium — but OUR bundle:
a statically-linked busybox at a direct path. A stock rootfs differs in exactly
two structural ways, and they are the arc's two blockers:

- **#146 — symlinks.** The rootfs carries 335 of them (measured at the task's
  filing), including `/bin/sh -> /bin/busybox` and the dynamic-loader path
  itself. `stalk` has zero symlink awareness: a symlink component today is an
  opaque leaf (traversal through it trips the #79 ENOTDIR gate; opening it
  fails server-side). Nothing in the rootfs is reachable.
- **#145 — dynamic linking.** Every stock Alpine ELF is PIE (`ET_DYN`) with
  `PT_INTERP = /lib/ld-musl-aarch64.so.1`. `elf.c:83` rejects
  `e_type != ET_EXEC`, so every binary in the rootfs is refused before its
  first instruction.

Non-goals of THIS arc (recorded seams, §8): apk and container networking,
an interactive tty inside the vivarium, device nodes beyond what diorama
serves, glibc distros, setuid (never at v1.0), per-exec PIE base
randomization.

---

## 2. Ground truth (what exists, measured 2026-08-05)

The symlink WIRE is fully built; only FOLLOWING is missing:

- `9p_wire.h:106-107,227,692` — `Tsymlink`/`Rsymlink` (16/17),
  `Treadlink`/`Rreadlink`, `P9_QTSYMLINK 0x02`, all built + parsed.
- `9p_client.c:1893` — `p9_client_readlink` (live; the phenotype `readlinkat`
  T2 shell consumes it since #66).
- Loom carries SYMLINK/READLINK payload opcodes (Loom-6b).
- Stratum stores + serves symlinks (the v2.x POSIX surface); `stratum-fs`
  has `symlink`/`readlink` CLI verbs. **The recursive `put` DOES recreate
  them** — read 2026-08-10 at D-4, closing what this line called an
  UNVERIFIED D-5 obligation: `src/cmd/stratum-fs/run.c:750,784-806` `lstat`s
  each child, `readlink`s an `S_IFLNK`, and calls `stm_9p_symlink`, with an
  explicit refusal (`:810-816`) rather than a skip when it cannot. Its own
  comment names the reason — "Alpine's `/bin/sh` IS one". So D-5's
  verify-then-extend reduced to VERIFY; the extend half was already built.
  **Both halves of the verify are now DONE (D-5, 2026-08-10).** In the POOL:
  `stratumd` on a clone of the baked `pool.img` shows `put` preserved every
  class — `/bin/sh -> /bin/busybox` absolute, `/etc/os-release ->
  ../usr/lib/os-release` relative, `/lib/libc.musl-aarch64.so.1` relative
  same-dir — with real files still real. In the GUEST: the ARC gate resolves an
  absolute pool symlink for exec (legs A and C) and a relative one for read
  (leg D). This closes the item; `stalk`'s symlink battery is no longer
  ramfs-only.
- `kernel/stalk.c`, `spoor.h`: zero symlink handling. No native
  SYS_READLINK/SYS_SYMLINK numbers exist.

The loader half:

- `elf.c:83` — `ET_EXEC` only. `elf.c:262-314` already PARSES `PT_INTERP` —
  as the V-1b brand diagnostic, consulted only on a FAILED load. D-4 upgrades
  it from diagnosis to dispatch.
- The auxv builder (`exec.c:548-555`) emits AT_PHDR/PHENT/PHNUM/PAGESZ/HWCAP/
  RANDOM/VDSO_CLOCK/NULL. **No AT_ENTRY** — needed for ET_DYN under either
  interpreter route. No AT_BASE — deliberately stays absent (§5.3).
- The phenotype memory rows (`vivarium.c:109-142,754-816`): mmap admits
  anon `PROT_READ|PROT_WRITE` only, `addr` ignored, MAP_FIXED refused,
  PROT_EXEC refused, fd-backed refused; mprotect is ENOSYS by construction.
- REVENANT (I-36): file-backed demand-paged R+X mappings with the qid-keyed
  Image cache — the exact machinery library text needs, today reachable only
  from exec.

Stock musl (vendored `third_party/musl` == Alpine's 1.2.x lineage):

- **Direct invocation as a program is supported** (`ldso/dynlink.c:1866` and
  the surrounding `__dls3` branch; the `ldd_mode` family) — the property the
  D-4 exec-rewrite rests on. Direct mode is SELECTED by `aux[AT_PHDR] ==
  ldso.phdr` (`dynlink.c:1834`), i.e. by the kernel having loaded the ldso as
  the image — which is exactly what the rewrite produces, so no auxv change is
  needed to reach it. Our `AT_PHDR` (`exec.c:655`, `seg->vaddr + (phoff -
  file_offset)`) and musl's `ldso.phdr` (`laddr(&ldso, e_phoff)` = base +
  `e_phoff`) are the same value for a segment-0-at-file-offset-0 PIE, which
  every stock ldso is.
- **The direct-mode option parser** (`dynlink.c:1868-1890`) accepts
  `--argv0 VALUE` (`replace_argv0` applied at `dynlink.c:2071`) and a bare
  `--` terminator, and it REWRITES argc in
  `argv[-1]` (`dynlink.c:1891`) — so the initial frame must keep argc
  immediately below `argv[0]` and both writable. All three are load-bearing
  for the D-4 argv shape (§7.1).
- **Its mprotect calls tolerate ENOSYS specifically** (`dynlink.c:855,1428`) —
  so the phenotype's `mprotect = ENOSYS` row survives dynamic linking
  unchanged. RELRO degrades (no loss vs status quo); aarch64 PIC has no
  textrels.
- **`map_library`'s sequence** (`dynlink.c:648,807,842,848`) is the D-3 spec:
  one whole-span fd-backed `MAP_PRIVATE` map at the FIRST load segment's prot
  (R+X), then per-segment `MAP_FIXED` fd-backed RW replacement INSIDE that
  span, then `MAP_FIXED` anon for the bss tail. The NOMMU fallback at :653 is
  compile-gated (`DL_NOMMU_SUPPORT`, off in Alpine builds) — do NOT design
  against it.

---

## 3. The two voted forks

### 3.1 Symlink resolution lives in `stalk` (kernel) — user-voted 2026-08-05

**Rejected: phenotype-only resolution loops.** Exec funnels through
`exec_resolve_from_namespace -> stalk`, so a phenotype-only resolver still
needs a special exec pre-resolve; every T2 shell (openat, newfstatat, execve,
readlinkat, ...) would carry a duplicate resolution loop; and native Thylacine
could never traverse an Alpine tree.

**Rejected: install-time flattening.** Modifies the rootfs (not stock),
copies diverge on write, and apk upgrades that rewrite symlinks break later.

**Why the Plan 9 deviation is smaller than it looks:** Plan 9 has no symlinks
by creed, but Thylacine's FS layer crossed that line long ago — Stratum
stores them, the 9P wire speaks them, Loom creates them, the phenotype reads
them. Only following was missing, and following is precisely where
Thylacine's resolution model is STRONGER than the POSIX systems that made
symlinks infamous: containment is per-Proc by construction (§4.2).
Native Devs never mint `QTSYMLINK` qids, so native behavior changes only on
dev9p trees that actually contain symlinks.

### 3.2 PT_INTERP execs via rewrite-to-ldso — user-voted 2026-08-05

The vivarium exec path, on seeing `PT_INTERP` in a `PHENO_LINUX` exec,
restarts resolution on the interpreter and shifts argv: the kernel loads
exactly ONE image per exec, ever; stock ldso then open()/mmap()s the program
itself (its supported direct-invocation mode, §2). This is the
Fuchsia/Genode userspace-loader shape with the redirect at the kernel exec
chokepoint.

**Rejected (recorded as the v1.x fidelity lift): in-kernel dual-image
PT_INTERP** (the Linux/gVisor model). Full fidelity — `/proc/self/exe`,
`AT_EXECFN`, `AT_BASE` — at roughly double D-2's loader scope, for fidelity
nothing in the `/bin/sh` gate needs.

**The fidelity ledger, re-measured at D-4 implementation (2026-08-10).** The
2026-08-05 vote recorded two accepted gaps and asserted one property. Reading
musl's direct-mode parser and fixing where the rewrite LIVES changed three of
those answers; the ledger below is the corrected one, and §7.1 carries the
shape that produces it.

| Surface | 08-05 vote said | AS-BUILT at D-4 | Why |
|---|---|---|---|
| `argv[0]` | "the app still sees its own argv[0]" | **HOLDS** — exactly | Not from the stated shape, which delivers the PATH instead. `--argv0` is what delivers it (§7.1). |
| `/proc/self/exe` | reports ldso (accepted gap) | **CLOSED** — reports the program | The rewrite lives INSIDE `exec_load_into`; the Proc-side stamps run in its two callers, which still hold the ORIGINAL `exe`. |
| `AT_EXECFN` | absent (accepted gap) | absent (unchanged) | We emit no `AT_EXECFN` in either route; musl's consumer is the kernel-loaded branch only. |
| `argc == 0` | not considered | becomes `argc == 1`, `argv[0] == ""` | Inherent: the ldso's own command line must name a pathname, so a zero-length vector cannot be expressed. Linux itself treats `argc == 0` as a hazard (the CVE-2021-4034 class). |
| mode `0111` (X, not R) | not considered | REFUSED at load | Inherent to a userspace loader: the ldso re-opens the program `O_RDONLY`. The kernel's own `OEXEC` gate still runs first, so this only ever SUBTRACTS reachability. Fuchsia and Genode have the identical property. |
| `argc >= 509` | not considered | REFUSED (a clean refusal, never truncation) | The rewrite spends 4 of the 512 argv slots, so a dynamic exec's ceiling is 508 where a static one's is 512. Same for an argv within ~40 bytes of the 64 KiB byte ceiling. Fail-safe by construction: the bound is checked before the frame builder, which would otherwise EXTINCT. |
| the program path | not considered | resolved TWICE (kernel peek, then ldso `open`) | Benign: the kernel's resolution only decides "this needs an interpreter"; the ldso's is what loads. A file swapped between them is caught by the ldso's own `map_library` validation, never by the kernel mapping the wrong bytes. |

`--argv0` and `--` bind D-4 to a musl ldso carrying both. Read out of the
vendored tree's own `WHATSNEW` rather than recalled: the `--`-terminated
option parser arrived in **musl 1.1.1** (`WHATSNEW:1221`, "new options
--preload and --library-path to dynamic linker") and `--argv0` in **musl
1.1.17** (`WHATSNEW:1788`). We vendor 1.2.5; the floor is eight years below
it. That is not a new constraint of substance — glibc distros are already a
recorded seam (§8) — but it is a CLI dependency rather than an ABI one, so it
fails LOUD (the ldso's usage text on stderr, exit 1) rather than silently.

---

## 4. D-1 — symlinks in `stalk` (#146)

**Audit-bearing: I-28 is REFINED, and this is a privilege boundary.**
No new invariant number: expansion is contained by the same machinery the
invariant already names, which is the design's central claim.

**STATUS: AS-BUILT** — kernel `f10b1675`, the live-FS gate + userspace ABI
mirror `b77581b6`. Six `stalk.symlink_*` kernel tests + the 24-leg boot-fatal
`/symlink-probe`. Two things the build changed about this design:

- §4.5's battery is one probe, not a joey-side one: it needs to CREATE links,
  and `SYS_WALK_CREATE` has no symlink mode, so creation goes through
  `LOOM_OP_SYMLINK` (this is that op's first consumer). Self-minted rather than
  baked, so #126's `PRESERVE=1` staleness cannot reach it.
- The containment proof is TWO legs. The chroot inversion alone does not
  discriminate the re-anchor — every root-based caller already resolves FROM
  the root, so storing the root over the base is a no-op there, and deleting
  it is green on every other leg and the whole unit suite. See
  `docs/reference/104-stalk.md` "The I-28 containment claim, and what proves
  it."

Audited independently (Fable 5) at `6ad9bbc3`: 0 P0 / 1 P1 / 3 P2 / 3 P3, all
fixed. Two corrected the RECORD rather than the code:

- **#181 is CLOSED, demonstrated by construction.** This section previously
  said `pounce_skip_one`'s failure mode was undemonstrated. It is not:
  removing the flag HANGS `stalk.symlink_follow`, because the `link_at == 0`
  arm pushes nothing, charges nothing, and resumes at the run's own start —
  and that arm is every final-component symlink follow on a `walk_attrs` Dev.
  The earlier revert probe reporting no loop never reached the built kernel.
- **#184 was a P1, not the P2 it was filed as**, and is fixed. Its safety
  argument — "a symlink fid is not `Tlopen`-able for byte I/O" — is false
  against the server we ship: Stratum's `h_lopen` refuses a link only under
  `O_TRUNC`. `sys_walk_open_handler` now gates on `QTSYMLINK`.

One behaviour §4.3 still states too strongly: `STALK_MOUNT` "never follows" is
true only WITHOUT a trailing slash — `SYS_MOUNT("/mnt/")` on a link follows it
and keys the mount on the target, matching Linux `mount()`. Per-Proc, so
contained (I-1); recorded rather than changed.

### 4.1 Expansion

When a resolved hop's qid carries `QTSYMLINK` and the component is not
being held back by a no-follow disposition (§4.3), `stalk` expands it:
clone-walk parent->link (one RPC), `Treadlink` via a new NULL-permitted
`Dev.readlink` slot (dev9p -> `p9_client_readlink`; every other Dev leaves it
NULL, so a symlink on a Dev that cannot read one stays an opaque leaf —
fail-closed, and unreachable today since only dev9p mints the qid), clunk the
transient fid (async-clunk), splice the target into the remaining component
stream, and continue through the SAME main loop.

That last clause is the soundness argument: expanded components are not a
second resolution path. They re-enter the loop that already carries the
per-component X-search, the #79 ENOTDIR gate, the #81 dot gate, the #82
trailing-slash gates, the #84 dot-X-search, and mount-crossing. The gate
family binds expansion BY CONSTRUCTION because expansion produces components,
not answers.

### 4.2 Containment (the I-28 refinement)

- An ABSOLUTE target re-anchors at the caller's own `root_spoor` — never a
  global root. A confined Proc's `/bin/sh -> /bin/busybox` resolves inside
  its container; the classic chroot-escape-by-absolute-symlink is closed by
  construction, not by auditing call sites (contrast the Linux CVE history).
- A RELATIVE target splices at the link's PARENT (the current trail
  position); `..` keeps the trail-floor clamp exactly as today.
- Bound: **40 total follows per resolution** (Linux parity; POSIX floor is
  8), exceeded -> `T_E_LOOP`. **T_E_LOOP = 40 (POSIX ELOOP) is a NEW errno
  registration — ERRORS.md is ABI-bearing, so D-1 carries a signoff item.**
- Length: each splice re-checks the effective bound (`SYS_OPEN_PATH_MAX`
  class); overflow fails clean (the #83 ENAMETOOLONG note applies — today a
  bare -1; the ER-x registration is separable).

### 4.3 Final-component dispositions (POSIX)

Follow: open (without O_NOFOLLOW), stat, chdir, exec, the mount SOURCE.
Do not follow: lstat (phenotype `AT_SYMLINK_NOFOLLOW`), unlink, rename (both
ends), readlink itself, the mount POINT (STALK_MOUNT's no-cross-final
precedent extends naturally). `open(O_NOFOLLOW)` on a link -> `T_E_LOOP`
(Linux semantics). A TRAILING SLASH forces following even for no-follow ops
(POSIX 4.13 — the #82 gates read the crossed quarry, so a followed
link-to-directory composes; the full truth table is a D-1 deliverable and
regression battery). Mechanically this is a no-follow-final FLAG on stalk,
not a new amode — but any new amode added later must still join the stalk F1
amode guard (standing obligation).

### 4.4 Interactions

- **POUNCE**: a `QTSYMLINK` mid-run SPLITS the fused run at that hop (the
  mount-mid-run split precedent); the resolver expands and re-enters. A
  fused record's qid.type is already returned per-hop, so detection is free.
- **Larder**: the dentry cache binds `(parent,name) -> qid` — that binding is
  to the LINK's qid and stays correct (expansion happens above the cache).
  A (link-qid -> target) sub-cache is a recorded seam, NOT built at D-1:
  correctness first, the per-crossing readlink RPC is the honest v1.0 cost.
- **Phenotype rows**: openat threads O_NOFOLLOW; newfstatat's
  `AT_SYMLINK_NOFOLLOW` becomes real (today vacuous — no link ever resolved);
  readlinkat is already live. Native surfaces: none added (no SYS_SYMLINK /
  SYS_READLINK at D-1; creation exists via Loom for whoever needs it; a
  native pair is a seam).

### 4.5 Gate + coverage

A joey boot-fatal probe on the pool: chain-walk (link -> link -> file),
absolute-target re-anchor under a chroot, ELOOP at the bound, stat-vs-lstat
divergence, trailing-slash-on-link-to-dir, unlink-removes-the-link-not-the-
target. Each leg revert-probed (the #79-#84 battery discipline). Focused
audit round (I-28 surface).

---

## 5. D-2 — ET_DYN load + AT_ENTRY

`elf.c` accepts `ET_DYN`; all `p_vaddr` become base-relative; the loader
places the image at a fixed default bias chosen against the exec VA plan
(constant picked at D-2 with the map in front of us; per-exec randomization
is a recorded I-16-adjacent seam). The REVENANT path is unchanged — the
Image cache is content-keyed on `(qid, page index)`, so text pages are
base-independent and a PIE's text is shared across differently-based
instances for free.

Auxv grows **AT_ENTRY** (base + `e_entry`); `EXEC_INIT_AUXV_COUNT` bumps
with the CF-4 frame-reservation precedent. **AT_BASE stays absent** — it is
the dual-image protocol's tag, and emitting it without the dual load would
lie to ldso.

**CORRECTION (measured at impl 2026-08-06, task #186).** This section used
to say "musl's direct-invocation discrimination reads the entry," flagged
for verification against `dynlink.c`. Verified: it is **false**. The
discriminator is `aux[AT_PHDR] != (size_t)ldso.phdr` (`ldso/dynlink.c:1834`),
where `ldso.phdr` comes from ldso's own SELF-RELOCATED base (`:1733`), not
from auxv. AT_ENTRY is *written* by ldso inside that branch (`:1914`) and
read only at the final `CRTJMP` (`:2075`), so on the direct-invocation path
it never reads ours. Both directions were then confirmed by sabotage, not
just by citation:

| PIE-path-only sabotage | stock-ldso gate | unit suite |
|---|---|---|
| AT_ENTRY forced to 0 | **still PASSES** | 1363/1363 |
| AT_PHDR forced to 0 | **FAILS at D2-A** | 1363/1363 |

So what the gate actually rests on is that a directly-exec'd ldso's AT_PHDR
equals its own `base + e_phoff` — which the existing AT_PHDR computation
yields for free once vaddrs are biased. AT_ENTRY is still emitted: it is the
standard SysV tag, `getauxval(AT_ENTRY)` answers it, and the v1.x dual-image
lift (§3.2's recorded alternative) would need it to name the PROGRAM's entry
while AT_PHDR named the program's phdrs.

Gate: Alpine's stock `ld-musl-aarch64.so.1`, exec'd directly with no
arguments inside the Alpine vivarium, prints its usage line — a REAL foreign
ET_DYN image loading, before any mmap work exists. Boot-fatal via two joey
markers (the usage-arm string AND the `musl libc (aarch64)` identity, so a
garbled load that still produced output cannot satisfy both).

**SEAM (user-dispositioned 2026-08-06, task #188): no static-PIE hello.**
This section also asked for one from our toolchain. Measured: `-static-pie`
is silently DROPPED by the fork's `aarch64-thylacine` driver (it hard-codes
`-static` + `crt1.o`; the flag is reported "unused during compilation" and
an ET_EXEC comes out at rc=0), and driving `ld.lld` directly with `-pie` +
`rcrt1.o` fails on `R_AARCH64_ABS64 cannot be used against local symbol` —
the pouch `libc.a` is built non-PIC. Unblocking it needs a PIC rebuild of
the pouch musl (re-codegens every pouch binary, its own audit surface) plus
a Clade-track driver change. It is also strictly WEAKER evidence than the
ldso gate, which exercises an AT_PHDR path a static-PIE never touches, and
nothing in D-3/D-4/D-5 needs our toolchain to emit PIE.

Audit posture: audit-noted (the loader row extends; no new invariant — the
W^X/segment gates are byte-identical, only the base moves). AS-BUILT: the
bias enters at exactly one place, `elf_load`'s segment loop, so every
downstream consumer reads FINAL addresses; `elf.pie_load_bias` pins both
halves of that claim, the ET_DYN one and the ET_EXEC control.

---

## 6. D-3 — file-backed EL0 mmap (the arc's audit-heavy heart)

**Audit-bearing: I-36 GENERALIZES and I-12 is on the line.** The domain is
not designed — it is MEASURED off stock ldso's `map_library` (§2), the V-2d
measured-off-the-binary discipline applied to the loader:

**Split into sub-chunks at implementation:**

| Sub-chunk | Delivers | State |
|---|---|---|
| D-3a | arm 1 (the whole-span fd-backed R / R+X map) + the #190 fix | **AS-BUILT** |
| D-3b | arms 2+3 (MAP_FIXED split/replace: fd-backed RW eager copy, anon tail) | **AS-BUILT** |
| D-3c | the gate (#189) + the #194/#193/#199 fixes + the #192 verdict + the FOCUSED round (F1 [P1] deferred-free fix) + reference docs | **AS-BUILT** |

**Three corrections the build forced, recorded before the code:**

- **The gate below was impossible as written (#189).** `ld-musl /bin/echo hi`
  cannot work: measured against the staged rootfs, `/bin/echo -> /bin/busybox`,
  and Alpine's busybox is **ET_EXEC with no PT_INTERP and no PT_DYNAMIC**.
  `map_library` refuses it twice (`if (!dyn) goto noexec`, and the ET_EXEC arm
  at `dynlink.c:816` demands the map land exactly at `addr_min`). Only SIX
  dynamic interp-carrying binaries exist in the whole rootfs — `getconf`,
  `getent`, `iconv`, `scanelf` (libc only), plus `ssl_client` and `apk`.
  **The gate is now `ld-musl-aarch64.so.1 /usr/bin/getconf PAGESIZE` -> `4096`**,
  whose output is a real end-to-end assertion (musl's `sysconf(_SC_PAGESIZE)`
  reads the `AT_PAGESZ` our exec builds). CORRECTED AT D-3c: the earlier claim
  that this "re-crosses D-1's symlink" (`libc.musl-aarch64.so.1 ->
  ld-musl-aarch64.so.1`) is FALSE — measured off the vendored `dynlink.c`,
  getconf's libc DT_NEEDED short-circuits **by name** in `load_library`'s
  reserved list (`"c.pthread.rt.m.dl.util.xnet."`) and never touches the
  filesystem. The gate maps exactly TWO objects: ldso (the kernel's D-2 ET_DYN
  exec) and getconf (ldso's `map_library`, through the D-3 phenotype arms).
- **arm 2 also carries R and R+X segments, not only RW — but NOT on this
  rootfs.** `dynlink.c:842` MAP_FIXEDs *every* PT_LOAD whose page-floored vaddr
  differs from `addr_min`, at that segment's own prot. At D-3b this was measured
  across **every ELF in the staged rootfs, not just the libc: all 18 are exactly
  `R-X` then `RW-`**, so every arm-2 request there is RW and the R / R+X paths
  have NO producer. They need a `-z separate-code` four-segment layout (binutils
  >= 2.31, so Debian/Fedora); Alpine's toolchain does not emit one. D-3b builds
  and unit-tests them, and does NOT claim gate coverage for them.
  The same census measured **zero page-rounded PT_LOAD overlaps** (so the
  wholly-inside-one-VMA rule holds) and the arm-2 eager-copy cost: **888 KiB**
  summed over every library at once, **372 KiB** for the largest single one
  (libcrypto.so.3), **16 KiB** for the ld-musl a typical dynamic process maps.
- **The whole-span map deliberately overshoots EOF.** For the shipped libc,
  `map_len` is 0xc3000 against a 0xb0a58-byte file (musl's own comment: "we map
  too much, possibly even more than the length of the file"). The fault arm's
  EOF-tail-stays-zero behaviour is therefore load-bearing here, not an edge.

**One invariant retired (#190), pulled forward into D-3a as a dependency.**
The R-5 audit filed the FILE fault arm's cached `freq->slot` as F2
[P3, *unreachable at v1.0*] on the premise that a FILE Burrow "is created only
by exec, mapped exactly once at burrow_offset 0". D-3 is that path on every
count, so the premise is retired rather than re-argued. Note F2's prescribed
remedy — "recompute slot from `vma->burrow_offset`" — is **wrong**:
`freq->file_offset` came from the same pre-sleep geometry and the page already
holds those bytes, so recomputing only the index files stale bytes under a fresh
slot. Both install paths now **verify** the geometry and bail; a re-fault
re-resolves. Regression: `demand_page.file_geometry_shift_bails{,_single}`,
one per install path, each revert-probed against its own check.

1. **fd-backed `MAP_PRIVATE`, `PROT_READ|PROT_EXEC` (or PROT_READ alone),
   no MAP_FIXED** — the whole-span initial map. Rides a `BURROW_TYPE_FILE`
   Burrow demand-paged through the Image cache: library text is deduped
   machine-wide across every container by existing machinery (the arc's
   headline composition). The I-36 conditions are checked at map time; the
   `exec` component of the Image key (#45) carries.
2. **fd-backed `MAP_PRIVATE`, `PROT_READ|PROT_WRITE`, MAP_FIXED wholly
   inside a caller-owned mapping** — the data-segment replacement. A private
   EAGER COPY at map time (segment-sized, bounded, I-32-charged); never
   shared, never written back. This keeps I-36's ban intact in substance:
   **no userspace WRITABLE file mapping exists** — a writable request
   terminates in private anonymous memory holding a copy of the file bytes,
   condition (4)'s shape.
3. **anon `MAP_FIXED` wholly inside a caller-owned mapping** — the bss tail.
   The lazy-anon arm at a fixed VA.

MAP_FIXED admission rule (**CORRECTED at D-3b, #196 — the design's rule was
one shape short**): the target is either (a) `[addr, addr+len)` lying WHOLLY
inside ONE existing VMA owned by the caller, replaced atomically under
`vma_lock` (split + replace), or (b) an entirely FREE range, plain-mapped.
Shape (b) is not a convenience: Linux MAP_FIXED does not require the target to
be mapped already, and omitting it made an unmapped-address request answer
**ENOMEM** — a WORSE reply than the ENOSYS it replaced, because ENOMEM cannot
be told apart from real memory pressure and an allocator reads it as OOM. The
residual divergence is the third shape (spanning two VMAs, or partially
overlapping one), which Linux serves by unmapping the overlapped part and which
we refuse because partial unmap is post-v1.0. Everything else about MAP_FIXED
stays refused — MAP_FIXED_NOREPLACE included. `addr` without MAP_FIXED stays
ignored.
`PROT_WRITE|PROT_EXEC` stays refused unconditionally; anonymous PROT_EXEC
stays refused (I-42/CAP_JIT untouched). **mprotect stays ENOSYS** — the
measured musl tolerance (§2) is what makes that survivable, and it is
re-verified as a D-3 regression (the vendored-source line, not an assumption).

The I-36 statement's "kernel-internal, never a userspace writable file
mapping" RELAXES to "kernel-internal OR a read-only/exec phenotype mmap;
writable file mappings stay banned" — the ARCH section 28 row carries this
in the same scripture commit as this document.

Prosecution list (the D-3 focused round): W^X completeness (no path mints
W+X or anon-exec); MAP_FIXED containment (wholly-inside-own-VMA; no
cross-VMA clobber; the split/replace is atomic vs a faulting sibling
thread); fd rights + lifetime (the Spoor pinned across the blocking read;
#844 discipline); death-interruptible page-ins from mmap-time (the I-36
condition 5 now reachable from a new entry); partial-failure rollback (a
half-built multi-segment library map unwinds fully); I-32 accounting
(private copies charged; demand-paged FILE pages keep the R-5 uncharged-at-
v1.0 posture, documented). Native scope: NONE — this is a phenotype row
over shared kernel core; no native mmap API is added.

**D-3b as-built notes.**
- The writable arm-2 request is served by an eager private copy because musl
  **writes into that mapping** before arm 3 runs: `dynlink.c:849` memsets the
  partial tail page. A writable private FILE mapping would need copy-on-write
  over shared Image-cache pages, where a bug leaks one container's writes into
  another's view of the same library. The eager copy is a conforming
  MAP_PRIVATE (POSIX and Linux both leave post-mmap file changes unspecified
  for private mappings) and is the FULLY CHARGED path — `burrow_lazy_populate`
  takes the I-32 page charge for the whole run up front, unlike the read-only
  arm (task #194).
- Note arm 2's length comes from `p_memsz`, not `p_filesz`, so it maps past the
  file's data and arm 3 then overlays the whole bss pages. The double-charge is
  the bss tail only — 12 KiB of libcrypto's 372 KiB.
- PROT_NONE declines on BOTH fixed arms, and on the anon one that deliberately
  diverges from the non-fixed anon arm's documented degrade-to-writable: a
  FIXED PROT_NONE over an existing mapping is a GUARD, and answering it with a
  writable page is a hole rather than a degradation. Measured, it costs nothing
  (arm 3 fires only under PF_W, so its prot always carries R|W).
- The split primitive is NEW code — `burrow_map` has no `burrow_offset`
  parameter and `burrow_unmap` demands an exact `(vaddr, length)` match. It
  REUSES the old `Vma` as the surviving remainder (shrunk in place, never
  removed) so no address-space hole can exist on any failure path; only the
  exact-cover case removes, and there the restoring re-insert is provably
  infallible (same lock hold, into the range just vacated, count strictly below
  its entry value).
- **D-3b is the first producer of a non-zero `vma->burrow_offset` in the tree.**
  Every other `vma_alloc` call passes a literal 0 or copies one that can only be
  0. The survivors' byte identity (`burrow_offset + (va - vaddr_start)`) is
  invariant across the cut, which is also what makes D-3a's #190 post-sleep
  geometry check come out right against a concurrent split: it passes exactly
  when the bytes read before the sleep still belong at that slot.

**D-3c as-built notes.**

- **The gate (#189) is LIVE and boot-fatal.** It rides the L-6c Alpine gate
  script (`tools/build.sh`, pool-resident) as marker `D3-A-getconf-pagesize-4096`
  in joey's leg list: the script runs
  `/lib/ld-musl-aarch64.so.1 /usr/bin/getconf PAGESIZE` and emits the marker
  only when the run's OWN rc is 0 AND its captured stdout is exactly `4096` —
  output only the success path can produce. The raw BEGIN/END block above it is
  diagnostics. The FIRST run of the raw block was green end-to-end (ldso mapped
  getconf through all three phenotype arms and printed 4096); the marker leg
  failed on its own instrumentation — `2>/dev/null` — which surfaced **#201**:
  a shell write-redirect passes `O_CREAT`, and `vivarium_openat_decide` refuses
  `O_CREAT` even on an existing file, so every `> file` in a container breaks
  (a D-4/D-5 blocker, tracked). The gate line now uses the proven `2>&1` dup,
  which also tightens the assertion (stderr pollutes the exact match).
- **#194 FIXED (the D-3a P1): the fault arm is now Linux-faithful past EOF.**
  Every Image-cache Burrow carries `file_limit`, sampled at creation
  (`spoor_file_size`): the guest-facing mmap arms REFUSE to map without it
  (fail-closed, `-EIO`; a hostile near-2^64 size is excluded by the same
  predicate); exec admits `BURROW_FILE_LIMIT_UNKNOWN` only because the sole
  size-less backing Dev is the baked, immutable ramfs. The fault arm answers
  a page wholly past `round_up(file_limit)` with `FAULT_USER_BUS` (Linux
  SIGBUS) BEFORE any allocation — nothing is minted, so the uncharged-FILE
  posture is again justified by what it was always justified by: real, shared
  file bytes. Read-ahead clamps its cluster at the limit so no resident zero
  page can serve the fast path unchecked. The file's partial last page still
  zero-fills (the read-short path). This also closes the lying-ELF variant
  through exec on any stat-answering FS.
- **The EOF-tail scripture sentence was WRONG about which arm.** Measured off
  the staged binaries (getconf: file 0x13F90, arm-2 window [0x10000,0x21000)):
  every wholly-past-EOF page of the whole-span map is OVERLAID by arm 2 before
  any touch, on BOTH gate objects — so the FAULT arm's EOF-tail-zero was never
  load-bearing for the real path (and could not be: musl runs on Linux, where
  those pages SIGBUS). The load-bearing EOF-zero is the EAGER arm's short-read
  (arm 2 reads past the file end into its charged private copy), unchanged.
- **#193 FIXED:** both mmap-family success paths now `burrow_unref` OUTSIDE
  `as->lock` (the FILE arm and its pre-existing `attach_lazy` twin), removing
  the non-local "cannot be the last ref" argument; failure paths already did.
- **#199 FIXED (phenotype-only): whole-span munmap over split VMAs works.**
  `sys_munmap_range_for_proc` detaches every VMA wholly inside the range (each
  one WHOLE — not partial unmap), succeeds on an empty range (the Linux no-op),
  and refuses ATOMICALLY on a boundary straddle or a CODE region, via a
  validation pass under one lock hold. The per-VMA accounting body is the
  factored `detach_one_locked` the exact syscall also uses, so the I-32 refund
  logic exists once. `unmap_library`'s error path and dlclose now tear down.
  The NATIVE `SYS_BURROW_DETACH` keeps exact-match — Linux semantics belong to
  the phenotype row, and the native ABI does not move under a phenotype chunk.
- **#192 VERDICT: document, do not enforce.** File-backed `PROT_EXEC` mmap
  keeps requiring READ authority only — no X-bit check — because (a) it is the
  Linux semantic (the x bit gates execve, not mmap; noexec is a mount option we
  lack), (b) Debian Policy section 8.1 ships shared libraries 0644, so an X-bit
  gate would break every Debian-shaped rootfs at D-5 while Alpine's 0755 libs
  never exercise it, and (c) the authority argument holds: the caller could
  already read the bytes (pread) and already run them (exec-from-namespace);
  only "in THIS address space" is new, and it is confined to the caller's own.
  The cost is that I-42's "only executable-memory path" wording narrowed — ARCH
  section 6.6 now enumerates the three fixed-permission paths (exec, file-backed
  R+X phenotype mmap, CAP_JIT) instead of claiming one. The #201 narrow fix
  (open-if-exists) preserves the "phenotype cannot create files" premise this
  verdict leans on; the FULL create fix must revisit #192. The FOCUSED round is
  instructed to attack this verdict rather than inherit it.

  **CORRECTED AT D-CLOSE (the ARC ROUND attacked it, as instructed, and one
  third of it fell).** (a) and (b) STAND -- the Linux semantic and Debian
  Policy 8.1's 0644 shared libraries are both real, and (b) is why D-close does
  NOT bolt an X-bit check onto this arm. **(c) IS FALSE, and it is false in
  exactly the case that matters.** "The caller could already run them
  (exec-from-namespace)" holds only for a file the caller has X on:
  `exec_resolve_from_namespace` walks with `STALK_OPEN`/OEXEC, so exec REFUSES a
  non-executable file. This arm refuses nothing -- `sys_mmap_file_for_proc`
  looks the fd up with `RIGHT_READ` alone and contains **zero** `perm_check`
  calls (measured across `syscall.c:4854-4960`), then installs
  `VMA_PROT_READ|VMA_PROT_EXEC`. So R+X mmap runs bytes that exec would decline.
  That is not "the same authority in this address space"; it is authority exec
  does not grant, and it is precisely what I-42 exists to gate.

  The reachability half is worse than the verdict assumed. This entry's own
  premise is narrowly TRUE -- the phenotype cannot CREATE files (#201 refuses
  `O_CREAT`) -- but it does not protect, because the phenotype can WRITE an
  existing one: `VIV_LINUX_WRITE -> SYS_WRITE` is a **tier-1 direct row**
  (`vivarium.c:149`) and `openat O_RDWR -> VIV_OMODE_RDWR` is translated
  (`vivarium.c:532`). So `openat(F, O_RDWR)` + `write(shellcode)` +
  `mmap(F, PROT_READ|PROT_EXEC)` + jump is native code execution with no
  `CAP_JIT`, using only DAC authority over F. **The closed-list record widened
  the premise to "phenotype cannot create/WRITE files"** (`audit_d3_closed_list`
  F4), and the D-4 round then re-affirmed #192 on that widened form -- so the
  false clause lived in the audit memory, not here, and two successive focused
  rounds inherited it because each was scoped to a commit that did not contain
  the `SYS_WRITE` row that falsifies it.

  **What actually stands between this and exploitation is one bundle's DAC.**
  `root.readonly: true` in the shipped manifest is parsed and DISCARDED
  (`usr/viv/src/main.rs:210-213`: "not acted on at v1.0: there is no read-only
  bind flag, so the FS permission model (a SYSTEM-owned bake vs a non-SYSTEM
  invoker) is the enforcement"). That is real for OUR bake and does not survive
  a user-supplied bundle -- the entire point of a container runtime -- where the
  attacker authors the image and ships a 0666 file, nor a writable mount.

  **DISPOSITION (user-voted 2026-08-10): correct the record now, enforce via
  per-mount `noexec` later.** Not an X-bit check, because (b) still stands and
  it would break every Debian-shaped rootfs. `noexec` is what Linux itself
  gates `PROT_EXEC` on (`path_noexec`), it survives 0644 libraries, and it
  re-couples #192 to a kernel mechanism instead of a bundle's file modes -- the
  capability-microkernel SOTA agrees on the principle if not the shape (Zircon
  requires the vmex resource to mark a VMO executable; seL4 and Genode make
  execute a mapping right granted by capability). Owed as its own named chunk.
  **Until it lands, this hole is OPEN for any bundle we do not bake ourselves,
  and that is stated here rather than papered over.**

  **The tripwire is rewritten because the old one could never fire.** It read
  "the instant D-4/D-5 grants file writes, revisit #192" -- but the writes were
  granted before the arc began, so it watched a door that was already open. It
  now reads: **#192 is re-opened by the `noexec` chunk, or by any change that
  admits a new `PROT_EXEC` file-mapping shape, whichever comes first.** A
  tripwire keyed to an event that already happened is not a guard; it is a
  comment that looks like one.

  **RESOLVED at #217 (`MNOEXEC`), and the research moved the shape of the fix
  twice before any code was written.** Two facts measured while designing it
  are load-bearing and neither was known when the disposition was voted:

  1. **The container root is a `chroot`, not a mount** (`usr/viv/src/main.rs`
     `t_chroot(rootfs)`), and it is reached through the same dev9p session as
     the host root -- same `(dc, devno)`, no `PgrpMount` entry of its own. **No
     per-mount flag can describe it**, so the mechanism as named could never
     have covered the surface the finding named. Worse, it could not have
     covered it even in principle: the container root is the tree busybox must
     `exec` FROM, and a `noexec` root is a container that cannot start. A flag
     that must be off wherever the threat lives is not a fix for that threat.
  2. **The escalation the finding described does not exist for the bundle it
     described.** `VIV_LINUX_EXECVE` is a served tier-2 row (`vivarium.c:308`),
     so a bundle that owns its own tree can already overwrite one of its own
     0755 binaries and `execve` it -- arbitrary code as itself, without
     touching mmap. Against a self-owned tree the R+X map is a second door to
     a room the attacker is already standing in.

  So the fix lands where a real delta remains, and the claim is trimmed to
  match. The target is the case where `exec` would have **refused** and mmap did
  not: a writable, readable, non-X file on a surface the SYSTEM *hands* the
  container. `/env` is the live instance -- measured: entries are
  `T_S_IFREG | 0644` (no X bit, so `exec` declines them), `.read` and `.write`
  both present, `perm_enforced == false`, `devenv_stat_native` reports a real
  size, and `ENV_VALUE_MAX` is **4096**. A full page of attacker-chosen bytes,
  written and then mapped executable, on a file `exec` itself refuses.

  **IT TAKES TWO MECHANISMS, AND THE FIRST DRAFT SHIPPED WITH ONLY ONE.** The
  #217 round (Fable 5) found as F1 [P1] that `MNOEXEC` is **structurally
  incapable of covering `/env`** -- the exact surface named above. `MNOEXEC` is
  consulted through the `(dc, devno)` a file shares with its mount source, which
  assumes `devno` names the mounted INSTANCE; `devenv_walk` instead stamps the
  **calling Proc's** env devno (an I-1 fix: entry ids restart at 1 per Env, so
  without it two Procs' variables claim to be the same file and the REVENANT
  Image cache serves one the other's bytes). A container's `/env` files
  therefore never match the `/env` mount source `viv` installed, no entry
  matches, and the R+X map was admitted. The scripture asserted a door was shut
  while it stood open -- **the D-close closed-list lesson recursing one level
  down: a claim of closure that reads as a guarantee it does not make.**

  What actually closes it is a FLOOR beneath the flag: **`Dev.may_back_exec`**,
  an allowlist on `struct Dev` set by exactly the two Devs that serve real file
  content (`devramfs`, `dev9p`). Every other Dev leaves the zero default and is
  refused outright -- an environment variable is not code, and neither is a
  `/proc` field, a `/srv` endpoint or a console, mounted or not. Allowlist and
  not denylist deliberately: a Dev added later is refused until it opts in.
  `MNOEXEC` remains the refinement ABOVE that floor, for real filesystems where
  the mount, not the device, is the right granularity. Every mount `viv` makes
  is `MNOEXEC`; the only tree that stays executable is the chroot'd root, which
  is also the only tree the container must run from. **What remains open, and is documented rather than claimed closed: a
  bundle that owns its own executable tree can still author its own code.**
  That is not a boundary crossing (see fact 2), and making it impossible is a
  bundle-STAGING problem -- the rootfs must not be writable by its own
  container -- not a mount flag's job.

  **`MNOEXEC` IS OWNER-REVISABLE, AND THE THING THAT ACTUALLY CONFINES A
  CONTAINER IS THE PHENOTYPE GATE (#221).** Both halves are recorded here
  because the section above, read alone, invites exactly the wrong inference.

  Coverage is derived from the **current** mount table (`mount_noexec_covers`
  is an ANY-scan over it), and that table is unprivileged-mutable by its owner.
  So the restriction is revisable by the Proc it restricts, three ungated ways:
  **unmount**; **`MREPL`**, which replaces the entry wholesale; and -- since
  #219 -- the **converged idempotent re-mount**, `kernel/territory.c:811`
  (`territory->mounts[i].flags = flags`). Two of the three PRE-DATE #219 and are
  not created by its fix. The reasoning is already argued at the code site
  (`kernel/territory.c:788-802`), which states outright that a mount flag "is
  not [a lock], deliberately"; this paragraph exists so scripture agrees with it
  instead of leaving the point discoverable only by reading `territory.c`.

  **That is not the container's security story, and the distinction is the whole
  point.** A containered Linux guest cannot reach any of the three, because it
  cannot call `mount(2)` or `umount2(2)` at all. VERIFIED on this tree: the
  vivarium's `linux_nr` values are an enum in
  `kernel/include/thylacine/vivarium.h`, and it has **no member for Linux nr 39
  or 40, and no `VIV_LINUX_*MOUNT*` member anywhere in `kernel/`** -- so
  `viv_linux_dispatch` falls to its `default:` arm, which reports
  `"not in the table at all"` and answers `-T_E_NOSYS`
  (`kernel/syscall.c:11130-11133`). Fail-closed, by absence rather than by a
  check that could be edited. Container confinement therefore rests on **what
  the phenotype serves**, not on `MNOEXEC` being unstrippable -- and a reader who
  believed the latter would be trusting the weaker of the two.

  Two things that look like defects and are not, stated so this does not read as
  a to-do list:

  - **`SYS_MOUNT` / `SYS_UNMOUNT` having no capability gate is correct Plan 9.**
    A per-process namespace edit confers no authority the caller did not already
    hold; it rearranges names it can already reach. Gating it would be a
    departure from the model, not a fix.
  - **I-12's PROVENANCE wording needs no correction.** It states what the CHECK
    requires -- a Dev with `may_back_exec` AND a mount not marked `MNOEXEC` --
    and never claims the mount table is immutable. It is accurate as written.

  **The lock is NOT built, and the trigger is a caller that does not exist yet:
  the first confined NATIVE Proc that owns a namespace.** Every container today
  is a Linux phenotype, which the paragraph above already fences. A lock also
  needs a "locked by WHOM" axis Territory has no notion of, and designing that
  without a concrete caller risks designing the wrong one. When the trigger
  arrives, the shape (a pickup, not a re-derivation): a `PgrpMount.locked` bit
  set only when the mounter holds a capability the target lacks, honoured by
  unmount, `MREPL`, and the #219 converge. That is Linux's `MNT_LOCK_NOEXEC`
  answer -- restrictive flags lock on inheritance into a less-privileged
  context, the owner may ADD restrictions but never remove -- i.e. **monotone
  reduction, already our idiom on every other authority axis** (I-2
  fork-grantable caps, I-6 handle rights). Scripture-first: an ARCH change
  before any code.

  CONSIDERED, NOT CHOSEN: executability as a monotonically non-increasing right
  on the Spoor/handle (`ZX_RIGHT_EXECUTE`; Zircon and Genode both land here).
  Cleanest long-run model and the best fit with I-6 -- there is no mount table
  to edit at all -- but a substantially larger change across the handle surface.
  Reconsider if that surface is being reworked for another reason.

  The reachability is also narrower than the finding assumed, which is worth
  recording because it bounds the severity honestly:
  `sys_mmap_file_for_proc` has **exactly one caller** (`syscall.c` `VIV_LINUX_MMAP`),
  so this arm is PHENO_LINUX-only. A native Thylacine Proc has no syscall that
  reaches it, and CAP_JIT is not bypassable this way outside a vivarium.
- **#198 DISPOSITION: documented, not fixed.** The eager arm's up-to-65536
  serialized 9P reads in one syscall are bounded (BURROW_ATTACH_MAX), fully
  charged, and death-interruptible by inheritance from the dev9p read — so a
  kill unwinds the train. Measured need is ~93 reads for the largest real
  library (372 KiB). The cure, when one is needed, is the Larder read path /
  bulk staging, not a bespoke loop here.
- **The FOCUSED round (Fable 5) found F1 [P1], FIXED: the sleeping FILE-burrow
  free ran under `as->lock`** — the teardown twin of #193. A FILE Burrow's free
  reaches `spoor_clunk` -> (last 9P session ref) a synchronous Tclunk that MAY
  SLEEP; detach / munmap-range / vma_drain all freed under `as->lock`, which is
  the lock-across-sleep extinction. FIX = a deferred-free stack
  (`burrow_release_mapping_deferred` drops the mapping ref under v->lock,
  returning the dead Burrow without freeing; `burrow_free_deferred` frees after
  the unlock; the range munmap collects the chain across ONE lock hold, so the
  straddle-refusal atomicity is preserved, then frees after). Guest-reachable by
  D-3 (the first detachable sleeping-free Burrow at a guest address) but a
  pre-existing STRUCTURE. Regression = a stub Dev `.close` recording
  `current_thread()->preempt_count` at free time (asserts 0; the revert-probe
  reddens both legs at 1383/1385). **DIRTY CLOSE** — a P1 returned + an invasive
  teardown restructure, so a follow-up re-audit round is owed on the fix
  (ideally a non-Fable reviewer — this round was Fable-on-Fable). The three P3s
  (F2 file_limit cache-key coverage, F3 the corrected split-vs-fault comment, F4
  the #192/#201 D-4 tripwire) are recorded, not fixed.
- **The dirty-close re-audit (Opus, non-Fable — the F1 code was Fable-authored)
  found F5 [P1], FIXED: F1 was INCOMPLETE.** It deferred the sleeping FILE free at
  THREE teardown sites but missed the FOURTH -- `vma_replace_range_in`'s exact-cover
  arm (`vma.c`), reached under `as->lock` via `MAP_FIXED`
  (`sys_mmap_fixed_file_for_proc` / `sys_mmap_fixed_anon_for_proc` ->
  `burrow_map_fixed`). A `MAP_FIXED` that EXACTLY covers an existing FILE mapping
  freed that mapping's Burrow INLINE under the lock -> the same `spoor_clunk`
  lock-across-sleep extinction, guest-constructible (the bypass FILE Burrow at
  `{h:0,m:1}`: >128-image-cache-fill + a 9P file mmap). This is the
  "fix-on-site-N stops you asking about site-N+1" trap. FIX = thread a deferred
  Burrow out-param through `vma_replace_range_in` / `burrow_map_fixed_in` /
  `burrow_map_fixed`; the exact-cover `vma_free(old)` becomes `vma_free_deferred`
  handed back to the two `MAP_FIXED` arms, which free it past the unlock (the F1
  pattern). Also F6 [P3]: `detach_one_locked`'s `out_free ? &tf : NULL` ternary
  left a dead-today inline-free-under-lock path -- made always-defer (mandatory
  out_free, extinct on NULL). Regression = `burrow.map_fixed_replace_file_frees_
  outside_lock` (the F1 discriminator applied to the replace path; revert-probe
  reddens ONLY it at 1385/1386, F1's two legs stay green). Pre-existing #205 noted
  (the replace does not uncharge the old burrow's I-32 page_count -- over-charge,
  benign direction).
- **The F5-fix re-audit (Fable, the standing primary AND diverse vs the
  Opus-authored F5) closed CLEAN: 0 P0 / 0 P1 / 0 P2 / 2 P3**, both parity
  one-liners, both FIXED in the follow-up: **F7** -- the F5 plumbing tolerated a
  NULL `out_free` (a silent Burrow LEAK, strictly worse than F6's inline free it
  landed beside), made mandatory-extinct-on-NULL across `vma_replace_range_in` /
  `burrow_map_fixed` / `burrow_map_fixed_in` (F6 parity); **F8** --
  `vma_replace_range_in` lacked the `BURROW_TYPE_CODE` refusal both detach paths
  enforce (a latent I-42 pair-lifetime bypass; pre-existing D-3b, unreachable today
  since MAP_FIXED is phenotype-only + CODE is native-only), added the one-line
  refusal. Regression `burrow.map_fixed_refuses_code_alias` (revert-probe reddens
  only it). #205 re-audit-verified benign (over-charge only). The re-audit
  re-verified the F5 fix sound on every axis (out_free on every path, freed once
  past the unlock, no alias, no double-free, {0,0}-unreachable, the regression a
  valid discriminator). The dirty-close chain on the D-3c teardown surface is now
  CLOSED -- F7/F8 are P3 one-liners, not a dirty close, so no further round is owed.

Gate (as-built; CORRECTED from the design, see #189 above): the L-6c script
spawns `ld-musl-aarch64.so.1 /usr/bin/getconf PAGESIZE` explicitly (no D-4
needed) — stock ldso maps a stock dynamic binary end to end, printing `4096`.
The ORIGINAL design line named `/bin/echo`, which is a symlink to a STATIC
busybox and can never load through ldso.

---

## 7. D-4 — the exec rewrite, and D-5 — the pipeline

### 7.1 D-4: PT_INTERP -> ldso (the §3.2 vote)

In the kernel exec core, gated on the execing Proc being `PHENO_LINUX`
(native execs keep rejecting PT_INTERP): on detecting PT_INTERP, read the
interpreter path (bounded, NUL-checked, from the already-read header
buffer), resolve it THROUGH THAT PROC'S NAMESPACE via stalk — the interp
path is container-relative and crosses its own symlink, which is why D-1
precedes — and restart the load on the interpreter with argv shifted. One
level only: an interpreter that itself carries PT_INTERP is refused. Both
entries route here (the in-container execve T2 shell and the runner's ENTRY
spawn), so there is ONE mechanism.

**The argv shape**, corrected 2026-08-10 from the 08-05 vote's
`[interp_path, orig_path, orig argv[1..]]`:

```
[interp_path, "--argv0", orig_argv0, "--", orig_path, orig argv[1..]]
```

Four inserted slots, `argc + 4`. The vote's shape does NOT produce the
property the vote asserted next to it ("the app still sees its own argv[0]"):
musl's direct mode uses ONE slot for both "which file to load" and "what
argv[0] becomes" (`dynlink.c:1913` `app.name = argv[0]`), so under the stated
shape a program is handed the PATH it was resolved from, not the name its
caller invoked it by. `--argv0` separates the two — musl consumes it into
`replace_argv0` and applies it at `dynlink.c:2071`, after which the app's
vector is byte-identical to the caller's. The parser consumes exactly the four
slots it is given and rewrites argc to `N` in `argv[-1]`, so the app sees the
original `argc` too.

Why the flag rather than the literal shape: the corrected version delivers
what was voted, and the difference is not cosmetic — `argv[0]` is a
DISPATCH input for the two programs this arc is built on. busybox selects its
applet from `basename(argv[0])`, and a login shell is identified by a leading
`-`. The literal shape happens to survive both only while path-basename ==
applet-name, i.e. it passes the gate and fails `exec -a`.

`--` is emitted unconditionally, not only when needed: without it a program
path beginning with `--` is eaten by the option parser, and a shape that
changes with its data is a shape only one of its branches ever gets tested.
Same reason `--argv0` is unconditional — when `orig_argv0 == orig_path` it is
semantically a no-op, and paying four slots to keep ONE audited path is the
trade this file makes everywhere else.

Audit posture: audit-noted, joins the exec/REVENANT row (resolution runs
under I-28; the loaded image is the interp — everything downstream is D-2's
already-audited single-image path).

Gate: busybox `sh` execs a DYNAMIC `/bin/ls` via its own execve — proving
the kernel-shell route, not just the runner route. It must discriminate on
BY-NAME execution specifically: D3-A already runs the same objects through an
explicit `ld-musl ... getconf` spawn, so a D-4 gate that only proves "a
dynamic binary ran" is green before D-4 is written. The leg therefore asserts
rendered output from a program named WITHOUT its interpreter, and the
argv[0] half is asserted separately (a program that reports its own
`argv[0]`), because the two are independent claims and one leg cannot fail
for both reasons distinguishably.

### 7.2 D-5: the stock rootfs pipeline + THE ARC GATE

**The fixture, measured 2026-08-10** (`alpine-minirootfs-3.21.0-aarch64.tar.gz`,
sha256 `f31202c4070c4ef7de9e157e1bd01cb4da3a2150035d74ea5372c5e86f1efac1`,
3.85 MiB compressed / 8.1 MiB extracted). The composition is the reason this
chunk is shaped the way it is:

| Entry kind | Count | Consequence for D-5 |
|---|---|---|
| symlinks | **335** (64%) | The image is mostly links. Pool-symlink fidelity is the load-bearing property, not an incidental one. |
| regular files | 88 | Of which exactly two matter to the gate: `bin/busybox` (919 KB) and `lib/ld-musl-aarch64.so.1` (723 KB). |
| directories | 97 | Includes `/proc`, `/sys`, `/dev`; `/net` + `/env` are Thylacine-shaped and must be added as anchors. |
| device nodes | **0** | The planned "device nodes are skipped, documented" item is **VACUOUS for this image** — there are none to skip. Recorded as a measurement rather than performed as a step. `tar -xzf` exits 0 with no privileged operation. |

Two symlink shapes are present and they behave differently:
`/bin/sh -> /bin/busybox` is **absolute** (I-28 re-anchors it at the container's
own `root_spoor`, which is exactly the containment D-1 built), and
`/etc/os-release -> ../usr/lib/os-release` is **relative**, crossing `..` and
back down.

**Correction to the planned gate line.** `viv run` takes exactly three
arguments — `usr/viv/src/main.rs:653` rejects anything else (`args.len() != 3`)
— so `viv run /vivarium/alpine /bin/sh -c '…'` names a CLI form that does not
exist. The entrypoint comes from the bundle's `config.json`, as it does for
every other bundle. No CLI change is made for this: an argv override would put
the runner in a position to contradict the manifest, and the manifest is the
only thing that can declare a phenotype (VIVARIUM §12.1 rule 1). The gate's
command therefore lives in `process.args` as `["/bin/sh", "-c", "…"]`.

**Two bundles, not a flip.** The gate gets a NEW `/vivarium/alpine-stock`;
the existing `/vivarium/alpine` is left exactly as it is.

- `/vivarium/alpine` substitutes busybox-**static** at `/bin/sh`, and all nine
  `L6C-*` legs plus `D2-*`/`D3-*`/`D4-*` run through it. Flipping it to the
  stock dynamic shell would make that entire leg list depend on D-4, so any
  D-4 regression would present as "the shell did not run" with no first-missing
  signal — `tools/build.sh` already carries this reasoning at the substitution
  site.
- Two bundles also *are* the discrimination: stock gate red while `L6C-A..I`
  stay green isolates the fault to the stock-dynamic path specifically.
- It costs a second 8.1 MiB copy in the pool. That is the price of the signal.

**"UNMODIFIED" means: no stock file is replaced, removed, or edited.** The only
additions are the mount anchors the recipe structurally requires (a bind needs
an existing mount point): the `/net` and `/env` directories, and the six
`/dev` leaf files. Nothing is written into the rootfs to carry the gate — the
script rides in `process.args`, so the staged tree is the tarball plus anchors.

**THE ARC GATE, boot-fatal in joey.** Five legs, ordered so that each one adds
**exactly one** new mechanism to the one before it; that is what makes a
first-missing marker name a cause rather than a symptom.

| Leg | Marker | The one new mechanism | Red alone means |
|---|---|---|---|
| A | `DISTRO-A-stock-sh` | The whole D-1..D-4 chain at once: `/bin/sh` (an absolute POOL symlink) -> stock ET_DYN PIE busybox -> `PT_INTERP` -> stock ldso -> applet dispatch on `basename(argv[0]) == "sh"`. Emitted by a shell BUILTIN, so no second exec is involved. | The stock shell cannot start at all. |
| B | `DISTRO-B-stock-exec` | fork + exec of a stock dynamic binary FROM a stock dynamic parent, in busybox's **multiplexer** form (`/bin/busybox echo` — a real file, argv[0]-independent). | Exec-from-a-dynamic-parent is broken, independent of symlinks and of argv[0]. |
| C | `DISTRO-C-applet-by-symlink` | A second absolute pool symlink resolved for **exec**, plus argv[0] applet dispatch: `/bin/cat` (a symlink) reading `/usr/lib/os-release` (a **real** file). | The kernel leaked the symlink-RESOLVED path into `argv[0]`: busybox would then see `basename == "busybox"`, take the filename for an applet name, and emit nothing. |
| D | `DISTRO-D-relative-symlink` | A **relative** pool symlink crossing `..` (`/etc/os-release`), read through the already-proven multiplexer form so the link is the only new variable. C and D are independent — symlinked-applet/real-target versus multiplexer/symlinked-target — so neither can mask the other. | Relative pool symlinks are not traversable by the guest resolver. |
| E | `DISTRO-E-pinned-image` | The pool holds the **pinned** image, asserted from inside the guest (`VERSION_ID=3.21.0`) — the #126 stale-bake detector. Reads D's capture, so a broken D darkens E too; read D first. | A `PRESERVE=1` build served a stale rootfs. |
| — | `DISTRO-DONE` | The script reached its end. | The shell died mid-script rather than a leg merely failing. |

Leg C is worth stating precisely, because it is easy to overclaim: it does
**not** discriminate `--argv0` from passing the path alone (§7.1's property —
`joey.c` records that nothing on this rootfs can produce a vector where those
two differ, and the claim is carried at the unit level by
`exec.interp_argv_shape`). What C discriminates is a different and previously
untested claim: **symlink resolution must not become visible in `argv[0]`.**
Every busybox-based distro depends on it.

Equally: the gate does **not** exercise a relative symlink on the *loader* path.
`libc.musl-aarch64.so.1` matches the `"c."` entry of musl's reserved list
(`third_party/musl/ldso/dynlink.c:1074-1082`), so `load_library` short-circuits
it to `&ldso` and never opens the file — the `/lib/libc.musl-aarch64.so.1 ->
ld-musl-aarch64.so.1` link is present but never traversed. Leg D covers the
relative class through an ordinary `open`, which is why it exists.

**No `>` redirection anywhere in the gate script.** #201: the vivarium's
`openat` refuses `O_CREAT` unconditionally, and a plain `>` passes
`O_WRONLY|O_CREAT|O_TRUNC` even onto a file that already exists. Every
assertion is therefore a `$( )` capture (a pipe) or a `2>&1` dup — the same
constraint D-3c's gate line already works under. This is a real fidelity gap
being routed around, not a stylistic choice; #201 remains the blocker.

**BOOT-FATAL WHEN IT RUNS — AND SINCE #212, VISIBLE WHEN IT DOES NOT.** The
gate soft-skips when `/vivarium/alpine-stock` is absent, which is correct (the
Alpine tarball is untracked and lives only in gitignored `build/cache/`, so a
fresh clone, CI and any hermetic build have none) and which L-6c does
identically. But `tools/test.sh` keyed its verdict on the boot banner alone, so
the skip never reached the exit status: **a tree whose D-1..D-4 chain had
regressed exited 0 identically to one where the whole arc ran**, provided the
tarball was absent. The arc's headline claim was opt-in and its absence
invisible. joey now records each gate's disposition and emits
`joey: ARC-GATES l6c=<state> d5=<state>` (`not-built` in the `--production`
shape), `tools/check-arc-gates.sh` reads it, and `tools/test.sh` fails on a
MISSING state or an absent report — the dropped-gate shape a green boot could
otherwise hide. A skip still passes, but is now *reported*, and reported as
NOT coverage; `THYLA_ARC_GATES=require` makes it fatal where the fixture
ships. Full contract: `TOOLING.md §5.1`. Note what this does not do — it makes
the unknown visible, it does not make it known.

**The "and its full fix must re-open #192" clause is RETIRED at D-close** —
see the #192 verdict's correction in section 6. #192 was never actually gated
on `O_CREAT`: the phenotype can already WRITE any existing file it has DAC
write on (`VIV_LINUX_WRITE -> SYS_WRITE` is a tier-1 direct row), so the
write→mmap-R+X→jump chain is reachable with or without the create fix. #192 is
now tied to per-mount `noexec` (task #217) instead.

- Bake note: the bundle is pool-resident — the #126 stale-binary trap
  applies to it VERBATIM (a PRESERVE=1 build runs the OLD rootfs); the gate
  leg's comment carries the pointer.
- Soft-skip when the tarball is absent, so the default build stays hermetic
  (the L-6c precedent: an absent fixture is a missing input, not a broken
  kernel). The fatality applies to a gate that RAN.

### 7.3 D-close

The ARC holotype (the L-7 precedent: a round scoped to the whole arc catches
premises a later chunk voided that no chunk-scoped round can see) + the SMP
gate + reference docs (`docs/reference/` per-surface) + the audit-trigger
rows.

**CORRECTED at D-close (measured, 2026-08-10).** This line used to owe "the
ARCH section 25.4 rows for D-1 and D-3", which named the wrong file: the
trigger table moved out of `CLAUDE.md` into `docs/AUDIT-TRIGGERS.md` at
`83f9e0a2`, hours before this arc's design commit, and `ARCHITECTURE.md`
section 25.4 is the older expanded twin that has received **no new row since**
(measured: exactly two commits touched `ARCHITECTURE.md` after the move, both
this arc's, and both edited section 6.5 prose and the section 28 I-28 cell --
neither added a 25.4 row). The obligation the parenthetical actually stated --
*rows land with their chunks* -- was therefore discharged where the tree now
keeps them: D-1's row at `1acae6fa`, D-2's, D-3's and D-4's with their own
chunks, all in `docs/AUDIT-TRIGGERS.md`. Duplicating them into section 25.4
would widen the two-table drift that task #152 exists to close, so D-close
does not do it.

What D-close DID owe on the docs, and paid: the D-3 row still carried
"**DIRTY CLOSE** -> a follow-up re-audit round is owed on the fix" long after
that round had run **twice** (Opus found F5 [P1]; Fable then closed clean with
F7/F8 [P3]) and the chain had closed at `d4c49e5b`. Section 6 above recorded
the closure; the row did not, so a future session grepping the trigger table
before touching `kernel/burrow.c` or `kernel/vma.c` would have been told an
audit was outstanding. Fixed at D-close. **The generalization is #123's:** a
claim with a lifetime -- "owed", "deferred", "unreachable", "in flight" --
outlives the condition that made it true, so when the condition changes, grep
the CLAIM across every file that carries it rather than updating the one doc
you happen to be editing.

---

## 8. The ladder, dependencies, seams

| Chunk | Delivers | Gate | Audit |
|---|---|---|---|
| D-1 | symlinks in stalk + phenotype NOFOLLOW rows + T_E_LOOP (signoff) | joey symlink battery, revert-probed | FOCUSED ROUND (I-28) |
| D-2 | ET_DYN load + AT_ENTRY (**AS-BUILT**) | stock-ldso usage line, boot-fatal + revert-probed (static-PIE hello = seam, #188) | audit-noted |
| D-3 | file-backed EL0 mmap + MAP_FIXED subset (a: **AS-BUILT**; b, c owed) | runner-spawned `ld-musl /usr/bin/getconf PAGESIZE` (#189: `/bin/echo` is STATIC) | FOCUSED ROUND (I-36/I-12/I-32) |
| D-4 | PT_INTERP -> ldso rewrite (**AS-BUILT**) | `D4-A-byname-getconf-4096` + `D4-B-argv0-is-the-program`, boot-fatal; 3-way discriminated (S1 rewrite-off reddens the gate + the suite is blind; S2 argv0:=path is gate-green + reddens the unit test) | audit-noted (exec row) |
| D-5 | stock rootfs pipeline (**AS-BUILT**): `/vivarium/alpine-stock` staged UNMODIFIED from the sha256-pinned tarball, alongside (not replacing) the L-6c bundle; put-symlinks verified in the pool 2026-08-10 | THE ARC GATE, boot-fatal + PASSING: `DISTRO-A-stock-sh` / `B-stock-exec` / `C-applet-by-symlink` / `D-relative-symlink` / `E-pinned-image` / `DONE`; discriminated 3 ways on the host (C alone / D+E / E alone) | build-infra |
| D-close (**AS-BUILT**) | arc holotype + SMP gate + docs; the audit-trigger rows were already discharged with their chunks (section 7.3's "ARCH 25.4" named the pre-move file) | suite **1389/1389**; boot OK with **both** arc gates PASS (RAN, not skipped -- all six D-5 markers); **SMP 40/40, 0 corruption / 0 external-kill / 0 inject-miss / 0 timing / 0 other** across default+ubsan x smp4+smp8; `burrow.tla` control CLEAN (461 states) + 3/3 buggy cfgs DETECT; doc-tables 70 | **CLOSED @`8cebe2b1`: 0 P0 / 1 P1 / 0 P2 / 0 P3, Fable 5, `MODEL(start)==MODEL(end)`, NOT dirty.** The P1 was a premise two earlier chunk-scoped rounds had re-affirmed -- see section 6's #192 correction; enforcement owed at the noexec chunk |

Order: D-1 first (independent; everything else walks through it). D-2 ->
D-3 -> D-4 sequential. D-5's Stratum half can land any time; its gate needs
all of D-1..D-4.

Recorded seams (not built in this arc): apk + container networking; tty in
the vivarium; Larder symlink-target cache; native SYS_SYMLINK/SYS_READLINK;
PIE base randomization (I-16); in-kernel PT_INTERP (fidelity lift);
`/proc/self/exe` fidelity (#90); mmap charge for demand-paged FILE pages
(the R-5 seam carries); glibc distros.

## 9. Spec + invariant posture

No new TLA+ module: resolution logic and loader shapes, no new wait/wake
protocol — the stalk-gate family precedent (prose + revert-probed
regressions + focused audits) governs; the 2026-05-23 suspension stands.
No new invariant NUMBER: **I-28 is refined** (symlink expansion contained by
the same machinery — the section 28 row gains the clause in this scripture
commit) and **I-36 is generalized** (the seven conditions extend to
phenotype mmap-time; the "kernel-internal" qualifier relaxes to admit
read-only/exec userspace file maps, writable stays banned). ERRORS.md
signoff owed at D-1 for T_E_LOOP = 40.
