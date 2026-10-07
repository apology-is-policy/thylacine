#!/bin/sh
# Rebuild ONLY the kernel, from the already-configured build dir.
#
# WHY NOT `tools/build.sh kernel --config ci`. That target rebuilds the whole
# image -- userspace, pouch, stratumd, the ramfs -- and the mutations the red
# legs apply are kernel-only. Holding userspace FIXED makes the kernel the
# single changed variable, which is the experiment the legs are actually trying
# to run; rebuilding the world around each mutation would add a dozen variables
# to a test whose whole point is one.
#
# It is also the only path that works in this tree today: build/ carries a
# stratumd CMake cache generated from a PEER's source tree (D7's residue,
# inherited through the APFS clone of a peer's build/), so the full target
# REFUSES at the stratumd step. That is enqueued as its own bug and is NOT
# worked around here -- this script does not touch stratumd, the ramfs or the
# pool, so nothing it produces depends on that cache either way.
#
# thylacine.bin -- the image QEMU actually boots -- is an objcopy POST_BUILD on
# thylacine.elf (kernel/CMakeLists.txt:342-345), so a relink regenerates it.
# That is what makes a kernel-only rebuild sufficient rather than a trap: there
# is no stale flat binary left behind for the next boot to pick up.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 3

BD=build/kernel
[ -f "$BD/CMakeCache.txt" ] \
  || { echo "REFUSING: $BD is not a configured cmake build dir -- run tools/build.sh kernel --config ci once first"; exit 3; }

# Pinned to the configured tree so this can never silently build a different
# source root than the one it was configured for -- the same class of defect as
# the stratumd cache above.
want=$(cd "$ROOT" && pwd)
got=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BD/CMakeCache.txt")
[ "$got" = "$want" ] \
  || { echo "REFUSING: $BD was configured for $got, not $want"; exit 3; }

exec cmake --build "$BD" --target thylacine.elf -j"${KJOBS:-8}"
