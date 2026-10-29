#!/bin/sh
# Give the asr9 worktree a build/ (reference_ci_image_worktree_recipe): an APFS
# clone of the primary checkout's (b3, the nearest tree to main with a build),
# then drop every CMake tree whose cache names that checkout -- each would build
# the PRIMARY's sources into this image -- keeping clade/llvm-build and
# pouch/cxx-runtimes, whose caches name pristine sources outside every checkout.
# Run under the mac lease (the primary's build must not be mid-write).
set -eu
SRC=/Users/northkillpd/projects/thylacine
ROOT=/Users/northkillpd/projects/thylacine-asr9
[ ! -e "$ROOT/build" ] || { echo "REFUSING: $ROOT/build exists"; exit 3; }
df -m "$ROOT" | awk 'NR==2 {print "free before clone: " $4 " MiB"}'
cp -Rc "$SRC/build" "$ROOT/build" 2> /Users/northkillpd/projects/thylacine-corona/work/oct19-asr9/clone-skips.txt || true
df -m "$ROOT" | awk 'NR==2 {print "free after clone: " $4 " MiB"}'
for cc in $(find "$ROOT/build" -maxdepth 6 -name CMakeCache.txt); do
  h=$(/usr/bin/grep -m1 '^CMAKE_HOME_DIRECTORY' "$cc" | cut -d= -f2)
  d=$(dirname "$cc")
  case "$h" in
    /Users/northkillpd/projects/thylacine|/Users/northkillpd/projects/thylacine/*)
      echo "drop ${d#$ROOT/build/}  (names $h)"; rm -rf "$d" ;;
  esac
done
for d in host-stratum pouch/stratumd-cmake; do
  [ -d "$ROOT/build/$d" ] && { echo "drop $d (its build paths name the primary)"; rm -rf "$ROOT/build/$d"; }
done
rsync -a --ignore-existing "$SRC/third_party/rust/" "$ROOT/third_party/rust/"
echo "kept caches:"; find "$ROOT/build" -maxdepth 6 -name CMakeCache.txt -exec sh -c 'echo "  ${1#'"$ROOT"'/build/} -> $(grep -m1 "^CMAKE_HOME_DIRECTORY" "$1" | cut -d= -f2)"' _ {} \;
df -m "$ROOT" | awk 'NR==2 {print "free after setup: " $4 " MiB"}'
