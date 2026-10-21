---
id: sub-substrate-build
type: sub
parent: moc-substrate
title: "The build — targets, the ledger, and the four guards on a stale artifact"
code:
  - tools/build.sh
  - tools/mkcpio.py
  - tools/mkdisk.py
  - tools/build-config.sh
  - tools/build-manifest.toml
  - tools/forage.sh
  - tools/test-forage.sh
  - tools/test-build-config.sh
  - tools/configure.sh
  - tools/test-configure.sh
  - tools/check-flag-words.py
  - tools/test-host-boundaries.sh
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/TOOLING.md"]
created: 2026-08-01
updated: 2026-10-21
---
## Purpose

Produce the bootable image: kernel ELF + flat binary, the native and Rust
userspace, the pouch POSIX sysroot, the Go GOROOT, the Clade toolchain, the
`ramfs.cpio` initrd, and the Stratum `pool.img` + `system.key` twins.

## Contract

`all` is an ALIAS for `kernel` — the same chain:

```
all -> kernel -> { userspace, pouch-progs, stratumd, pool-fixture, ramfs, disk }
```

Sub-targets build one stage each. `clean` is the only true from-scratch
reset.

The dispatcher is authoritative for the target list: it has 21 named arms,
including `dosbox-x` and the Clade staging targets. The unknown-target help
lists 17 and omits `quake-host`, `clade`, `stage-clade`, and `stage-storm`;
the introductory comments are also not an exhaustive inventory.

**Every run ends with a `SUMMARY for target ...` block listing exactly what
was BUILT / REUSED / PRESERVED.** That block is the contract: read it to
know the resulting state rather than inferring it from the target name.

## Mechanism

### Typed configuration and external inputs

`build-config.sh` loads the typed configuration axes, presets and fragments;
its tests check parsing and precedence. `build-manifest.toml` records external
inputs, while `forage.sh` resolves and checks those inputs before a build.
`MANIFEST` and `FORAGE_ROOT` isolate fixture tests; they are not permission to
silently substitute downloaded bytes for a pinned archive. DOSBox and game
baking follow the same configured input path as the existing toolchain.

An input the manifest does not declare is invisible twice over: `forage.sh
status` cannot report it missing, and whatever it feeds degrades without a
word. The static Linux git (`/viv/bin`, and the git-probe / git-net /
git-workflow bundles) was such an input until 2026-09-21 -- present only in one
worktree's `build/cache`, hinted at by `build.sh` as a forage target that did
not exist, and claimed as one by `docs/GIT-ON-THYLACINE.md`. `git-shell` had
failed 3/3 on main for eleven days as a result, with a timeout that read like a
broken git, because ut reports a missing command only in `$status`. It is now
`remote.static_git`: a `remote-pull` from the Pi that built it, sha-pinned, and
`do_remote_pull` verifies the pin of any pulled FILE (a pulled tree has none).
The gate SKIPs on an image without it.

**`build.sh kernel` is `build.sh all`, and this is the tree's most-repeated
footgun.** It pulls the whole chain including a pool re-bake driven by the
*ambient environment* — so running it without carrying `THYLACINE_BAKE_CLADE`
/ `THYLACINE_BAKE_GOROOT` / `THYLACINE_MKFS_PRESERVE` silently produces a
pool missing payloads a previous invocation had baked in. The gate learned
this the expensive way (#101); the countermeasure lives at the chokepoint,
in [[sub-substrate-gates]].

**Two staleness checks make the cache trustworthy, and both were added
after a stale artifact shipped.**

`sysroot_is_stale` — the pouch libc is cached and reused by `all`, so
editing a boundary-line patch used to link every pouch consumer against a
STALE libc. That is exactly what masked the A-2a `t_stat` 72→80 growth: the
kernel wrote 80 bytes into stratumd's stale 72-byte buffer, a silent stack
overflow that a "passing" boot hid. The check rebuilds when any file under
`usr/lib/pouch/{patches,compiler-rt}` is newer than the built `libc.a`.

`compiler-rt` was **not** watched when it landed, so `all` happily reused a
sysroot predating it and the change looked inert — the same
enumerate-what-you-expect failure as #91. A change to the *recipe*
(`build.sh` itself) still needs an explicit `sysroot`, deliberately:
watching build.sh would rebuild on every unrelated edit. The durable
backstop is a boot probe — a stale `libclang_rt` fails `/pouch-hello`'s
outline-atomics agreement check. Cheap mtime check on top, in-guest proof
underneath.

`stratum_host_tools_stale` mirrors it for the host-native Stratum tools. Its
predecessor rebuilt only when a *binary was missing*, so a Stratum source
edit shipped a stale host `stratumd` and the pool bake failed with "unknown
option".

**The pool and its key are coupled, and the coupling is the fix for a
year-long ghost.** `system.key` is random per regeneration, so a pool
re-bake without a ramfs re-bake leaves `/bin/system.key` (baked into the initrd)
pointing at the wrong pool → `STM_EBADTAG` at mount. That mismatch is "the
year-long 'AEGIS corruption' ghost." The `pool` target therefore couples the
two, and the `kernel`/`all` chain regenerates both together so they always
agree.

**`ramfs.cpio` is not re-baked by `disk` or `userspace`.** The devramfs
holds the PRE-PIVOT binaries — joey's boot chain and every probe that runs
before the pivot to the disk-backed FS. So after editing a userspace binary,
`userspace` + `disk` boots the STALE pre-pivot binary and the change reaches
only the post-pivot image. The tell is precise and worth memorizing: *a
probe's self-reported count or output does not move though you "rebuilt"*.

**`libc.so` is a second musl build, and a static program does not change**
(B-1d, ARCH 6.5 "Dynamic loading"). `build_libc_shared` configures the patched
musl tree again in its own object directory (`build/pouch/musl-obj-shared`),
compiled AND linked by the LLVM fork's clang: it is the one compiler here whose
driver links for aarch64-thylacine, so musl's link probes pass and
`LDFLAGS_AUTO` carries `--no-undefined`, `--exclude-libs=ALL` and
`--dynamic-list`, without which `libc.so` is unsound. (Homebrew clang fails
every link probe silently, which is why the static build's `LDFLAGS_AUTO` is
empty; `libc.a` and the CRT objects stay the static build's.) Its compiler
runtime is a second builtins archive, `libclang_rt.builtins_pic.a`, built with
compiler-rt's own `-fPIC -fvisibility=hidden -DVISIBILITY_HIDDEN`; the
non-PIC archive is unchanged, and `sysroot_is_stale` rebuilds a sysroot that
lacks either. The installed `libc.so` is verified by shape: `ET_DYN`, no
`PT_INTERP`, no `DT_NEEDED`, no `TEXTREL`, entered at `_dlstart`, no undefined
dynamic symbol, `dlopen` / `dlsym` / `dlerror` / `malloc` / `printf` exported
and the compiler runtime (`__addtf3`, `__aarch64_cas8_acq_rel`) not. Without
the fork's clang it is skipped, and so are the prover and `lib/`. Measured at
B-1d against a snapshot: `crt1.o` and `Scrt1.o` identical, `libc.a` differs in
`mmap.o` alone (0047), the builtins identical, program objects differ only in
`.comment` (the clang revision), and relinking the new objects against the old
`libc.a` reproduces 25 of 26 executables byte for byte. The Rust target
(`usr/ports/rust/aarch64-unknown-thylacine.json`) sets
`static-position-independent-executables` false, because the driver now
refuses `-static-pie` where it used to drop it silently.

**The initrd is a tree, and the bake names what it must hold.** `mkcpio.py`
walks `ramfs-src` in sorted pre-order, emitting each directory (with its own
permission bits) before its contents, so the archive is byte-deterministic and
every parent precedes its children, which devramfs requires
([[dec-2026-09-25-devramfs-directories]], [[sub-kernel-content]]). It packed the
top level only until B-1d, and so dropped the staged `lib/` without a word.
`build_ramfs` stages every program and data file into `ramfs-src/bin/` and the
loader's files into `ramfs-src/lib/`; nothing else goes at the top, so no staged
name can meet one of the kernel's synthetic mount points
([[dec-2026-09-25-initrd-bin-directory]]; the flat layout had carried the
native `env` under the `/env` mount point since G15). The bake always passes
`--require bin/joey`, and when the sysroot has `libc.so` also `--require
lib/libc.so --require lib/libdlprobe.so --require bin/pouch-hello-dlopen`:
`mkcpio.py` re-reads the archive it wrote, and a missing entry deletes the
archive and fails the build, so no stale `ramfs.cpio` survives for `test.sh` to
boot. `bin/` and `lib/` are chmod 0755 whatever the host's umask, because every
principal's path search crosses them.

**The snapshot twins are minted at bake time.** `populate_stratum_pool`
finishes by cloning `pool.img` and `system.key` to `.baked-snapshot`
siblings (`cp -c`, APFS clonefile, plain copy elsewhere). Those twins are
what every downstream harness restores from per boot or per attempt — and
why the key twin can be compared to prove a restore is coherent.

**No directory the bake makes on the pool is world-writable (2026-10-18).** A
create through the pool's session is admitted only to system (IDENTITY-DESIGN
9.7 M7), and M7 does not cover remove or rename, so a world-writable directory
there would still let any user delete system's files. `stratum-fs put` carries
a host directory's permission bits and bakes every file 0644 or 0755 by its
exec bit. Four stamps made pool directories world-writable and are gone: each
viv bundle's `rootfs/tmp` 1777 (viv binds the invoker's own `/tmp` there), ICU's
`testdata/out` and the muster build trees 1777 (ls-libs layers a user-owned
directory over them), and Quake's game directories 0777, which populate now
sets 0755, since a stage an earlier bake left 0777 would otherwise carry over.
The git-net push token, the one file whose host mode matters, is restamped 0600
after the put. Runtime creates are another matter: nothing masks a create's
mode yet (OPEN-BUGS, 2026-10-18).

**A third check now runs before any target does, and it is the file's best
structural argument.** A hand-written patch series is validated for
unified-diff hunk line counts at one unconditional chokepoint ahead of the
dispatcher — ~50 ms for 281 hunks. Its comment states the reason plainly:
there are several `patch` loops (the sysroot, the SDL port, the game port,
the compiler and graphics ports), and the lesson from the earlier bake
failure is *verify at one chokepoint instead of copying a check into every
caller.* It exists because the tool ate a function definition out of a port
patch and exited zero — a `patch` that reports success having dropped added
lines past a mis-counted hunk header.

Note what makes this different in kind from the two staleness checks: those
watch mtimes and can only warn. This one reads the artifact's own internal
arithmetic and refuses.

**What the count check cannot see is WHERE a hunk lands** — and a review once
read its clean output as "no fuzz/offset/reject line" (B-0 poll audit round 4
F6, when pouch 0029 had in fact applied two lines off for its whole life). So
the pouch musl series is applied with `patch -F 0`: a hunk whose context does
not match exactly fails the build. That matters on the Linux builders, whose
GNU `patch` fuzzes up to two context lines by default and says so only on
stdout; the control was measured — a perturbed context line applies under
`-F 2` with exit 0 and fails under `-F 0`. The port patch loops are not yet
fuzz-strict.

**The spawn-args mirror check runs beside it (2026-09-29).**
`tools/check-spawn-args-mirrors.py` runs right after the hunk check, before the
dispatcher, for the same one-chokepoint reason: each target builds a different
copy of `struct sys_spawn_args` (libt, libthyla-rs, the pouch patch, and the Go
fork when `$GOFORK` has one), so a check inside any one target would miss the
others. It lays the record out from the kernel header, compares every copy
field by field, and proves it can fail before it passes. It is sub-second and
fatal, with no skip switch. Like the hunk check it refuses rather than warns,
because the failure it guards against is silent: a copy left behind when the
kernel record grows passes its own size assertion while the kernel reads past
it (#100). The record's rules are [[sub-kernel-syscall-abi]]'s.

**The flag-word check runs after it (2026-10-05; nine words since
2026-10-06).** `tools/check-flag-words.py` holds a table of flag words -- the
header, the pattern a member's name matches, the width: `proc_flags`, the
spawn permission word, the four one-bit spawn words and the spawn record's
`ext_flags` tail word (`SPAWN_EXT_*`, 2026-10-06), the walk-create mode
word (`SYS_WALK_CREATE_*`, with DMDIR and the DMSRV bits), the 9P attach flags
and the mount flags. It evaluates every member, resolving the header's other
macros, and fails when two members share a bit. A member owns the bits its own
literals contribute; a reference to another member contributes nothing, so a
mask built from members (`TERMINATE_PENDING_MASK`, `SPAWN_PERM_ALL`) overlaps
them freely, while the mode bits `SYS_WALK_CREATE_PERM_VALID` adds by literal
are its own. A member that uses another other than as an operand of `|` (a
shift, an `&`) cannot be classified and fails, and so do a member that does not
evaluate, one outside its word, and a word with no member: an unread flag is not
a checked one. Each flag's own `_Static_assert` names only the flags its author
knew, so two branches each took bit 22 of `proc_flags` and both compiled; the
header is the one source both must pass through. A passing check then proves it
can fail: each word's header is mutated in memory (a new member on an owned bit,
a member shifted from another, an undefined macro, a member outside the word,
every member renamed away), and a mutation the check does not report by the rule
it targets stops the build. It prints one line per word, then
`check-flag-words: 10 words ok; the self-test caught all 50 mutations`
(9 and 45 before the tail word). It is
sub-second and fatal, with no skip switch. It replaced `tools/check-proc-flags.py`,
main's single-word check, whose rule let a literal mask equal to a union of
flags overlap them; the literal rule is stricter.

**A free-space floor refuses before any target writes (2026-10-05).**
`disk_floor_check` refuses a target, and separately the pool generate, when
the build volume has less than `THYLACINE_MIN_FREE_GB` free (default 6; `0`
disables it, and `clean` is never refused, since it frees space). The refusal
names the stage, the free space, the floor and the override on one line, then
lists the largest entries under `build/`. The reason is the failure a full
disk causes elsewhere: a bake took the shared Mac's volume to 121 MB free
mid-populate, and every agent's shell then failed before it ran, because the
harness could not create its output file. A refusal while there is still room
keeps the failure inside this build. `du` counts APFS-cloned blocks in full,
so in a worktree with a cloned `build/` the list overstates what deleting an
entry frees; `df` is the measure.

**The host boundary checkers run on every build (2026-10-19).** Each
`tools/test-<name>.py` compiles the code it names from this tree beside
`tools/host-tests/<name>.c` with mocked owners, and runs it under ASan and UBSan
at -O0 and -O2; the native ones also pin the syscall numbers in every mirror and
the phenotype's collision asserts. All eight were run by nothing until this
date, so a renumbered mirror could drift with every build green.
`host_boundary_check` runs named drivers in parallel, each into a fresh
`build/host-boundaries/<set>/<name>`, and stops the build with the failing
log's tail: the native three (handle target replacement, the descriptor
handlers, the handle query) before any target except `clean`, and the Pouch
adapters in `build_sysroot`, step 2b, on the musl tree just patched.
`host_boundary_sets` derives both sets as the gate below does, from
`tools/host-tests/*.c`, a driver whose usage names a musl tree being Pouch's:
a test with no driver stops the build, and an empty set is refused, where the
two hand lists it replaced (round 1 F4) would have let a new checker join no
build. `tools/test-host-boundaries.sh` runs every checker clean and under each
mutant it declares (50 legs in 35 s when it landed), deriving the set the same way
and the mutants from each driver's `--help`; a checker that declares no mutant
fails it. A planted `VIV_NATIVE_CEILING` 132 stops the build at the check,
naming the pin. Moving patch 0057's getppid number stops it there too, since
the native handle-query checker pins that line; the Pouch call site's red is a
kill.c body only `test-pouch-kill-probe.py` reads (2026-10-19: step 2b refused,
three probe legs red, nothing configured after it).

**Both ambush builds spawn the launch target held (2026-09-30; untagged
2026-10-06).** The Go fork's `SysProcAttr.DebugHeld` sets the spawn record's
`debug_flags`, and ambush's `Launch` sets it, so a launched target stays parked
until the debugger's stop ([[sub-kernel-birth-hold]]). This covers both
builds: `build_ambush`'s ramfs copy for `/ambush-probe`, and the `/goroot/bin`
copy that nora's `:debug` runs. While some trees' kernels lacked the hold, the
fork made the held launch a build tag (`thylacine_held`) that this file passed.
Main carries the hold since its aux-3 merge, and so does every tree built from
main since, so ambush 073faaa compiles the held launch untagged. A tree whose
kernel predates the hold gets an ambush whose launches fail loudly, since its
kernel refuses the flag. `ambush_fork_check` guards the fork's age: it asks
`go list` which files the build compiles. It refuses `held_off_thylacine.go`,
the running spawn an older fork compiles untagged under a log line that says
nothing. `held_on_thylacine.go` must declare `launchHeld = true`. A fork with
neither file, as once the constant is deleted, must name `launchHeld` in no
compiled file, and its `Launch` must set `DebugHeld`; a fork from before the
held launch does not, and is refused. The check and the build run
the same toolchain (`$GOFORK/bin/go`) with the same environment, so the file
selection cannot change between them and the artifact needs no check of its
own. Whether `Launch` still acts on the constant is
behaviour, which `/ambush-probe` checks at the entry. The ramfs also carries
`/bin/ambush-notelf`, an executable that is not an ELF image, for stage D's
abandoned-launch leg (DELVE-PORT-DESIGN section 7 (b)).

**A fourth guard warns about a stage the main chain never refreshes.** The
compiler-toolchain staging step is reachable only as its own explicit
target, never from `all`, so a rebuilt graphics binary does not reach the
staged tree on its own — and the pool then re-mints faithfully around the
*previous* binary with every ledger line green. That trap has been paid for
twice, and the second time the gate failed three times out of three on a
binary twenty-seven minutes older than the fix under test, *looking exactly
like a real defect in the change*. The warning compares mtimes deliberately:
a content check would mean stripping a ~145 MB binary on every pool bake.
See Caveats for what its own comment claims versus what it does.

**Host-side pool population reuses shipped Stratum tools, not new code.**
The "installer" is shell orchestration: `stratum-mkfs` creates the pool,
host `stratumd` is started on a temp socket, `stratum-fs write` writes each
corpus file through the audited 9P client, stratumd is stopped. No
Stratum-side code exists for it, so the bake exercises the same Twrite /
Tlcreate paths the guest does.

**The bake stamps `PRINCIPAL_SYSTEM` ownership (A-3, the no-brick pass).** So
the boot chain owns its own tree once dev9p enforces per-file rwx (A-3b), the
pool bake stamps `PRINCIPAL_SYSTEM` (`4294967294`) everywhere: `stratum-mkfs
--root-uid` / `--root-gid` stamps the pool ROOT inode, and host `stratumd
--bake-owner-uid` / `--bake-owner-gid` stamps every file `stratum-fs write`
creates ([[sub-stratum-server]]). Both are required — the mkfs root inode was
uid=0/0755, so without `--root-uid` the `PRINCIPAL_SYSTEM` boot chain hit the
pool root as *other* and joey's create in `/` was denied even with every baked
file SYSTEM-owned. With root + baked files + runtime creates all SYSTEM-owned,
the boot chain owns the whole tree. It is a stamped *value*, not a format change
(`si_uid` / `si_gid` already exist in the inode).

The aux integration adds default-on DOSBox-X build/staging and its system
configuration at `/lib/dosbox-x/dosbox-x.conf`, plus optional Duke3D and Tomb
Raider fixture stages. Emulator opt-out also skips its game data. Missing
external C++ tooling is announced as a skipped build, not emulator coverage.
View, Gallery, Manual, Nocturne and their probes are curated into the native
ramfs binary list. Under either Halcyon lever (`THYLACINE_HALCYON` or
`THYLACINE_HALCYON_SESSION`) the pool also carries the inline-media fixtures
from `usr/view/testdata`, each readback-verified: `/test.png` and `/test.jpg`
(the 640x400 witness card) and, since 2026-09-29, `/test-large.png` (the same
card at 2048x1536, which a pane shows only after `view` reduces it to the
pane's limit). `configs/ci.config` selects a serial shell for existing
interactive scenarios; the default profile starts the Halcyon session.
Use an explicit `HALCYON_SESSION=y` override for graphical session gates.

Every bake's pool also carries the symlink prover's fixture at
`/lib/pouch-links` (B-3f', 2026-10-20): the files `target` and `sub/leaf` and
real links of each shape the prover reads -- `live -> target`,
`dead -> nosuch`, `dir -> sub`, `abs -> /lib/pouch-links/target`,
`loop -> loop` and `chain -> live` -- and the dangling `long`, whose target is
64 `x`s, exactly the first buffer go-fs's `os.Readlink` reads (Stratum keeps
a target of at most 100 bytes, so no longer one exists to grow it twice).
They are staged from nothing under `$BUILD_DIR/pouch-links` and put with
`stratum-fs put`, which copies a link as a link (lstat, readlink,
`stm_9p_symlink`) and refuses to omit one; the bake then syncs and reads `abs`
and `long` back with `stratum-fs readlink`, and a failed put, sync or readback
stops it. `pouch-hello-links` is on both curated Pouch lists,
`build_pouch_progs`' build list and the initrd's `bin/` list
([[sub-stratum-boot]] runs it), and `pouch-hello-tz` (0060) after it; the time
zone prover writes its own fixture at run time, so the bake stages none.

`HALCYON_PROFILE` (`choice:instrument,legacy`, default `instrument`, since
2026-09-21) is the option that selects WHICH Halcyon UI an image draws; it bakes
`/lib/halcyon/profile`. The theme only colours the profile in force, and a
theme of the other schema is projected rather than refused, so before the
option existed a config that named an Instrument theme still built the legacy
layout. `configs/default.config` pins `instrument`; `configs/ci.config` pins
`legacy` because the pre-Instrument gate scenarios assert its literals, and an
Instrument gate overrides that pin with a caller-set
`THYLACINE_HALCYON_PROFILE=instrument`. `tools/configure.sh` tags each theme
file with the schema its own `[meta]` declares, hides `TEMPLATE.toml`, and
prints a note when the chosen theme and profile differ.

**`CHUNK_WEBKIT` (default OFF, since Boosty B-0, 2026-09-21)** builds ICU and
JavaScriptCore and bakes `/webkit/jsc` ([[sub-webkit]] describes the port; this
paragraph describes the wiring). Three things about it are unlike every other
chunk, each on purpose.

*A new input class: a pinned upstream plus an in-repo series.* `[source.webkit]`
is kind `clone-sparse`: nothing of ours is hosted, so it is not a `fork.`.
`forage.sh` makes a partial (`blob:none`) sparse clone at a TAG, refuses unless
that tag resolves to the manifest's commit, creates a local branch, and `git am`s
`usr/ports/webkit/patches/*`. The series is stacked -- a later patch may edit
lines an earlier one added (0003 corrects a comment 0001 wrote) -- so no patch is
asked about alone: `series_tree` builds the pin plus the first k patches in a
scratch index, and the longest k whose tree is HEAD's is what is applied; the rest
are `am`'d. It is idempotent (the whole series' tree reports every patch as
already applied) and it never resets: a checkout that is the pin plus no prefix of
the series -- including one that carries an extra hunk in a file a patch names --
is reported as drifted and left alone, because it may hold work. An existing sparse checkout is only widened (`add`, so a
cone somebody added by hand stays); a full checkout at the path is reported and
left alone, never converted. `test-forage.sh` C1-C9 drive the outcomes against a
local upstream with no network: C6-C8 a stacked series (fresh, idempotent, from a
prefix), C9 an extra hunk in a named file; against the per-patch forage that
preceded gcpark, C7 and C9 fail.

*The build re-checks the checkout from its own side.* `webkit_checkout_ok`
requires the pin to be an ancestor of HEAD, a clean tree, and HEAD's tree to be
EXACTLY the tree `webkit_series_tree` builds from the pin plus the series in
order (about 0.6 s on the WebKit tree; no network, the blob-less clone holds every
tree and the patched files' blobs). Until gcpark (2026-10-12) it asked each patch
to reverse-apply and the changed files to be the series' files, which passed a
commit adding an extra hunk to a file a patch names (measured on the B-2c tip:
one line in `ExecutableAllocator.cpp` passed) and could not express a stacked
series. `test-forage.sh` D1-D5 lift the two functions from `build.sh` and drive
them; D5 is the extra hunk over a one-patch series, which the old check accepted. So `WEBKIT_PIN` / `ICU_SHA256` in
`build.sh` and the manifest are two copies of one truth, and `test-forage.sh` A10
fails when they drift (sabotaged: one hex digit -> FAIL). A11 does the same for
every `cache.*` pin whose `feeds` names `CHUNK_LIBTESTS`, found from the
manifest rather than listed: its `file` and `sha256` must appear quoted in
`build.sh` (sabotaged: one hex digit of `FC_SHA256`, and the DejaVu tarball's
name -> one FAIL each).

*With the chunk ON, an absent input is an error, not an announced skip.* Every
default-on chunk skips gracefully so a bare checkout still builds. This one
defaults off, so reaching `build_jsc` means it was asked for by name, and
"skipped" would be the silent omission detect-and-instruct exists to end.

The objects live under `build/pouch/{icu,jsc}` deliberately: `build_sysroot` wipes
`build/pouch/`, and a static binary linked against the old `libc.a` is exactly
what must not survive a libc change. The ICU HOST tools (`build/icu-host`) do not
depend on the sysroot and survive it. `libc.a` is not a ninja input, so
`build_jsc` removes `bin/jsc` to force the relink. After the link it asserts the
shape the platform requires -- `ET_EXEC`, no `PT_DYNAMIC`, no `LOAD` segment both
writable and executable, and at least two `LOAD` segments seen, so a parse that
matched nothing cannot read as "no W+X" -- and that each function on a list
holds an `isb` in the symbol-bearing binary (`llvm-objdump
--disassemble-symbols`; at least eight instructions first, so a missing symbol
cannot pass). A fence that lets a thread run code another CPU wrote or patched
has no run-time witness, so the build asks the binary for each: since B-2d
`MachineThreads::unparkCurrentThread` (RED on the B-2c `jsc`, which has none),
since B-2e1 the two places a thread takes a finished Wasm plan
(`Wasm::Module::compileSync`, `JSWebAssemblyInstance::finalizeCreation`; the
B-2d `jsc` holds no `isb` in either) and the FTL's `JITFinalizer::finalize`,
whose fence is upstream's. The list is a quoted heredoc of mangled name,
label and, optionally, the mangled prefix of a call the `isb` must precede:
`finalizeCreation` runs the module's start function itself, so its fence must
come before its first call into `WebAssemblyModuleRecord::initializeExports`.
That test uses address order for program order and fails closed when the call
is laid out first or absent (both controls refuse on the device `jsc`). A
refusal names the label and the patch that owes it. Since B-2e3 it also asks
`Wasm::resetInstructionCacheOnAllThreads` for `SYS_ICACHE_SYNC_SPACE`: at
least three instructions, and the last write to `x8` before an `svc #0` must
be a `mov` of 128 (any other write to `x8`, a store from it included, clears
the match, so the check fails closed). The B-2e1 `jsc`, whose function is
upstream's 90-instruction thread walk, is refused, as are a wrong number, a
write to `x8` between the `mov` and the `svc`, and an `svc` before the `mov`.
Then it strips into `build/webkit/stage`.
Parallelism is sized from RAM as well as cores (`webkit_jobs`): JSC's unified
sources peak over 1 GiB per job, and eight jobs on an 8 GiB host is an OOM, not a
speedup. Verification: the pool's bake-verify expects `/webkit/jsc` under the same
predicate the populate arm uses; `check-v80-floor.py --all` scans the stage; the
device gate is `tools/interactive/ls-jsc.exp` (SKIPs with 77 when nothing is
staged).

**`CHUNK_LIBTESTS` (default OFF, since Boosty B-3, 2026-10-14;
`THYLACINE_BAKE_LIBS`)** builds the browser libraries' compiled upstream test
programs against the same static archives the browser links
(`dec-2026-10-14-b3-test-bar`) and bakes them with their data under `/var`.
`build_lib_tests` (also the `lib-tests` subcommand) stages
`build/pouch/lib-tests/stage/var/...`, under `build/pouch/` for the same reason
as ICU and `jsc`: a sysroot rebuild must take every binary linked to the old
`libc.a` with it. ICU's tests need no patch for a target whose tree is not the
build host's: `make QNX_TARGET=1` in `test/intltest` and `test/cintltst` is
upstream's switch, compiling the data root in as `/var/icu_tests` (the source
and build trees merged there: `bin/`, `data/{unidata,sprep,out/build}`,
`test/testdata` plus its built `out/`), and intltest's shared MessageFormat 2
data sits at that root's `../testdata`, so the stage carries `/var/testdata`
too. The populate arm keys on the flag AND the staged `intltest`, the pool grows
by 1 GiB when it bakes, and bake-verify expects three payloads (`ICUTEST`,
`ICUDATA` = the built package `out/testdata.dat`, `MF2DATA`) so
`tools/interactive/ls-libs.exp` can tell "no tests baked" (SKIP 77) from a
partial bake (FAIL); its first bake named a loose `.res` that the package build
never leaves, and bake-verify refused the pool. The masters are SYSTEM's and
read-only, `test/testdata/out` included: cintltst's `TestFileStream` writes
`testdata.tmp` beside `testdata.dat`, as it would in upstream's own writable
build tree, so ls-libs binds a directory the logged-in user owns over
`testdata/out` first (`bind -bc`). Until 2026-10-18 populate re-stamped it 1777,
which let any user make system-owned files there; a create through the pool's
session is now admitted only to system (IDENTITY-DESIGN 9.7 M7). Like `CHUNK_WEBKIT`, a missing input is an error
once the chunk is on.

`build_icu`'s reuse test also compares a stamp of the configure line and flags
(`stage/.thylacine-config`). Before B-3c it compared only timestamps, so a stage
built under different flags (B-3c added `-DU_HAVE_MMAP=0` and `--enable-tests`)
counted as up to date because its files were newer than their inputs: the same
stale-artifact class as the four guards above, for a flag rather than a file.

**The codecs (B-3d, 2026-10-15).** `cmake_lib` builds one CMake library under
`build/pouch/<name>`: the tarball checked against its pin (`[cache.libpng]`,
`[cache.libjpeg-turbo]`, `[cache.libwebp]`; `tools/forage.sh codecs`),
unpacked fresh with the port's patches in order (`usr/ports/<name>/patches`),
configured with the pouch C++ toolchain, built whole -- the library and
upstream's test programs -- and installed at `<name>/stage`. It installs with
the stage as the prefix, not under `DESTDIR` with a prefix of `/`:
GNUInstallDirs maps that prefix to `/usr`, and the first build left the
stages' `lib/` empty for libwebp's link to miss. Its reuse stamp carries the
pin, each patch's sha256 and the configure line, and libc, libc++ and the
toolchain files are checked against it by time. `build_libwebp` builds
libpng and libjpeg-turbo first and links cwebp/dwebp to them, so libwebp's
checks also cross the other two codecs (`tools/build.sh codecs`).
`build_lib_tests` is now two halves, ICU's and the codecs', each staging only
its own part of the stage; before the split, ICU's up-to-date return would have
skipped the codecs and its rebuild deleted the whole stage. The codec half
dumps each library's CTest list, has `tools/libtests-manifest.py` translate it
into `/var/lib_tests/<lib>.manifest` plus the files it needs
([[sub-substrate-libtests]]), stages them (programs stripped) with the native
runner at `/var/lib_tests/bin/muster`, and writes each manifest's test count to
`build/pouch/lib-tests/<lib>.count`, which ls-libs reads as the number the
device must report. The populate arm puts `/var/lib_tests`, and ls-libs layers a
directory of the user's own over each directory the staging listed in
`build/pouch/lib-tests/writable` (every
library's build tree, where its tests write their outputs as in upstream's
own tree; libxml2's source top, where runsuite and runxmlconf write their
logs; and libxml2's own directory, the build tree's parent, because runtest
names each output `--out` plus the input's base name with nothing between
them and CTest passes the build tree with no trailing slash, so the outputs
land beside it as `build<name>.res`): a list the staging derives, so a library added there cannot be
missed by a hand-written list in ls-libs. bake-verify adds `MUSTER`, `PNGTESTS`,
`JPEGTESTS`, `WEBPTESTS`, `XMLTESTS` and `XMLCONF`.

**libxml2 (B-3e, 2026-10-15).** `build_libxml2` configures 2.15.4 as
upstream's static CMake CI job does (`.gitlab-ci.yml` cmake:linux:gcc:static,
`.gitlab-ci/test_cmake.sh`: schematron, zlib and the HTTP ABI stubs on; the
Python bindings off). `cmake_lib` takes a second pinned tarball as a leading
`+<tarball>=<sha256>`, checked like the first, unpacked into the source top
after the patches and carried in the reuse stamp: here the W3C XML
conformance suite (`[cache.xmlts]`), unpacked as `xmlconf/` the way
upstream's CI image installs it, which is what registers `runxmlconf`. The
port's patch 0001 restores the two CMake test scripts the release tarball
omits. The muster half also stages libxml2's `test/`, `result/` and
`xmlconf/` trees whole (merged by tar: the stage list already put some of
their files there), since runtest, runsuite and runxmlconf walk them rather
than naming their files; `tools/build.sh libxml2` builds it alone,
`tools/forage.sh libxml2` gathers both tarballs.

**FreeType and HarfBuzz (B-3f).** Both register their tests with meson
alone. Meson is pinned at 1.12.1 (`[cache.meson]`) and run as `meson.py` from
its tarball, which `meson_tool` unpacks under `build/meson`: the host has no
meson of its own. `meson_lib` is `cmake_lib`'s contract in meson's terms. The
pinned tarball is unpacked fresh with the port's patches
(`usr/ports/<project>/patches`, the project named by the tarball's top
directory, so FreeType's two passes share one series), built static with
upstream's tests and installed at `<name>/stage`. Its reuse stamp covers every
input by content: the tarball, each patch, meson's version, the cross file's
text, the options, and the archives and `.pc` files of each dependency
directory (the sysroot's among them, so a new libc relinks the tests). The
cross file is `cmake/Toolchain-aarch64-pouch-cxx.cmake` in meson's terms. The
toolchain flags sit in the compiler exelist, because meson's header and
function checks drop an `-isystem <dir>` pair from `c_args`, and with
`-nostdlibinc` that left FreeType's checks no `unistd.h` (measured, as
`tools/clade-mesa-cross.sh` had measured for Mesa). It sets
`cmake = 'false'`, since a dependency pkg-config cannot find would otherwise
be looked for among the host's own CMake packages (measured). Only the named
pkg-config directories are searched, under `--wrap-mode=nofallback`, so
nothing comes from the host or from a subproject download.

FreeType builds twice around HarfBuzz, as distributions build it:
`freetype-nohb`, without tests, for HarfBuzz to link, then `freetype` with
HarfBuzz and its tests, which is the one the browser links. Both have PNG
from B-3d's libpng and zlib from the sysroot, with brotli and bzip2 off.
HarfBuzz keeps upstream's `debugoptimized` with `b_ndebug=if-release`, so its
tests' and the fuzzer driver's asserts stay live; ICU and FreeType are on, and
GLib, gobject, cairo, chafa, the docs and introspection off. ICU's `.pc` files
name `-L/lib`, because it installs with prefix `/` under `DESTDIR`, so
HarfBuzz is handed copies naming the stage (`lib/pkgconfig-staged`).
`build_zlib` writes zlib's own `zlib.pc` into the sysroot, which libpng's `.pc`
requires. `cmake_lib`'s stamp includes the sha256 of each archive an option
names, so libwebp's tests relink when libpng's bytes change, and a
byte-identical rebuild does not cascade. issue-1063's font comes from
`[cache.ftfont]`, checked by the
zip's sha256 and by the font's own, whose first 8 bytes are the digest
upstream's script pins. It grants no licence (the sites that list it say
personal use only), so an image baked with `CHUNK_LIBTESTS` stays on the
builder's own machines and is never published.

The muster half lists the two libraries' tests with `meson introspect
--tests` after it builds each tree's `meson-test-prereq`, as `meson test`
does before it runs them (`mtest.py`'s `rebuild_deps`): a target only a test
depends on, such as issue-1441's two generated fonts, is outside `all`, and
the translator refuses a test whose depends are not built or not staged
([[sub-substrate-libtests]]). The muster half also stages HarfBuzz's
`perf/fonts`, `perf/texts` and `test/subset/data/fonts` for the thread tests.
bake-verify adds `FTTESTS`, `FTFONT`, `HBTESTS` and `HBFONTS`.
`tools/build.sh fonts` builds the three passes and fontconfig, and
`tools/forage.sh fonts` gathers meson, the four tarballs and the font.

**fontconfig (B-3g).** `build_fontconfig` builds fontconfig 2.18.3
(`[cache.fontconfig]`) after the final FreeType and libxml2, against the
`.pc` directories of FreeType, HarfBuzz, libpng and libxml2. fontconfig
compiles in its config, cache and template directories from its prefix, so
`meson_lib` takes `--prefix /usr`: meson is configured with the device's
prefix (its sysconfdir is then `/etc`, its localstatedir `/var`), and the
install runs under `DESTDIR=<stage>`, which meson also applies to those
absolute directories, so the stage holds `etc/`, `usr/` and `var/` and
nothing is written outside it. The reuse stamp names the prefix. The options
follow `dec-2026-10-21-fontconfig-layout-dejavu`: the libxml2 backend,
`default-fonts-dirs=/lib/font`, `additional-fonts-dirs=no` (`yes` would
probe this host's X11 directories), and NLS, iconv, the docs, bwrap, the
external test fonts, fontations and `cache-build` off (`cache-build` runs
the cross-built `fc-cache` at install). The port's one patch names two
tests' data (`SRCDIR`, `FONTFILE`) relative to the build root, where the
tests run on the host and on the device alike, instead of the build host's
absolute source path. The muster half adds fontconfig to the meson
libraries, stages `test/4x6.pcf` for those two tests, skips the pytest suite
by name when the build host has pytest (meson registers it only then), and
lists `test_d1f48f11` as not run (`dec-2026-10-21-d1f48f11-not-run`; its
fixture needs `symlink()`). `stage_font_runtime` then stages what fontconfig
reads on the device into the library-test stage: `/etc/fonts` with
`conf.d`'s relative links into `conf.avail` kept as links (each checked to
resolve), `/usr/share/fontconfig`, `/usr/share/xml/fontconfig`, the stripped
`fc-*` tools in `/usr/bin`, and `/lib/font`. `/lib/font` is the vendored
DejaVu Sans Condensed and IBM Plex Sans faces plus DejaVu Sans, Serif and
Sans Mono from `[cache.dejavu]`, the release the vendored faces came from:
the tarball is checked by its sha256 and the four vendored faces must be
byte-identical to its copies. Each family's licence text goes beside its
faces. `var/` is never staged: there is no system font cache
(`dec-2026-10-21-no-system-font-cache`). It writes `fonts.count` and
`confd.count` for `ls-libs.exp`. The populate puts all of it after the
blocks that make `/etc` and `/lib`, makes `/usr`, `/usr/share` and
`/usr/share/xml`, and reads back every `conf.d` link; bake-verify adds
`FCTESTS`, `FCDATA`, `FCCONF`, `FCAVAIL`, `FCDTD`, `FCMATCH`, `FONTSANS`,
`FONTSERIF`, `FONTMONO` and `FONTPLEX`. All of it is baked only with the
library tests (`CHUNK_LIBTESTS`), their one consumer until the browser.

## Data structures

`build/` layout: `kernel/` and `kernel-undefined/` (parallel sanitizer
trees, so the production CMake cache is never clobbered), `sysroot/`,
`go/goroot/`, `clade/stage/`, `fixtures/{pool.img,system.key}` + their
`.baked-snapshot` twins, `ramfs.cpio`, `disk.img`.

## Concurrency

None internally. Two worktrees build concurrently without interference
because every path is repo-root-relative — the shared resource is the host,
not the tree.

## Invariants enforced

None of §28. It *produces* the artifacts several are checked against, and
one build-time property is load-bearing for I-12 (W^X — no registry note
yet; its surface is unswept): the ELF loader rejects W|X, so a segment
layout that would violate W^X fails at exec, not at build.

## Error paths

CMake / cargo / clang failures propagate (`set -euo pipefail`). The
significant *non*-error is the silent skip: several stages return 0 having
built nothing when an optional toolchain is absent — which is why the remote
builders assert on artifacts rather than exit status
([[sub-substrate-builders]]).

## Performance

The sysroot is ~1–2 min; the Go GOROOT and Clade stages dominate a cold
build. The cache checks exist to keep an incremental `all` in the tens of
seconds.

## Prosecution

- A new source directory feeding a cached artifact must be added to the
  corresponding `*_is_stale` watch list, or the artifact silently ages
  (compiler-rt is the worked example).
- A new bake payload must be verified at the chokepoint, not assumed from a
  flag (#101).
- Any new mutable fixture needs a `.baked-snapshot` twin, or the harnesses
  cannot restore it and it will contaminate (#85).
- The pool/key coupling must not be split; a target that re-bakes one alone
  reintroduces `STM_EBADTAG`.

## Seams

[[seam-87-disk-write-proof]] — `disk.img` has no build-maintained twin;
LS-CI mints one with `mkdisk.py` at need.

## Caveats

- ~~The header comment block is the most accurate documentation of the target
  chain and is actively maintained — prefer it to any prose elsewhere.~~
  **THAT ADVICE IS NOW WRONG, AND THIS DOSSIER GAVE IT.** The header is
  still the best account of *what each target it names does* — the caching
  footguns, the pool/key coupling, the summary contract are all there and
  all correct. But as a *list*, it is the least complete of the three: a partial
  list against the dispatcher's complete target set, omitting working
  targets including Clade stages.

  The failure is worth more than the correction. The claim was true when
  written and decayed without anything failing, because a target added to
  the dispatcher works perfectly whether or not the header mentions it —
  there is no build error, no test, and no user complaint, since the people
  adding targets already know they exist. The only reader who pays is the
  one who does not, and they cannot tell an omission from an absence. That
  is why the recommendation was the dangerous part: it routed exactly that
  reader to the list most likely to be short. Task #180.
- `THYLACINE_MKFS_PRESERVE=1` skips populate entirely, so new pool content
  needs a one-time `PRESERVE=0`. A runtime "file absent" for something you
  believe you baked is that, and it has produced at least one gate that
  reported PASS having verified nothing.

- **The `corvus-mint` host tool builds from the repo ROOT, not `usr/` (H-4d).**
  Cargo's config discovery is cwd-based: launched from `usr/`, the workspace's
  vendor replacement applies and its `aegis 0.9.8` cannot satisfy the tool's own
  `0.9.12` lock -- a bake started there fails on the mint. The fix is the cwd,
  not a version bump: run it from the root, where the vendor replacement does not
  reach. (The `/lib/halcyon/layouts/default` populate step H-4d also adds is one
  more baked file in the `/lib/beacon/verbs` shape -- mkdir + write + sync +
  readback-cmp -- below this file's target/ledger granularity, so no target-set
  change.)

- **`/manual` is baked unconditionally, and its EXISTENCE is verified even when
  empty (2026-09-16, docs/MANUAL-DESIGN.md 6).** The pool step after the TH-5
  themes block writes every `docs/manual/NN-<name>.md` to `/manual` with the
  themes block's write + sync + readback-cmp shape, globbed so a new section ships
  by existing. It then `stat`s `/manual`, because `manual` reads an ABSENT
  directory as "no sections installed", so a silently failed mkdir could be
  indistinguishable from a deliberately empty catalogue. Six checked sections
  now ship: Manual, Remote files with Haul, View, Gallery, Nocturne and DOSBox-X. The `manual` binary rides
  `usr_rs_bins` like `view`, as does `heap-probe` (B-1c, the native heap's EL0
  witness). Same granularity note as above: no target-set change.

- **The stale-stage warning claims a property it achieves by maintenance,
  not by construction — and its own comment is the argument against
  itself.** The comment says it is *"checked for EVERY staged GL binary, not
  just the one that caught it: the trap is a property of the staging step,
  so a name-by-name check would go quiet again the moment a binary is added
  — which tyr-glquake then was."* The loop two lines below is a name-by-name
  check of four.

  It is **complete today** — the staged set and the watched set were diffed
  and match exactly, with nothing staged-but-unwatched — so the sharper
  finding ("a fifth will go quiet") would have been wrong, and checking is
  what stopped it being filed. What is true is weaker and still real: the
  set is maintained by hand in two places, and adding a binary takes two
  edits with nothing failing if only one is made. The failure mode is a
  warning that does not print, which is the quietest outcome in the file.

  One line from safe-by-default, because the staging step already computes
  the authoritative set. Task #181.

## Provenance

[[chg-2026-08-01-substrate-sweep]].

[[chg-2026-08-15-build-targets]] is the re-sweep: the target set nearly
doubled, the patch-hunk chokepoint and the stale-stage warning arrived, and
this dossier's own "prefer the header block" advice was falsified.

[[chg-2026-09-05-h4d2-family-fold]] folds H-4d-2/3: the `corvus-mint`-from-root
trip-hazard, and a note that the `/lib/halcyon/layouts/default` populate step is
below the target/ledger granularity (no target-set change).

[[chg-2026-09-06-9p-identity-absorb]] folds the A-3 bake-value pass absorbed from
docs/reference/100: the `PRINCIPAL_SYSTEM` no-brick stamping (`stratum-mkfs
--root-uid` + `stratumd --bake-owner-uid`).
