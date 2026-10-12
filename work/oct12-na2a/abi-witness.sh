#!/bin/sh
# The ring half of tools/check-loom-service-abi.py, run both ways: a copy of the
# headers it reads passes, and each sabotaged copy fails at the checker's own
# assert (a header's own _Static_assert firing first would prove nothing about
# the checker). Each sabotage keeps the header's own asserts true.
#   A  native C: hdr._pad split into _pad[4] + one u32 (size and offsets unchanged)
#   B  native C: T_LOOM_OP_CLUNK 10 -> 11 (no header assert covers it)
#   C  kernel: ring_hdr sq_mask and sq_entries declared in swapped order
# Usage: abi-witness.sh <scratch dir>  (run from the tree root)
set -e
ROOT=$(pwd)
W=$1
[ -n "$W" ] || { echo "usage: abi-witness.sh <scratch dir>"; exit 3; }
rm -rf "$W"; mkdir -p "$W"

copy() { # copy <dir>: the files the checker reads, nothing else
  for d in kernel/include arch/arm64 usr/lib/libt/include; do
    mkdir -p "$1/$d"; cp -R "$ROOT/$d/." "$1/$d/"
  done
  mkdir -p "$1/usr/lib/libthyla-rs/src/loom"
  cp "$ROOT/usr/lib/libthyla-rs/src/loom/service_abi.rs" "$1/usr/lib/libthyla-rs/src/loom/"
}
patch1() { # patch1 <file> <old> <new>: exactly one occurrence, or refuse
  python3 - "$1" "$2" "$3" <<'EOF'
import sys
f, old, new = sys.argv[1:]
old = old.encode().decode('unicode_escape'); new = new.encode().decode('unicode_escape')
s = open(f).read()
if s.count(old) != 1: raise SystemExit("REFUSING: %r occurs %d times in %s" % (old, s.count(old), f))
open(f, 'w').write(s.replace(old, new, 1))
EOF
}
check() { # check <dir> <log>: the checker's exit status
  set +e; python3 "$ROOT/tools/check-loom-service-abi.py" --root "$1" > "$2" 2>&1; _rc=$?; set -e
  return $_rc
}

copy "$W/control"
if check "$W/control" "$W/control.log" && [ "$(/usr/bin/grep -c '^PASS:' "$W/control.log")" = 2 ]; then
  echo "control: PASS (both lines)"
else
  cat "$W/control.log"; echo "control: the unsabotaged copy does not pass"; exit 1
fi

sabotage() { # sabotage <label> <file> <old> <new> <the checker's assert message>
  copy "$W/$1"
  patch1 "$W/$1/$2" "$3" "$4"
  if check "$W/$1" "$W/$1.log"; then echo "$1: the sabotage PASSED the checker"; exit 1; fi
  /usr/bin/grep -qF "$5" "$W/$1.log" || { echo "$1: failed, but not at '$5':"; head -20 "$W/$1.log"; exit 1; }
  echo "$1: FAIL at '$5'"
}
sabotage A usr/lib/libt/include/thyla/loom.h \
  '    uint32_t _pad[5];\n' '    uint32_t _pad[4];\n    uint32_t _pad4;\n' \
  'struct t_loom_ring_hdr._pad'
sabotage B usr/lib/libt/include/thyla/loom.h \
  '#define T_LOOM_OP_CLUNK            10u\n' '#define T_LOOM_OP_CLUNK            11u\n' \
  'T_LOOM_OP_CLUNK'
sabotage C kernel/include/thylacine/loom.h \
  '    u32 sq_mask;         // sq_entries - 1 (immutable; set at setup)\n    u32 sq_entries;      // immutable\n' \
  '    u32 sq_entries;      // immutable\n    u32 sq_mask;         // sq_entries - 1 (immutable; set at setup)\n' \
  'struct loom_ring_hdr.sq_mask'
echo "abi-witness: control passes; A, B and C each fail at the checker's own assert"
