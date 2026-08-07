# GPU-HOST-SETUP.md — the Linux GL host for the Warp arc

> Companion to `docs/GPU-DESIGN.md` §9.1. That section establishes *why* this
> exists: virgl cannot run on a macOS host (structurally — `ui/cocoa.m` never
> sets `display_opengl`, `egl_init()` has no macOS branch, and Homebrew's QEMU
> does not contain `virtio-gpu-gl` at all), and Venus additionally requires a
> Linux host outright. This document is the runbook for standing up that host as
> a local Parallels VM.
>
> **STATUS: EXECUTED AND VERIFIED, 2026-08-07.** The host exists
> (`Thylacine-Debian`, Debian 13 trixie ARM64, QEMU 10.0.11) and
> `tools/gl-host-probe.sh` returns **PASS at rung 6** — QEMU realises
> `-device virtio-gpu-gl` on `-display egl-headless`. **F1 is resolved in favour
> of the local Parallels VM; the Warp arc is unblocked.** Two setup facts
> that were not in the original proposal are recorded in §4.1; read them before
> rebuilding this host or standing up a second one.
>
> **The host GL is `virgl (Apple M2 (Compat))`** — Parallels' own Linux 3D
> acceleration is virgl-based, so the stack is virgl-inside-virgl with a **real
> Apple M2 GPU at the bottom**, not llvmpipe. §7's "one curiosity" is therefore
> confirmed fact. Consequence: our guest's GL is genuinely hardware-accelerated
> two virtio-gpu hops down, which is better than the CI-shaped fallback would
> have been — but when something renders strangely, remember there are two
> translation layers to bisect, not one.

---

## 0. Space, first — the measured numbers

| | |
|---|---|
| Free on `/System/Volumes/Data` today | **27 GiB** |
| `Windows 11.pvmz` actual on-disk | **24 GB** |
| Free after archiving it off | **~51 GiB** |

That is enough, but not lavish. Budget for the Linux VM:

| Item | Size |
|---|---|
| Debian 13 ARM64 + XFCE, base install | ~5 GB |
| QEMU + Mesa + virglrenderer + build essentials | ~3 GB |
| Our guest artifacts (`disk.img` 16 MB + `pool.img` **906 MB actual**, 5 GB sparse) | ~1 GB |
| Working headroom (logs, extra pool fixtures, a Mesa tree if we ever build one locally) | ~10 GB |
| **Provision the virtual disk at** | **48 GB expanding** (uses only what it touches) |

Note the pool image is **sparse** — 5 GB apparent, 906 MB real. Anything that
copies it must preserve that (`rsync --sparse`, `cp -c` / `cp --sparse=always`);
a naive copy inflates it 5×.

**Do not start until the Windows image is off the internal disk and `df -h
/System/Volumes/Data` confirms the space.** A build or a QEMU run that hits
ENOSPC on macOS is a recorded way to brick this session's tooling.

---

## 0.1 AS-BUILT: the host is RAM-constrained, and it changes the method

The Mac has **8 GB total**, so the VM gets **4096 MB / 4 CPUs** — not the
12 GB/6 the proposal assumed. Measured in-guest: 3912 MB usable, 4 CPUs.

**Footprint arithmetic.** QEMU with `-m 2048` lands around 2.6–3.0 GB resident
(guest RAM + the TCG translation cache + virglrenderer and Mesa in-process);
Debian minimal is ~200 MB. That is ~3.2 GB of 3.9, leaving ~700 MB.
**Run headless.** An XFCE session adds 600–800 MB and puts the OOM killer in
reach — and mid-benchmark it kills QEMU rather than slowing it. Bring the
desktop up only when watching:

```bash
sudo systemctl set-default multi-user.target   # headless by default
sudo systemctl isolate graphical.target        # desktop on demand
```

(Left at `graphical.target` as shipped — that is the user's screen to decide.)

**Swap, as built** — three tiers, deliberately ordered:

| Device | Size | Prio | Role |
|---|---|---|---|
| `/dev/zram0` | 768 M zstd | 100 | first tier; compressed in RAM, no disk I/O |
| `/swapfile` | 4 G | 10 | second tier backstop |
| `/dev/sda4` | 1.9 G | -2 | the Debian installer's own partition |

plus `vm.swappiness=10`. **Swap here is an OOM airbag, not capacity.** QEMU's
guest RAM is anonymous memory; a swapped-out guest page turns a 60 fps render
into multi-second stalls, so the goal is never to touch it. Corollary worth
enforcing: **a benchmark run that swapped is a measurement of swapping** —
sample `/proc/vmstat` `pswpin` before and after any timing run and discard the
result if it moved.

Two traps met while building this, both recorded so they are not re-derived:
`zram-tools` will not reconfigure a `zram0` that is already swapped-on
(`systemctl enable --now` silently leaves the old geometry — swapoff + `echo 1 >
/sys/block/zram0/reset` first), and sizing zram at the tool's percentage default
(50% = 1.9 G here) is actively wrong on a RAM-constrained box, where its
worst case competes with the very process it exists to protect.

### The measurement caveat — read before quoting any fps number

The **192.8 fps unpaced llvmpipe anchor was measured on macOS under HVF with
`-smp 4`** (`docs/reference/143-tyrquake.md`, #165). This host is **TCG inside
Parallels with 4 vCPUs and 4 GB**. Those are different machines. Comparing
`virgl here` against `llvmpipe there` would measure the hosts, not the
renderers.

**The llvmpipe baseline must be re-measured on this box** before any virgl
figure means anything, and both numbers must be quoted with the host attached.
This is the proximity-beats-provenance trap in its natural habitat: the old
number is right there in a document that looks exactly like the one being
written, and it is not a reading of this machine.

**AS-MEASURED at Warp-1 (three runs; `build/warp1-bench-run{1,2,3}.log`)**:
llvmpipe GLQuake on this host is the band **2.4–5.9 fps** — single-digit,
with real run-to-run drift and a 3/3-reproducing within-boot degradation
(the third demo of a boot collapses to ~2.4–2.9 fps) that per-demo swap
counters PROVED is not swapping → **#168, UNEXPLAINED** (macOS/HVF
multi-demo boots show no such decay). Two guard rules came out of it:
sample the swap counters **around each timed figure, not each run** (a
guard must certify at the granularity of the figures it stamps — runs 1–2
discarded healthy demos alongside the poisoned one), and treat any Warp-4
virgl comparison as valid only if it beats single digits decisively or
#168 is resolved first.

---

## 1. Distro: Debian 13 (ARM64) + XFCE — and why not Alpine

Alpine is the instinctive choice and the wrong one here. The job of this VM is
to be a *well-trodden graphics host*, and that is the one thing Alpine is not:

- Debian ships `qemu-system-arm` **linked against virglrenderer**; on Alpine you
  would first have to establish whether the packaged QEMU was built
  `--enable-virglrenderer` at all, and rebuild it if not.
- Mesa's own CI — the configuration that demonstrably runs virgl on GPU-less
  machines — is Debian-based. Matching it means our failures are *their* known
  failures, with answers already written down.
- Parallels Tools guest support (which is what makes `prlctl exec`, clipboard,
  and dynamic resolution work) is documented and tested for Debian/Ubuntu.
- Alpine is musl. We already spend real effort fighting musl inside Pouch; there
  is no reason to invite it onto the host side of the boundary too, where its
  only reward would be a smaller VM on a disk that has room.

XFCE rather than GNOME: we need *a* desktop so that `-display gtk,gl=on` has
somewhere to put a window and so we can watch GLQuake, but nothing more. XFCE is
~1 GB and does not itself want a GPU.

Debian 13 ("trixie") ARM64 netinst ISO, `arm64` — the Parallels installation
assistant will also offer to fetch Debian or Ubuntu directly, which is fine and
saves a download step.

---

## 2. Create + configure the VM

Create it through the Parallels GUI (the assistant handles the ARM64 ISO and the
Tools install cleanly; `prlctl create` is aimed at templates and is not worth
fighting). Name it exactly **`thyla-gl`** — the ssh config and every script
below assume that name.

Then set the resources from the CLI. These flags were verified against the
installed `prlctl 20.1.3`:

```bash
prlctl set thyla-gl --cpus 6 --memsize 12288
prlctl set thyla-gl --video-adapter-type virtio    # <-- the load-bearing one
prlctl set thyla-gl --3d-accelerate highest
prlctl set thyla-gl --videosize 512
```

**`--video-adapter-type virtio` is the whole plan in one flag.** It presents a
**virtio-gpu** to the guest instead of the legacy Parallels adapter, which means
Linux binds its standard `virtio_gpu` DRM driver and creates
`/dev/dri/renderD128` backed by Mesa's virgl driver — exactly the render node
QEMU's `egl-headless` demands. If the §4 probe fails, this flag is the first
thing to re-check.

Leave 2 CPUs and the rest of RAM for macOS; you will still want to build on the
Mac while the VM runs.

Optional, useful: a read-only shared folder so the guest can see Mac-side files
without a copy.

```bash
prlctl set thyla-gl --shf-host-add mac-projects \
    --path /Users/northkillpd/projects --mode ro
```

Use it for convenience (dropping a log where you can read it, glancing at a
file). **Do not run QEMU against an image on a shared folder** — `prl_fs` has
awkward mmap and sparse-file semantics, and QEMU doing random reads of a 5 GB
image across it will be slow at best and wrong at worst. Images live on the VM's
own disk; see §6.

---

## 3. Guest packages

```bash
sudo apt update && sudo apt install -y \
    qemu-system-arm qemu-utils ipxe-qemu \
    mesa-utils mesa-utils-bin libgl1-mesa-dri libglx-mesa0 \
    libegl-mesa0 libgbm1 \
    mesa-vulkan-drivers vulkan-tools \
    libvirglrenderer1 virglrenderer-test-server \
    expect python3 rsync git openssh-server \
    build-essential pkg-config
```

**`ipxe-qemu` is load-bearing and the probe cannot see its absence** (found
at Warp-1 first boot): Debian splits QEMU's PCI option ROMs into that
package, and without it any `-device virtio-net-pci` fails at startup with
`failed to find romfile "efi-virtio.rom"`. The probe's rung 6 runs
`-nodefaults` with no NIC, so it passes on a host where the real
`tools/run-vm.sh` invocation (which carries the canonical net devices)
cannot start at all.

What each group is for: `qemu-system-arm` is the host QEMU (aarch64 target);
the `mesa-*`/`libegl`/`libgbm` set provides the host GL and the EGL/GBM layer
QEMU needs; `mesa-vulkan-drivers` brings **lavapipe**, which is the host Vulkan
driver the Venus leg will need at Warp-6; `virglrenderer-test-server` gives us
`virgl_test_server` for the vtest lane (§7); `expect` matches our interactive
harness.

If `virglrenderer-test-server` is not packaged under that name on trixie, the
binary may ship inside `libvirglrenderer1` or a `-tools` package — `apt-file
search virgl_test_server` settles it. It is optional for Warp-1; do not block on it.

---

## 4. The decisive probe — run this before anything else

`tools/gl-host-probe.sh` (written alongside this document) is the verification
ladder from `GPU-DESIGN.md` §9.1 turned into a script. Copy it into the VM and
run it:

```bash
./gl-host-probe.sh
```

It checks, in order and with an explicit verdict per rung:

1. a DRM render node exists (`/dev/dri/renderD*`)
2. the GBM and EGL libraries are present
3. the host GL stack actually initialises, and **what it is** (`glxinfo -B`)
4. `virtio-gpu-gl` is compiled into this QEMU
5. `egl-headless` is an available display backend
6. **the real test** — QEMU actually realises `-display egl-headless -device
   virtio-gpu-gl` without erroring
7. the interactive path (`gtk,gl=on`) works too
8. (informational) whether a Vulkan ICD is present, for the later Venus leg

The script is written to **fail closed**: any rung it cannot positively verify
reports `UNKNOWN`, and an `UNKNOWN` anywhere makes the overall verdict
`INCONCLUSIVE`, never `PASS`. This is the recorded discipline — a gate that
cannot parse its own evidence must not pass. Rung 6 is the one that decides the
arc; rungs 1–5 exist to tell us *why* if it fails.

### 4.1 The two things that actually blocked it (both found by the probe)

Neither was in the original proposal; both cost a probe cycle, and both are
invisible until QEMU refuses.

**1. Debian ships QEMU's OpenGL support as a separate package.** With
`qemu-system-arm` alone, `virtio-gpu-gl` is absent from `-device help` *and*
`egl-headless` is absent from `-display help` — the same symptom as "built
without virglrenderer," but the cause is packaging, not the build. The fix:

```bash
sudo apt-get install -y --no-install-recommends qemu-system-modules-opengl
```

QEMU says so itself if you read its error rather than the probe's summary
(`Perhaps you want to install qemu-system-modules-opengl package?`) — which is
exactly why rung 6 dumps the raw output when it cannot classify. The probe now
recognises this string.

**2. An ssh user is not in the `render` group, and the failure lies about it.**
`/dev/dri/renderD128` is `root:render 0660` with an ACL (`crw-rw----+`). logind
grants that ACL to the **active local seat session** — an ssh session gets
nothing. So the node is plainly present, `ls` shows it, rung 1 passes, and QEMU
still reports **`egl: no drm render node available`**, sending you to hunt for a
missing device that is right there. The fix:

```bash
sudo usermod -aG render "$USER"
ssh -O exit thyla-gl     # REQUIRED: ControlMaster reuses the old session,
                         # which still carries the old group set
```

The probe now tests **openability**, not just existence — the two are different
questions and only the second one is the one QEMU asks. This also gave the run
its discrimination proof: the identical QEMU command failed before the group
change and passed after, one variable, same binary, so rung 6's PASS is not
vacuous.

**Expected failure strings and what each means:**

| Message | Meaning | Fix |
|---|---|---|
| `'virtio-gpu-gl' is not a valid device model name` | QEMU built without virglrenderer | wrong package/distro — see §1 |
| `The display backend does not have OpenGL support enabled` | the display backend cannot do GL | try `sdl,gl=on` or `gtk,gl=on`; check libepoxy |
| `egl: no drm render node available` | no renderD*, OR present-but-unopenable (see 4.1) | check openability first; then `--video-adapter-type virtio` |
| `egl: not available on this platform` | EGL/GBM missing | install `libegl-mesa0 libgbm1` |

---

## 5. Reaching it: ssh for work, `prlctl` for control

Use **both**, with a clean division of labour. Neither alone is sufficient.

### ssh — the workhorse

Everything that is actual work goes over ssh, because `prlctl exec` cannot do
three things we need:

- **`rsync`** — delta transfer of the guest images, with `--sparse` preserving
  the 906 MB-in-5 GB sparseness. `prlctl exec` has no file-transfer channel.
- **Port forwarding** — `ssh -L` lets a Mac-side script reach a QMP socket or
  VNC display belonging to a QEMU running *inside* the VM, so our existing
  instruments (`screendump.sh`, the QMP samplers) work unmodified.
- **Clean long-running commands** — real stdio, real exit codes, no dependency
  on Parallels Tools being healthy.

It is also what the rest of our tooling already assumes: the GCP builder path is
ssh + rsync, so scripts generalise instead of forking.

Setup — one key, one config block:

```bash
# on the Mac
ssh-keygen -t ed25519 -f ~/.ssh/thyla-gl -N '' -C thyla-gl
ssh-copy-id -i ~/.ssh/thyla-gl.pub northkillpd@<guest-ip>   # ip from prlctl, below
```

Add to `~/.ssh/config`:

```
Host thyla-gl
    HostName        <guest-ip-or-thyla-gl.shared>
    User            northkillpd
    IdentityFile    ~/.ssh/thyla-gl
    ControlMaster   auto
    ControlPath     ~/.ssh/cm-%r@%h:%p
    ControlPersist  10m
    ServerAliveInterval 30
```

`ControlMaster`/`ControlPersist` matter more than they look: I will make many
short `ssh thyla-gl <cmd>` calls, and without connection reuse each one pays a
full handshake. With it, they are effectively free.

Parallels' shared networking gives the VM a DHCP address; read it rather than
guessing, and note it can change across host reboots:

```bash
prlctl list -a                       # IP_ADDR column
prlctl exec thyla-gl ip -4 addr show # authoritative, works even pre-ssh
```

If it drifts often enough to annoy, switch the adapter to bridged
(`prlctl set thyla-gl --device-set net0 --type bridged`) and give it a static
lease, or rely on the `.shared` mDNS name Parallels registers.

### `prlctl` — the control plane

Reserved for the things ssh structurally cannot do:

```bash
prlctl start thyla-gl
prlctl stop  thyla-gl --acpi
prlctl status thyla-gl

# snapshots -- the real reason to keep prlctl in the loop
prlctl snapshot thyla-gl -n "clean-debian-tools"
prlctl snapshot thyla-gl -n "gl-stack-installed"
prlctl snapshot-list thyla-gl -t
prlctl snapshot-switch thyla-gl -i <snapid>

# rescue: works when the network does not
prlctl exec thyla-gl -- systemctl status ssh
prlctl enter thyla-gl                      # interactive, no network needed
```

**Take a snapshot before each graphics-stack change.** Installing and
reconfiguring GL/EGL/DRM packages is one of the classic ways to end up with a
machine that no longer reaches its own display or network, and rolling back a
snapshot is far cheaper than diagnosing that. `prlctl exec` is the rescue hatch
when a change breaks networking specifically — it goes through Parallels Tools,
not the network stack.

---

## 6. Getting our artifacts across

Source and artifacts move separately, because they have very different sizes and
change rates.

**Source: clone, don't sync.** The repo minus `build/` is small, and the VM needs
`tools/` (the harness scripts) more than it needs anything else.

```bash
ssh thyla-gl 'git clone <origin-url> ~/projects/thylacine'
```

**Artifacts: `tools/warp-host.sh sync` (AS-EXECUTED at Warp-1 — supersedes
the rsync recipe below).** Build on the Mac as usual
(`THYLACINE_BAKE_CLADE=1 THYLACINE_MKFS_PRESERVE=1 tools/build.sh all`), then:

```bash
tools/warp-host.sh sync     # repo (git archive HEAD) + all four boot artifacts
```

Three as-executed corrections to the original plan:

- **macOS ships openrsync**, whose `--sparse` support is not dependable, so
  the pool travels as `gzip -1 | ssh | dd conv=sparse` — zeros compress to
  nothing on the wire and re-punch as holes on the VM disk (906 MB physical
  lands as 906 MB).
- **The artifact set is four files, not two**: `kernel/thylacine.bin` (what
  `run-vm.sh -kernel` actually loads — the ELF alone stops the launcher
  cold), `kernel/thylacine.elf` (gdb), `ramfs.cpio`, `disk.img`, plus
  `fixtures/pool.img`.
- **Working copies on the VM live in `~/warp/`, never `/tmp`** — Debian 13
  mounts `/tmp` as tmpfs, so a ~1 GB pool copy there would eat guest RAM
  out of the 3.9 GB that QEMU needs. `tools/warp/boot-probe.sh` does the
  per-attempt fixture isolation (#78/#85) on disk.

The original rsync form, kept for a host with real rsync:

```bash
rsync -av --sparse --inplace --info=progress2 \
    build/disk.img build/kernel/thylacine.bin \
    thyla-gl:~/projects/thylacine/build/kernel/

rsync -av --sparse --inplace --info=progress2 \
    build/fixtures/pool.img \
    thyla-gl:~/projects/thylacine/build/fixtures/
```

Measured at Warp-1: first full sync ~4 min over shared networking; boot to
`Thylacine boot OK` **~230 s** (TCG, `-smp 4 -m 2048`, NOSTORM), against the
~180 s LS-CI budget calibrated for native-M2 TCG — set `LS_CI_BOOT_TIMEOUT=900`
for anything harness-driven on this host.

**Keep the Mac as the build host.** The VM has no KVM (Apple only added nested
virtualisation on M3; this is an M2), so QEMU there runs TCG. Our
`tools/test.sh` uses HVF on macOS and is dramatically faster for the 1356-test
suite. The right split is: **build and run the normal suite on macOS; use the VM
only as the GL bench.** Do not migrate the whole loop into it.

---

## 7. What the VM buys us, in order

Once §4's rung 6 passes:

- **Warp-1 immediately** — `-device virtio-gpu-gl` reachable, capset probe from
  tapestryd, the CI plumbing. This is the chunk the whole arc was blocked on.
- **The vtest lane, free** — `virgl_test_server --use-egl-surfaceless` plus
  host Mesa with `GALLIUM_DRIVER=virpipe` lets us exercise our winsys logic as
  an ordinary host process before any guest wiring exists. Mesa's own CI runs
  this lane; it is the cheapest possible smoke test of Warp-3.
- **Warp-6 (Venus) becomes possible at all** — lavapipe as the host Vulkan driver.
  This is impossible on macOS by any route.
- **A visible screen** — the reason to prefer this over a GCP runner. `-display
  gtk,gl=on` inside the VM's XFCE session puts GLQuake in a window you can
  watch, drive with a real mouse, and A/B by eye.

One curiosity to hold in mind: if Parallels' own Linux 3D acceleration is itself
virgl-based (which `--video-adapter-type virtio` suggests), we will be running
**virgl inside virgl** — our guest's virglrenderer talking to the VM's Mesa virgl
driver talking to Parallels' host renderer. That is functionally fine, but the
stack is deeper than it looks; if something renders strangely or a capability
looks unexpectedly absent, remember there are two translation layers, not one,
and bisect accordingly.

---

## 8. If the probe fails

In order of what to try:

1. **Rung 1 fails (no render node).** Confirm `--video-adapter-type virtio` took
   effect (`prlctl list -i thyla-gl | grep -i video`), confirm Parallels Tools
   installed, and check `lsmod | grep virtio_gpu` / `dmesg | grep -i drm`.
2. **Rung 4 fails (no `virtio-gpu-gl` device).** The packaged QEMU was built
   without virglrenderer. Either use a distro that ships it (§1) or build QEMU
   with `--enable-virglrenderer`.
3. **Rung 6 fails but 1–5 pass.** Try `-display sdl,gl=on` and `gtk,gl=on`
   before concluding; `egl-headless` is the strictest of the three.
4. **Nothing works.** Fall back to the **GCP Linux leg** on `thyla-keep` — same
   Debian shape, no visible screen, and the render-node question recurs there
   (t2a instances have no GPU), so investigate `vgem`/software render nodes at
   that point. This is the F1 fallback recorded in `GPU-DESIGN.md` §10.

Whatever the outcome, record it in `GPU-DESIGN.md` §10 F1 as the resolved vote —
including a failure, which would be a real finding about the substrate rather
than a dead end.
