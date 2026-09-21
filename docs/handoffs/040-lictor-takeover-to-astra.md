# Handoff 040 -- the lictor takeover, handed back (2026-09-21)

**Tip**: `a6dddb47` on `main`, both mirrors verified by `ls-remote`. Tree clean.
**From**: Claude (Fable 5.1), main track. **To**: whoever continues -- written
for Astra (Codex), who cannot read Claude's private memory directory, so
everything needed is here or in the files this names.

Read in this order: this file; `docs/JOURNAL.md`, the 2026-09-21 entry and its
two addenda (the narrative, the wrong turns, what caught them);
`vault/system/userspace/services/sub-lictor.md` (as-built + Caveats);
`memory/audit_lictor_closed_list.md` if present in the checkout (untracked,
repo-local: the do-not-re-report list).

## What is in main now

Your 09-17..18 work on the graphical Lex curiata -- `usr/lictor`, the kernel
seat machine (`g_seat`, `proc_seat_op`), `SYS_TRUSTED_SEAT` 121 /
`SYS_SEAT_IMPORT` 122 / `SYS_SET_NONBLOCK` 123, the three seat spawn roles --
committed as `db88cf71`, reviewed and fixed in `817c2339` (1 P1 + 3 P2;
Ctrl+Alt+F10 as a second attention chord, scanned in the KERNEL), plus
`0c33ecd9` (`HALCYON_PROFILE` build option, Instrument the default), the
pre-merge matrix (`20979b6a`, `20f73f27`, `3efa6d77`, `a5e453be`), the merge
`6b5dad04`, `bfc72798` (quaestor) and `a6dddb47` (a ratification note).

Guest changes of mine inside your code, so you are not surprised by them:

- `usr/lictor/src/fence.rs` + `backend/gpu.rs`: the 32-bit fence id was doing
  two jobs. It is two sequences now -- a WIRE id that rewinds to 1 past 2^31
  when the device holds no chain, and an OWNER id (u64, never reused) that the
  compositor's readback equality match sees. The batched pair takes its second
  id as BUSY so it cannot rewind under the first. Test builds start 64 short
  of the rewind; `ls-graphical-sak` asserts the witness line.
- `usr/lictor/src/backend/device.rs`: `PairProtocolSelftest` is refused without
  `test-mode` (lictor did not compile without the feature).
- `usr/netd/src/server.rs` `tcp_admit`: at the 64-transport bound the oldest
  TIME-WAIT retiree yields to a new admission; nothing carrying bytes ever
  does. Operator-RATIFIED 2026-09-21
  (`dec-2026-09-21-timewait-yield-ratified`). `usr/netperf` retries admission
  in the phase after the churn.
- `kernel/syscall.c`: `sys_seat_import_for_proc` split out so the gates are
  testable; `kernel/proc.c`: `proc_test_seat_expire` (KERNEL_TESTS only).
- Four kernel tests: `devsrv.seat_import_gates`,
  `sys_spawn_with_perms.seat_roles`, `cons.graphical_seat_deadline_and_death`,
  `cons.graphical_seat_service_death`. Suite 1577/1577.
- `usr/tapestryd/src/main.rs`: the idle-throttle witness is `cfg(test-mode)`.

## Verified -- do not re-run to re-confirm

ci fleet (76 scenarios) green; every lever gate green on its own bake; five
sabotage controls each fail exactly their assertion; `tools/ci-smp-gate.sh`
40/40, 0 corruption; `cargo check --release --no-default-features` clean for
lictor (`--features backend`), tapestryd and halcyond (`--features guest`);
`ls-graphical-sak` PASS on thyla-pi under KVM with `virtio-gpu-gl-pci`
(virgl=1 ctxinit=1), 161 s; the operator's image (bare `tools/build.sh kernel`
in the main checkout) passes `ls-halcyon-session-instrument` first attempt.

## One image per gate's lever (the trap that cost the most time)

| Gates | Bake |
|---|---|
| the shell-driving fleet, `ls-ci`, `ls-gfx-panes`, `ls-gfx-throttle`, ... | `tools/build.sh kernel --config ci` |
| `ls-graphical-sak`, `-states`, `-recover`, `ls-halcyon-session-media`, `-dosbox`, `ls-halcyon-session-instrument` | `THYLACINE_HALCYON_SESSION=1 THYLACINE_HALCYON_PROFILE=instrument tools/build.sh kernel --config ci` |
| `ls-gfx-session` (a LEGACY-session gate), `ls-gfx-session-image` | `THYLACINE_HALCYON_SESSION=1 tools/build.sh kernel --config ci` |
| `ls-halcyon`, `ls-gfx-gallery`, `ls-gfx-inline-view`, `ls-gfx-jpeg` | `THYLACINE_HALCYON=1 THYLACINE_HALCYON_SESSION=0 THYLACINE_HALCYON_PROFILE=legacy tools/build.sh kernel --config ci` |
| `ls-halcyon-instrument` | same with `THYLACINE_HALCYON_PROFILE=instrument` |
| the operator's image | bare `tools/build.sh kernel` |

A gate on the wrong image now SKIPs (exit 77) with its recipe. `build.sh
kernel` regenerates the pool AND its key: never sync to the Pi, or run a
second gate in the same tree, while a bake or `ci-smp-gate.sh` is running.

## The Pi

`WARP_HOST=thyla-pi-cf tools/warp-host.sh sync` (the LAN alias hangs; use the
`-cf` one). It ships `git archive HEAD` -- commit first. Your 09-18 rc=-201 was
not your fixture: the sync shipped pool + ramfs but not the restore twins
LS-CI boots from (`pool.img.baked-snapshot` + the key twins), so the Pi
restored a stale pool over the synced one. Fixed and verified end to end
(`vault/system/substrate/sub-substrate-remote-host.md`). The tunnel closes
long ssh sessions from the far end: run remote gates detached
(`nohup setsid ... > log 2>&1 < /dev/null &`) and poll. Accelerated SAK run,
from the remote repo: `THYLACINE_GPU_DEV=virtio-gpu-gl-pci
THYLACINE_DISPLAY=egl-headless THYLACINE_EGL_VNC_SOCKET=/tmp/sak-vnc.sock
LS_CI_JOBS=1 tools/test-interactive.sh ls-graphical-sak`.

## Queue, in order

1. **`/srv` is at 15 of 16 slots in a one-user session.**
   `kernel/include/thylacine/devsrv.h` `SRV_MAX_SERVICES 16`; a dead poster's
   name holds its slot forever; there are 11 resident services now (lictor
   took the headroom from 2 to 1), plus two boot-probe tombstones, plus two
   per logged-in user. A third username cannot get a home and `haul --post`
   cannot post once two users are in. Preferred cure: free an entry at its
   last handle ref (the header's own "recorded v1.x seam"); interim: raise to
   32 WITH `srv_registry_drain` made per-entry so its kernel-stack cost stops
   scaling. devsrv is an audit-trigger surface: scripture note first. Add a
   gate that logs in three usernames.
2. HOLD THE VIEW in halcyond's Normal mode (operator-decided 09-16): appended
   output must not move the rows under the cursor/pointer. It also retires
   `ls-halcyon`'s click-leg row counting.
3. The I-8 review's P1 (tapestryd's weave VA bump; `usr/tapestryd/src/va.rs`
   is built and unwired) + 7 P3; then the compositor transitions (three gates,
   including `THYLACINE_IDLE_STRICT=20 tools/ci-idle-gate.sh`).
4. Unread from your PCI interrupt work (I read the kernel side end to end --
   0 P0/P1/P2, three P3s in the closed list): `PciIrq::for_virtio` in
   libthyla-rs, the netd/GPU/nocturned migrations, `pci_handle.c`'s BAR
   placement and claim path, `virtio_pci.c`. `CAP_POST_SERVICE` residual: a
   cap-poster may take a TCB name that has NEVER been posted (another user's
   `halcyon-<user>` before their first login).
5. Lictor P3s: `test-mode` is a default cargo feature everywhere (the #880
   strip-for-production class); a display under 800x720 refuses the seat; the
   100 Hz loop's idle cost is unmeasured; warden never reaps the compositor
   (a dead lictor is a dark display until reboot); no blurred backdrop; no
   Pi 400/500 qualification.
6. Small: tapestryd's startup read of `/lib/halcyon/{profile,theme.toml}` can
   never succeed (warden spawns it before joey pivots to the pool; only
   halcyond's push aligns the compositor -- sub-tapestryd Caveats); `ut`
   prints nothing for a command that fails to spawn; `ut`'s post-spawn
   foreground handoff race (`run_foreground_jc`).

## House rules that bit during this run

- Stage by name from `git status`; never `git add -A`. `quaestor render`
  before committing; the record plane (`vault/record/`) is append-only -- a
  correction is a NEW note. Code owned by an `audit: hard` dossier needs the
  dossier co-staged or a `No-dossier-change: <why>` trailer.
- Push each mirror BY URL, guarded on `git merge-base --is-ancestor`, verify
  with `ls-remote`, never force. `SSL_CERT_FILE=/etc/ssl/cert.pem` for git
  network ops on this host.
- Every expect capture that ends its pattern with a number is anchored
  (`\r`, or the literal that follows). On a console-renderer image every
  daemon witness line is a transcript row. `expect`'s `timeout 0` never reads
  the pty. (`sub-substrate-interactive`, the five authoring rules.)
- A kernel test that returns early through `TEST_ASSERT` leaks its state into
  the next test: use goto-cleanup.
- `tools/halcyon-glass/backgrounds/bliss.png` is Microsoft's; it stays out of
  the mirrored repo. The glass study is preserved on `codex/halcyon-glass`.
