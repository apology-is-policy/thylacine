#!/bin/sh
# Type-check changed kernel translation units WITHOUT a build, off-lease.
#
# WHY THIS EXISTS. A kernel build needs the mac lease. Waiting for a lease to
# discover a typo wastes the scarcest resource in the fleet, so this runs the
# compiler's front end only -- one file per invocation, no codegen, no linking,
# no parallelism. That is the class the operator ruled may run off-lease (the
# same ruling that covers the host doubles).
#
# WHAT IT PROVES: the file parses and type-checks under the REAL kernel flags,
# taken from the configured build's flags.make rather than retyped, so a flag
# drift cannot make this check pass where the build would fail. _Static_assert
# is compile-time, so size assertions ARE evaluated here.
#
# WHAT IT DOES NOT PROVE: nothing about linking (an undefined symbol survives
# this), nothing about codegen, and nothing whatsoever about behaviour. A clean
# run here is a precondition for the build, never a substitute for it.
#
# NOTE FOR zsh CALLERS: the flags must be word-split. zsh does NOT split "$var",
# which silently hands clang one enormous unknown argument -- measured, it fails
# with `unknown argument:` naming the whole string. This script is /bin/sh.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 3

# Captured FIRST, because the define handling below reuses "$@" via eval and
# would otherwise turn the file list into the compiler's -D flags.
FILES=${*:-"kernel/loom.c kernel/handle.c kernel/proc.c kernel/main.c kernel/test/test_loom.c kernel/test/test_burrow.c kernel/test/test.c"}

FLAGS_MAKE=build/kernel/kernel/CMakeFiles/thylacine.elf.dir/flags.make
if [ ! -f "$FLAGS_MAKE" ]; then
  echo "REFUSING: $FLAGS_MAKE is absent -- configure a kernel build first, or the"
  echo "          flags below would be invented rather than read."
  exit 4
fi

# Read the flags the build actually uses. -fstack-clash-protection is dropped:
# clang warns it is unused on this target for every file, which would bury a
# real diagnostic in noise.
C_FLAGS=$(sed -n 's/^C_FLAGS = //p' "$FLAGS_MAKE" | sed 's/-fstack-clash-protection//')
C_DEFINES=$(sed -n 's/^C_DEFINES = //p' "$FLAGS_MAKE")
C_INCLUDES=$(sed -n 's/^C_INCLUDES = //p' "$FLAGS_MAKE")
CC=$(sed -n 's/^# compile C with //p' "$FLAGS_MAKE" | head -1)

# THE DEFINES MUST KEEP THEIR INNER QUOTES, and getting this wrong reads as a
# defect in the source rather than in this script. flags.make stores
# -DTHYLACINE_PHASE_STRING=\"P3-F\"; make hands that to a shell, which turns \"
# into a literal " that stays INSIDE the macro value, so the string-literal
# concatenations in main.c ("Thylacine v" THYLACINE_VERSION_STRING ...) work.
# Expanding $C_DEFINES unquoted instead lets the shell eat the quotes, the macro
# becomes a bare token, and main.c fails with `expected ')'` at exactly those
# concatenations -- two errors that are this script's fault and look like the
# kernel's. `eval set --` reproduces make's own quote handling, and the control
# for it is below: main.c must come out clean.
eval "set -- $C_DEFINES"
DEFS_OK=1
for d in "$@"; do
  case $d in *'="'*'"') ;; *=*'"'*) DEFS_OK=0 ;; esac
done
[ "$DEFS_OK" = 1 ] || { echo "REFUSING: string defines lost their quoting"; exit 4; }
[ -n "$CC" ] || CC=clang
if [ ! -x "$CC" ]; then echo "REFUSING: compiler $CC is not executable"; exit 4; fi

echo "cc    : $CC"
echo "flags : from $FLAGS_MAKE"
echo

rc=0
n=0
for f in $FILES; do
  n=$((n + 1))
  if [ ! -f "$f" ]; then echo "REFUSING: $f does not exist"; exit 4; fi
  # "$@" carries the defines with their quoting intact; includes and flags are
  # plain tokens and may word-split.
  # shellcheck disable=SC2086
  out=$($CC -fsyntax-only $C_INCLUDES "$@" $C_FLAGS "$f" 2>&1)
  errs=$(printf '%s\n' "$out" | grep -c 'error:')
  miss=$(printf '%s\n' "$out" | grep -c 'no previous prototype')
  other=$(printf '%s\n' "$out" | grep 'warning:' | grep -vc 'no previous prototype')
  printf '%-30s errors=%-3s missing-prototype=%-3s other-warnings=%s\n' "$f" "$errs" "$miss" "$other"
  if [ "$errs" -ne 0 ]; then
    printf '%s\n' "$out" | grep -A2 'error:' | head -40
    rc=1
  fi
  if [ "$other" -ne 0 ]; then
    printf '%s\n' "$out" | grep 'warning:' | grep -v 'no previous prototype' | head -10
  fi
done

# The denominator, so a run that checked nothing cannot read as a pass.
echo
echo "$n file(s) checked"
[ "$n" -gt 0 ] || { echo "REFUSING: checked zero files"; exit 4; }
if [ $rc -eq 0 ]; then
  echo "NO ERRORS. missing-prototype warnings are the pre-existing pattern for"
  echo "kernel test entry points (declared in test.c, not in a header); the kernel"
  echo "build carries no -Werror, so they do not fail it."
else
  echo "ERRORS ABOVE -- do not spend a lease on a build until they are gone."
fi
exit $rc
