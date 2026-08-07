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
  has `symlink`/`readlink` CLI verbs (`run.c:1235,1312`). Whether the
  recursive `put` RECREATES symlinks is UNVERIFIED — a D-5 obligation.
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
  D-4 exec-rewrite rests on.
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
nothing in the `/bin/sh` gate needs. Known gaps under the rewrite, accepted:
`/proc/self/exe` reports ldso (the #90 class — that surface already reports
the mounter, not the reader), `AT_EXECFN` absent.

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
| D-3c | the gate + reference docs + the FOCUSED audit round | owed |

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
  reads the `AT_PAGESZ` our exec builds), and which re-crosses D-1's symlink on
  the way (`libc.musl-aarch64.so.1 -> ld-musl-aarch64.so.1`).
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

Gate (CORRECTED, see #189 above): the runner spawns
`ld-musl-aarch64.so.1 /usr/bin/getconf PAGESIZE` explicitly (no D-4 needed)
— stock ldso maps stock libc.so and runs a stock dynamic binary end to end,
printing `4096`. The ORIGINAL line named `/bin/echo`, which is a symlink to a
STATIC busybox and can never load through ldso.

---

## 7. D-4 — the exec rewrite, and D-5 — the pipeline

### 7.1 D-4: PT_INTERP -> ldso (the §3.2 vote)

In the kernel exec core, gated on the caller being `PHENO_LINUX` (native
execs keep rejecting PT_INTERP): on detecting PT_INTERP, read the
interpreter path (bounded, NUL-checked, from the already-read header
buffer), resolve it THROUGH THE CALLER'S NAMESPACE via stalk — the interp
path is container-relative and crosses its own symlink, which is why D-1
precedes — and restart the load on the interpreter with argv shifted to
`[interp_path, orig_path, orig argv[1..]]` (musl direct mode: the app still
sees its own argv[0], musl shifts internally). One level only: an
interpreter that itself carries PT_INTERP is refused. Both entries route
here (the in-container execve T2 shell and the runner's ENTRY spawn), so
there is ONE mechanism.

Audit posture: audit-noted, joins the exec/REVENANT row (resolution runs
under I-28; the loaded image is the interp — everything downstream is D-2's
already-audited single-image path).

Gate: busybox `sh` execs a DYNAMIC `/bin/ls` via its own execve — proving
the kernel-shell route, not just the runner route.

### 7.2 D-5: the stock rootfs pipeline + THE ARC GATE

- Fetch/pin the Alpine aarch64 minirootfs (version-pinned + checksummed at
  build time; vendoring vs fetch decided by size at impl). The tarball is
  staged UNMODIFIED.
- **`stratum-fs put` learns symlinks** (Stratum-side, in-scope): the CLI
  symlink verb exists (`run.c:1235`); the recursive put's walker must
  recreate `S_IFLNK` entries via it. Verify-then-extend — this is the
  unverified link in the chain (§2). Device nodes in the tarball are
  skipped, documented.
- The bundle: `/vivarium/alpine/rootfs` in the existing bundle shape;
  `viv run` entrypoint `/bin/sh`.
- **THE ARC GATE, boot-fatal in joey**: `viv run /vivarium/alpine
  /bin/sh -c 'echo DISTRO-GATE-OK'` — through the symlink, through ldso,
  through stock libc. The L-6c gate-fatality discipline (a gate that cannot
  redden is a disabled test).
- Bake note: the bundle is pool-resident — the #126 stale-binary trap
  applies to it VERBATIM (a PRESERVE=1 build runs the OLD rootfs); the gate
  leg's comment carries the pointer.

### 7.3 D-close

The ARC holotype (the L-7 precedent: a round scoped to the whole arc catches
premises a later chunk voided that no chunk-scoped round can see) + the SMP
gate + reference docs (`docs/reference/` per-surface) + the ARCH section
25.4 rows for D-1 and D-3 (cumulative-trigger rule: rows land with their
chunks).

---

## 8. The ladder, dependencies, seams

| Chunk | Delivers | Gate | Audit |
|---|---|---|---|
| D-1 | symlinks in stalk + phenotype NOFOLLOW rows + T_E_LOOP (signoff) | joey symlink battery, revert-probed | FOCUSED ROUND (I-28) |
| D-2 | ET_DYN load + AT_ENTRY (**AS-BUILT**) | stock-ldso usage line, boot-fatal + revert-probed (static-PIE hello = seam, #188) | audit-noted |
| D-3 | file-backed EL0 mmap + MAP_FIXED subset (a: **AS-BUILT**; b, c owed) | runner-spawned `ld-musl /usr/bin/getconf PAGESIZE` (#189: `/bin/echo` is STATIC) | FOCUSED ROUND (I-36/I-12/I-32) |
| D-4 | PT_INTERP -> ldso rewrite | busybox execs dynamic `/bin/ls` | audit-noted (exec row) |
| D-5 | stock rootfs pipeline + put-symlinks (Stratum) | THE ARC GATE: `/bin/sh -c` boot-fatal | build-infra + Stratum |
| D-close | arc holotype + SMP gate + docs | clean close | ARC ROUND |

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
