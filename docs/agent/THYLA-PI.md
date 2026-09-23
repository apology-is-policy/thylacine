# The thyla-pi host

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). 
> Section headings keep their original levels.

## The thyla-pi host (permanent ARM64 / KVM / GPU box)

A Raspberry Pi 400 — BCM2711 (4× Cortex-A72 @ 1.8 GHz), 4 GB RAM, VideoCore
VI / V3D 4.2 GPU; Debian 13 arm64; QEMU 10.0.11 (distro) with
`virtio-gpu-gl-pci` + `egl-headless`; `expect`; `/dev/kvm` and
`/dev/dri/renderD128` accessible to user `cora` — is **permanently online**
(user commitment 2026-08-12) and available to every instance for anything
that benefits from real ARM64 silicon. No reservation protocol; keep QEMU
single-flight (verify no stray: `ssh thyla-pi 'ps -eo pid,args | grep
"[q]emu-system"'` — bracket-trick the pattern or it matches its own wrapper).

**Access**: LAN `ssh thyla-pi` (thyla-pi.local, user `cora`, key
`~/.ssh/thyla-pi`). From anywhere: `ssh thyla-pi-cf` (Cloudflare tunnel via
`thyla-pi-ssh.treeso.net`; cloudflared ProxyCommand — both aliases live in
`~/.ssh/config`; the Pi-side connector is a systemd service). After changing
cora's groups, drop the control master: `ssh -O exit thyla-pi`.

**Roles**:
- **The KVM GL host** (Warp arc): `WARP_HOST=thyla-pi WARP_ACCEL=kvm
  tools/warp-host.sh <sync|smoke|capset|prove|tri|bench|quake|decomp|wedge|wedge-gate>`.
  Real-silicon KVM (`-cpu host -gic-version=host`, auto-detected by
  run-vm.sh on Linux-aarch64 + rw /dev/kvm) boots the full gauntlet in
  ~210 s where Pi-TCG never finishes. Real GPU: virgl on V3D 4.2.14 (first
  Thylacine GL-on-silicon 2026-08-11; gl-host-probe rung 6 PASS) and a
  Vulkan V3D ICD (the Warp-6/Venus prerequisite).
- **Real-silicon SMP / memory-model witness**: the only non-Apple ARM
  hardware in the loop (#214 was closed on it); A72 vs M2 diversity.
- **General ARM64 Linux box**: native aarch64 builds, KVM guests, ad-hoc.

**Layout**: repo sync at `~/projects/thylacine` (push with `WARP_HOST=thyla-pi
tools/warp-host.sh sync` — git-archive of HEAD + boot artifacts + the pool via
sparse gzip; uncommitted tool scripts ride separately, re-scp after editing).
Working fixtures + logs in `~/warp/`.

**Care**:
- 4 GB RAM: ONE 2048 MiB guest at a time.
- SD-card I/O is the bottleneck — FS-round-trip-heavy guest work (go builds)
  dominates wall clock; budget bounds off the ~210 s banner, not TCG numbers.
- The Pi's `build/` holds the **certified artifact set of the last sync**
  (md5-stable). It has served as the bit-exact restore source after a local
  bake clobbered the fixtures: reverse-sync (`ssh thyla-pi 'gzip -1 -c
  .../pool.img' | gunzip | dd of=... conv=sparse bs=1m`) + md5 both sides.
- Artifacts pair cryptographically: `pool.img` + the key-bearing `ramfs.cpio`
  ship TOGETHER or the guest gets `STM_EBADTAG` (stratumd `rc=-201` ->
  `EXTINCTION: joey exited non-zero`). **A reverse-sync recovery is only valid
  if you sync BOTH and DO NOT rebuild** -- a rebuilt ramfs carries a fresh key
  that no longer matches a reverse-synced pool. If a real code change forces a
  rebuild, re-bake BOTH paired with `THYLACINE_MKFS_PRESERVE=0`, never `=1`.

---
