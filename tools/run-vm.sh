#!/usr/bin/env bash
# tools/run-vm.sh — canonical QEMU launcher for Thylacine development.
#
# Per TOOLING.md §3. Always launch the dev VM via this script — direct
# qemu-system-aarch64 invocations diverge and accumulate inconsistencies.
#
# Usage:
#   tools/run-vm.sh                  — boot kernel, interactive UART, Ctrl-A x to quit
#   tools/run-vm.sh --gdb            — expose GDB stub on :1234, halted at entry
#   tools/run-vm.sh --cpus N         — override vCPU count (default 4)
#   tools/run-vm.sh --mem M          — override RAM in MiB (default 2048)
#   tools/run-vm.sh --no-share       — disable VirtIO-9P host share
#   tools/run-vm.sh --snapshot NAME  — restore from snapshot (Phase 5+)
#
# Phase-specific notes:
#   - At P1-A: kernel boots, prints banner, hangs. No disk image yet.
#     Disk + 9P share + virtio-net flags are commented out below; uncomment
#     as the corresponding subsystems land in P1-D, P1-F, etc.
#   - At Phase 8: graphical flags (virtio-gpu-pci, virtio-keyboard-pci,
#     virtio-mouse-pci, -display sdl) get added when Halcyon needs a display.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Allow the kernel build dir to be overridden so sanitizer / debug variants
# (P1-I: tools/test.sh --sanitize=ubsan) can boot without clobbering the
# production build artifacts.
KERNEL_BUILD_DIR="${THYLACINE_BUILD_DIR:-$REPO_ROOT/build/kernel}"
KERNEL_ELF="$KERNEL_BUILD_DIR/thylacine.elf"
# Flat-binary form, with Linux ARM64 image header. QEMU's load_aarch64_image()
# detects the ARM\x64 magic at offset 0x38 and treats us as a Linux Image
# (is_linux=1), which causes it to load the DTB and pass its address in x0.
# An ELF load (is_linux=0) skips the DTB entirely. See docs/reference/01-boot.md
# "Caveats > DTB pointer observed as 0x0" for the investigation.
KERNEL_BIN="$KERNEL_BUILD_DIR/thylacine.bin"

cpus=4
mem_mib="${THYLACINE_MEM_MIB:-2048}"
gdb_flags=()
share_flags=()
snapshot=""
extra_qemu_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gdb)
            gdb_flags=(-s -S)
            echo "GDB stub on :1234. Connect with:"
            echo "  /opt/homebrew/opt/llvm/bin/lldb $KERNEL_ELF"
            echo "  (lldb) gdb-remote 1234"
            shift
            ;;
        --cpus)
            cpus="$2"
            shift 2
            ;;
        --mem)
            mem_mib="$2"
            shift 2
            ;;
        --no-share)
            share_flags=()
            shift
            ;;
        --snapshot)
            snapshot="$2"
            shift 2
            ;;
        --)
            shift
            extra_qemu_args+=("$@")
            break
            ;;
        *)
            extra_qemu_args+=("$1")
            shift
            ;;
    esac
done

if [[ ! -f "$KERNEL_BIN" ]]; then
    echo "Kernel flat binary not found: $KERNEL_BIN" >&2
    echo "Build first: tools/build.sh kernel" >&2
    exit 1
fi

# P4-E: ramfs cpio archive loaded by QEMU at boot via -initrd. The
# kernel parses it via lib/dtb.c (linux,initrd-start/end probe) +
# kernel/cpio.c iterator and populates devramfs. tools/build.sh kernel
# also runs build_ramfs which writes this file; we point at the
# build/-relative path (NOT the per-build sanitizer dir, since the
# cpio is build-config-independent).
RAMFS_CPIO="${THYLACINE_RAMFS_CPIO:-$REPO_ROOT/build/ramfs.cpio}"
ramfs_flags=()
if [[ -f "$RAMFS_CPIO" ]]; then
    ramfs_flags=(-initrd "$RAMFS_CPIO")
fi

# P4-Ic5b2: virtio-blk-device backing store. tools/build.sh disk
# generates build/disk.img (1 MiB, block 0 = "THYLACINE-DISK-1\0..." +
# zeroes). The userspace virtio-blk-probe binary issues VIRTIO_BLK_T_IN
# for sector 0 and verifies the signature; absence of the file produces
# a graceful skip in test_virtio_blk_probe.c (it sees no DeviceID=2
# slot via virtio_mmio_find_by_device_id and reports SKIP rather than
# spawning a probe that would block forever on t_irq_wait).
#
# Slot assignment on QEMU virt: virtio-mmio devices are added to slots
# in REVERSE order of their flag position (hw/arm/virt.c reads the
# device list back-to-front). We list virtio-blk-device FIRST so it
# lands in slot 31 (PA 0x0a003e00, INTID 79). virtio-rng-device follows
# and gets slot 30 (PA 0x0a003c00, INTID 78). The probe scans all 32
# slots, so the order is informational rather than load-bearing.
DISK_IMG="${THYLACINE_DISK_IMG:-$REPO_ROOT/build/disk.img}"
disk_flags=()
if [[ -f "$DISK_IMG" ]]; then
    # `-global virtio-mmio.force-legacy=false` flips the virtio-mmio bus
    # to its MODERN (Version=2) transport per VIRTIO 1.2 §4.2.2.
    # QEMU's default on the ARM virt machine is the legacy transport
    # (Version=1) for backward compatibility with kernels that pre-date
    # VIRTIO 1.0. Thylacine's userspace driver crates target the modern
    # transport exclusively (the legacy MMIO register layout is
    # different + uses GUEST_PAGE_SIZE / QueuePFN semantics that we
    # don't support).
    disk_flags=(
        -global virtio-mmio.force-legacy=false
        -drive "if=none,id=disk0,format=raw,file=$DISK_IMG,cache=writeback"
        -device virtio-blk-device,drive=disk0
    )
fi

# P6-pouch-stratumd-boot sub-chunk 16b-beta: second virtio-blk-device
# backing the boot system pool. tools/build.sh::build_stratum_pool_fixture
# pre-generates build/fixtures/pool.img (64 MiB, populated by host
# stratum-mkfs with the bootstrap pool + a single root inode in dataset
# id=1). stratumd is spawned by joey holding CAP_HW_CREATE, claims the
# virtio-mmio bank via bdev_thylacine.c (Stratum's thylacine-pouch-arm
# branch), finds this slot, mounts the pool, binds /srv/stratum-fs.
#
# Slot assignment: QEMU virt-machine assigns virtio-mmio slots in
# REVERSE creation order. pool_flags is listed FIRST so pool.img gets
# slot 31 (the "primary" virtio-blk slot); disk_flags follows so the
# test-disk lands at slot 30.
#
# bdev_thylacine.c scans HIGH-to-LOW so stratumd picks slot 31 (pool).
# The legacy virtio-blk-probe / virtio-blk-rw test binaries scan
# LOW-to-HIGH so they pick slot 30 (disk.img with the "THYLACINE-DISK-1"
# signature they expect). Two virtio-blk-devices, two scan directions,
# no collision.
POOL_IMG="${THYLACINE_POOL_IMG:-$REPO_ROOT/build/fixtures/pool.img}"
pool_flags=()
if [[ -f "$POOL_IMG" ]]; then
    pool_flags=(
        -drive "if=none,id=pool0,format=raw,file=$POOL_IMG,cache=writeback"
        -device virtio-blk-device,drive=pool0
    )
fi

# P4-Ja: virtio-net-device backing. QEMU's user-mode network (slirp)
# binds a host-side userspace TCP/UDP relay to a guest-visible NIC; no
# privilege needed (vs. -netdev tap which needs root + bridge config).
# The slirp gateway lives at 10.0.2.2; guest gets a DHCP-issued IP of
# 10.0.2.15; DNS at 10.0.2.3. The virtio-net-probe binary issues a
# broadcast ARP request "who-has 10.0.2.2 tell 10.0.2.15" purely to
# exercise the TX virtqueue path — it doesn't process the RX response.
#
# A network filter (-object filter-dump) is NOT installed by default;
# add via THYLACINE_NET_DUMP=path to capture frames for debugging.
#
# Slot assignment: this -device sits AFTER disk_flags in the exec
# invocation (QEMU virt assigns virtio-mmio slots in reverse creation
# order, so virtio-blk-device stays at slot 31; virtio-net lands at
# slot 30; virtio-rng-device drops to slot 29). The probe scans all
# 32 slots, so order is informational.
# NP-3 (M6): an OPTIONAL slirp guestfwd on net1 (netd's NIC -- the warden binds
# `virtio-pci:1` -> netd, which opens VirtioNetPci on net1). When THYLACINE_GUESTFWD
# is set, the guest's magic `10.0.2.100:7820` forwards to a HOST server on
# `127.0.0.1:$THYLACINE_GUESTFWD_HOSTPORT` (default 28099) -- the apples-to-apples
# NIC-path benchmark target (`tools/np3-bench.sh`). Unset (the standard boot/SMP
# gate): net1 is byte-identical, no forwarding rule. Inert without a host server
# (a closed target RSTs); the in-guest M6 probe gates on a bounded connect, so an
# absent/inert guestfwd is a fast SKIP, never a hang.
net1_opts="user,id=net1"
if [[ -n "${THYLACINE_GUESTFWD:-}" ]]; then
    gf_hostport="${THYLACINE_GUESTFWD_HOSTPORT:-28099}"
    # Three rules: 7820/+1/+2 (rtt-echo / floor-delayed-echo / bw-sink) -> the host
    # server's 28099/+1/+2. One rule per metric so each gets its own host
    # connection (a guestfwd rule maps to one connection; same-port dials coalesce).
    for gf_i in 0 1 2; do
        net1_opts="${net1_opts},guestfwd=tcp:10.0.2.100:$((7820 + gf_i))-tcp:127.0.0.1:$((gf_hostport + gf_i))"
    done
fi
net_flags=()
if [[ "${THYLACINE_NO_NET:-0}" != "1" ]]; then
    net_flags=(
        -netdev "user,id=net0"
        -device "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56"
        # pci-2: the virtio-net-pci NIC on its own page-aligned BAR (the #140
        # resolution). A SEPARATE slirp backend (net1) -- one -netdev binds one
        # device, and each user-mode net is its own 10.0.2.0/24 with its own
        # 10.0.2.2 gateway, so both ARP probes round-trip independently.
        # disable-legacy=on -> modern-only (device_id 0x1041, no legacy I/O BAR),
        # which the VirtioNetPci modern-common-cfg driver requires. netdev-pci-
        # test claims it (virtio_device_id=1); the warden-bound netdev-driver
        # claims the mmio net above (MENAGERIE 5d-3). The PCI function is not a
        # virtio-mmio slot (like rng-pci), so the mmio slot map below is unchanged.
        -netdev "$net1_opts"
        -device "virtio-net-pci,netdev=net1,disable-legacy=on,mac=52:54:00:12:34:57"
    )
fi
if [[ -n "${THYLACINE_NET_DUMP:-}" ]]; then
    net_flags+=(-object "filter-dump,id=netdump0,netdev=net0,file=$THYLACINE_NET_DUMP")
fi

# P4-K: virtio-keyboard-device on the MMIO transport, for the
# virtio-input probe. QEMU virt exposes a HID keyboard whose
# DeviceID=18 on the virtio bus; we don't actually consume events
# (would require -monitor send-key or QMP wiring on every CI run), but
# initializing it to DRIVER_OK + reading config-space + classifying via
# EV_BITS exercises a third VirtIO device class on the same composed
# substrate.
#
# Slot assignment: this -device sits AFTER net_flags in the exec
# invocation. With QEMU's reverse-creation slot allocation, disk stays
# at slot 31, net at slot 30, keyboard lands at slot 29, gpu drops to
# slot 28, virtio-rng drops to slot 27. The probe scans all 32 slots
# so order is informational. THYLACINE_NO_INPUT=1 disables for
# environments where the device isn't desired.
input_flags=()
if [[ "${THYLACINE_NO_INPUT:-0}" != "1" ]]; then
    input_flags=(
        -device "virtio-keyboard-device,id=kbd0"
    )
fi

# P4-L: virtio-gpu-device on the MMIO transport, for the virtio-gpu
# probe. QEMU virt exposes a virtio-gpu whose DeviceID=16 on the
# virtio bus. With -nographic + -display none (the default test
# environment), the device still enumerates a scanout (num_scanouts
# >= 1) and responds to GET_DISPLAY_INFO with OK_DISPLAY_INFO; the
# pmodes[0].enabled bit may be 0 if no display backend is attached.
# The probe exercises (a) DeviceID=16 dispatch, (b) two-virtqueue
# configuration (controlq idx 0 + cursorq idx 1; first driver to use
# REG_QUEUE_SEL=1), and (c) the controlq command/response chain
# pattern (req+resp via two descriptors with NEXT linkage). This is
# the substrate gate for Phase 8 Halcyon.
#
# Slot assignment: this -device sits AFTER input_flags in the exec
# invocation. With QEMU's reverse-creation slot allocation: disk=31,
# net=30, kbd=29, gpu=28, rng=27, rng-pci doesn't count (PCI bus, not
# virtio-mmio). THYLACINE_NO_GPU=1 disables.
gpu_flags=()
if [[ "${THYLACINE_NO_GPU:-0}" != "1" ]]; then
    gpu_flags=(
        -device "virtio-gpu-device,id=gpu0"
    )
fi

# P4-K-events: QMP control socket for test-harness key injection.
# tools/test.sh polls the boot log for the userspace virtio-input
# probe's "AWAITING_QMP_KEY" sentinel and, upon match, connects to
# this socket and sends `send-key` for the target keycode. QEMU's
# virtio-keyboard-device translates that into eventq writes; the
# driver drains + validates.
#
# Disabled by THYLACINE_NO_QMP=1. The socket path lives under build/
# alongside other build artifacts; it's overwritten per run (server
# mode), so stale sockets from a previous run don't accumulate.
qmp_flags=()
if [[ "${THYLACINE_NO_QMP:-0}" != "1" ]]; then
    qmp_sock="${THYLACINE_QMP_SOCK:-$REPO_ROOT/build/qmp.sock}"
    mkdir -p "$(dirname "$qmp_sock")"
    rm -f "$qmp_sock"
    qmp_flags=(-qmp "unix:$qmp_sock,server,nowait")
fi

# 9P host share — appears at /host inside the guest once the 9P client lands
# (P1-A: no client yet, so the QEMU virtfs entry is benign overhead). Per
# TOOLING.md §4 (the hot-reload mechanism). Default-on; --no-share disables.
if [[ ${#share_flags[@]} -eq 0 ]] && [[ "${THYLACINE_NO_SHARE:-0}" != "1" ]]; then
    mkdir -p "$REPO_ROOT/share"
    share_flags=(
        -virtfs "local,path=$REPO_ROOT/share,mount_tag=host0,security_model=none,id=host0"
    )
fi

# Optional snapshot restore (Phase 5+; ignored if the QCOW2 backing isn't yet
# created). Placeholder for now — `tools/snapshot.sh save/restore` lands in
# a later sub-chunk.
if [[ -n "$snapshot" ]]; then
    echo "Snapshot restore not yet implemented (Phase 5+ deliverable)." >&2
fi

# Hardware-acceleration toggle. DEFAULT: HVF on a capable host (Apple Silicon
# with Hypervisor.framework), else TCG. This is the Lazarus M1 end-state -- HVF
# is the fast dev/test loop; TCG is the portable compat reference (PORTABILITY.md
# section 8). Auto-detection probes the host AND the qemu build, so the launcher
# still works on a non-Apple box (it falls back to TCG). Force either explicitly:
#   THYLACINE_ACCEL=hvf tools/run-vm.sh     # require HVF
#   THYLACINE_ACCEL=tcg tools/test.sh       # the full-emulation compat run
# Under HVF the guest sees the HOST CPU via -cpu host: Apple cores have
# LSE+PAC+BTI but NOT FEAT_RNG/RNDR, so the kernel CSPRNG seeds from the
# virtio-rng device (Lazarus W3) -- one software path on every target.
detect_accel() {
    if [[ "$(uname -s)" == "Darwin" \
       && "$(sysctl -n kern.hv_support 2>/dev/null)" == "1" ]] \
       && qemu-system-aarch64 -accel help 2>/dev/null | grep -qw hvf; then
        echo hvf
    else
        echo tcg
    fi
}
accel="${THYLACINE_ACCEL:-$(detect_accel)}"

# -cpu model + GIC version default off the chosen accel. HVF wants -cpu host +
# GICv2: its emulated GICv3 distributor MMIO trips an `isv` data-abort assert,
# and the GICv2 MMIO CPU interface is the HVF-on-Apple enabler (Lazarus W2). TCG
# wants -cpu max (full ISA incl. RNDR) + GICv3 (QEMU virt's modern default; the
# kernel autodetects v2-vs-v3 from DTB). THYLACINE_CPU / THYLACINE_GIC override.
case "$accel" in
    hvf) cpu="${THYLACINE_CPU:-host}"; gicv="${THYLACINE_GIC:-2}" ;;
    *)   cpu="${THYLACINE_CPU:-max}";  gicv="${THYLACINE_GIC:-3}" ;;
esac
echo "==> qemu: accel=$accel cpu=$cpu gic=v$gicv smp=$cpus" >&2

# Canonical QEMU flags per TOOLING.md §3.
#
# disk_flags (P4-Ic5b2) comes BEFORE virtio-rng-device because QEMU
# assigns virtio-mmio slots in reverse-creation order, so the first
# -device lands at slot 31. virtio-blk-probe scans 0..31 either way;
# the ordering keeps slot 31 conventionally the "primary device."
exec qemu-system-aarch64 \
    -machine "virt,gic-version=$gicv,accel=$accel" \
    -cpu "$cpu" \
    -smp "$cpus" \
    -m "$mem_mib" \
    -kernel "$KERNEL_BIN" \
    ${ramfs_flags[@]+"${ramfs_flags[@]}"} \
    ${pool_flags[@]+"${pool_flags[@]}"} \
    ${disk_flags[@]+"${disk_flags[@]}"} \
    ${net_flags[@]+"${net_flags[@]}"} \
    ${input_flags[@]+"${input_flags[@]}"} \
    ${gpu_flags[@]+"${gpu_flags[@]}"} \
    -device virtio-rng-device,id=rng0 \
    -device virtio-rng-pci,id=rng_pci0 \
    -nographic \
    -serial "${THYLACINE_SERIAL:-mon:stdio}" \
    ${qmp_flags[@]+"${qmp_flags[@]}"} \
    ${gdb_flags[@]+"${gdb_flags[@]}"} \
    ${share_flags[@]+"${share_flags[@]}"} \
    ${extra_qemu_args[@]+"${extra_qemu_args[@]}"}
