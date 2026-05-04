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
KERNEL_ELF="$REPO_ROOT/build/kernel/thylacine.elf"
# Flat-binary form, with Linux ARM64 image header. QEMU's load_aarch64_image()
# detects the ARM\x64 magic at offset 0x38 and treats us as a Linux Image
# (is_linux=1), which causes it to load the DTB and pass its address in x0.
# An ELF load (is_linux=0) skips the DTB entirely. See docs/reference/01-boot.md
# "Caveats > DTB pointer observed as 0x0" for the investigation.
KERNEL_BIN="$REPO_ROOT/build/kernel/thylacine.bin"

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
exec qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu max \
    -smp "$cpus" \
    -m "$mem_mib" \
    -kernel "$KERNEL_BIN" \
    -nographic \
    -serial mon:stdio \
    ${gdb_flags[@]+"${gdb_flags[@]}"} \
    ${share_flags[@]+"${share_flags[@]}"} \
    ${extra_qemu_args[@]+"${extra_qemu_args[@]}"}
