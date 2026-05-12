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
mem_mib=2048
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
        -drive "if=none,id=disk0,format=raw,file=$DISK_IMG"
        -device virtio-blk-device,drive=disk0
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
net_flags=()
if [[ "${THYLACINE_NO_NET:-0}" != "1" ]]; then
    net_flags=(
        -netdev "user,id=net0"
        -device "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56"
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

# Canonical QEMU flags per TOOLING.md §3.
#
# disk_flags (P4-Ic5b2) comes BEFORE virtio-rng-device because QEMU
# assigns virtio-mmio slots in reverse-creation order, so the first
# -device lands at slot 31. virtio-blk-probe scans 0..31 either way;
# the ordering keeps slot 31 conventionally the "primary device."
exec qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu max \
    -smp "$cpus" \
    -m "$mem_mib" \
    -kernel "$KERNEL_BIN" \
    ${ramfs_flags[@]+"${ramfs_flags[@]}"} \
    ${disk_flags[@]+"${disk_flags[@]}"} \
    ${net_flags[@]+"${net_flags[@]}"} \
    ${input_flags[@]+"${input_flags[@]}"} \
    ${gpu_flags[@]+"${gpu_flags[@]}"} \
    -device virtio-rng-device,id=rng0 \
    -device virtio-rng-pci,id=rng_pci0 \
    -nographic \
    -serial mon:stdio \
    ${gdb_flags[@]+"${gdb_flags[@]}"} \
    ${share_flags[@]+"${share_flags[@]}"} \
    ${extra_qemu_args[@]+"${extra_qemu_args[@]}"}
