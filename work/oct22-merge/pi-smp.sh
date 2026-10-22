#!/bin/sh
# The A72/KVM axis for the main merge (corona/merge-main), derived from
# work/oct19-pi/pi-smp.sh, which carried vote 16 to the same axis: only the Go
# fork pin (3471e2a, the merge's), the evidence paths and the usage differ.
# A race fix green on one memory model is one reading; thyla-pi is 4x
# Cortex-A72 under real KVM, the only non-Apple ARM64 we have.
#
# Two phases, invoked separately, because only the first needs the mac:
#   sh work/oct22-merge/pi-smp.sh bake  HOLD THE MAC. Builds the UBSan kernel, then
#                                    the ci image, of HEAD; writes a manifest.
#                                    Release the mac as soon as it prints DONE.
#   sh work/oct22-merge/pi-smp.sh run   HOLD THE PI. Checks build/ against the
#                                    manifest, syncs it to a corona-private tree
#                                    on the pi, boots N per config there, pulls
#                                    every boot's logs back, and judges them.
# DRYRUN=1 stops either phase after its preconditions.
#
# The pi tree is ~/projects/thylacine-corona. The shared ~/projects/thylacine is
# the fleet's certified restore source (docs/agent/THYLA-PI.md): never touched.
# The run's boots are detached on the pi (setsid), so a dropped tunnel loses
# the watcher, not the run; re-run `wait` to re-attach to the same run.
# Run from the checkout root (no cd).
set -e
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
PHASE=${1:-}
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
# The LAN name thyla-pi.local does not resolve from this mac (measured 10-19).
PI_HOST=${PI_HOST:-thyla-pi-cf}
PI_TREE=projects/thylacine-corona
PI_N=${PI_N:-10}
# label:cpus:sanitizer:boot-timeout. The timeouts are ceilings, not sleeps:
# SD-card I/O dominates a pi boot (docs/agent/THYLA-PI.md), so they sit well
# above ci-smp-gate's mac figures (300/420).
PI_CONFIGS=${PI_CONFIGS:-"default-smp4:4::1500 ubsan-smp4:4:ubsan:2400"}
ARTS="build/kernel/thylacine.elf build/kernel/thylacine.bin build/kernel-undefined/thylacine.elf build/kernel-undefined/thylacine.bin build/ramfs.cpio build/disk.img build/fixtures/pool.img build/fixtures/system.key"
# smp-multiboot restores the pool from these twins before every boot (and
# refuses without them), and checks the key twin to know the snapshot belongs
# to this ramfs. A populate leaves each twin equal to its source; the bake
# checks that, so the run can make them on the pi by copying the verified
# pool and key instead of shipping 2.5 GiB twice.
TWINS="build/fixtures/pool.img build/fixtures/system.key"
LAST=work/oct22-merge/PI-LAST-BAKE
LASTRUN=work/oct22-merge/PI-LAST-RUN
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
STRATUM_DIRTY_OK='v2/.audit_r172_findings.md v2/specs/compound_ops_per_inode.tla'
STRATUMD_LINE="==> building stratumd (aarch64-thylacine) from $STRATUM_SRC"
D7_LINE='joey: D7 overlapping login probe PASS'
GOFORK_PIN_DIR=/Users/northkillpd/projects/go-thylacine-pin-3471e2a
GOFORK_PIN=3471e2ac027de4a5cbbee9337a3f335b96156e29
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'

STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
free_mb() { df -m . | awk 'NR==2 {print $4}'; }
lease_ok() { # lease_ok <mac|pi> <label>
  "$YIP" resources > "$OUT/lease-$2.txt" 2>&1 || true
  /usr/bin/grep -qE "^$1 +HELD by you" "$OUT/lease-$2.txt" || {
    echo "STOPPING before $2: the $1 lease is not mine"; exit 6; }
}
# Not "times": sh finds that special builtin before any function.
occurs() { # occurs <file> <count> <fixed text>
  _c=$(/usr/bin/grep -cF "$3" "$1" || true)
  [ "$_c" = "$2" ] || { echo "'$3' occurs $_c times in $1, want $2"; return 1; }
}
tally() { /usr/bin/grep -aE '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/'; }
carries_tests() { # carries_tests <elf> <test name>: the KERNEL_TESTS=OFF kernel has no names
  [ "$(/usr/bin/grep -c -a -F "$2" "$1" || true)" -ge 1 ]
}
stratum_dirty() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=no | sed 's/^...//' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'; }
stratum_inputs() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=all | /usr/bin/grep '^??' | sed 's/^?? //' | /usr/bin/grep -E '\.(c|h|cc|cpp|S|s|cmake)$|CMakeLists\.txt$' || true; }
stratum_ok() {
  [ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING at $1: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
  [ "$(stratum_dirty)" = "$STRATUM_DIRTY_OK" ] || { echo "REFUSING at $1: Stratum tracked edits '$(stratum_dirty)' are not '$STRATUM_DIRTY_OK'"; exit 3; }
  [ -z "$(stratum_inputs)" ] || { echo "REFUSING at $1: untracked Stratum build inputs: $(stratum_inputs)"; exit 3; }
}
cache_ok() {
  _h=$(/usr/bin/grep -m1 '^CMAKE_HOME_DIRECTORY' "$1" | cut -d= -f2)
  [ "$_h" = "$STRATUM_SRC" ] || { echo "$1 is configured against '$_h', not $STRATUM_SRC"; exit 1; }
}
built_from_canonical() { # built_from_canonical <label> <build log>
  occurs "$2" 1 "$STRATUMD_LINE" || exit 1
  if /usr/bin/grep -q 'stratum-astra' "$2"; then echo "$1: the build log names stratum-astra"; exit 1; fi
  cache_ok build/host-stratum/CMakeCache.txt
  cache_ok build/pouch/stratumd-cmake/CMakeCache.txt
}
last_test() { /usr/bin/grep -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c | tail -1 | sed -E 's/^[^"]*"([^"]+)".*/\1/'; }
pi() { ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 "$PI_HOST" "$@"; }

bake() {
  OUT=work/oct22-merge/pi-bake-$STAMP
  mkdir -p "$OUT"
  trap 'record "exit $?"' EXIT
  _t=$OUT/occurs-self-check.txt
  printf 'x\nx\n' > "$_t"
  occurs "$_t" 2 x > /dev/null || { echo "REFUSING: occurs refuses a true count"; exit 3; }
  if occurs "$_t" 1 x > /dev/null; then echo "REFUSING: occurs accepts a false count"; exit 3; fi
  record "occurs self-check: a true count passes, a false one fails"
  [ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
  [ -n "${DRYRUN:-}" ] || lease_ok mac pre
  _d=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
  [ "$_d" = 0 ] || { echo "REFUSING: $_d tracked modification(s); commit first"; exit 3; }
  COMMIT=$(git rev-parse HEAD)
  EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
  [ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
  NAME=$(last_test)
  record "HEAD $COMMIT, clean; expected tests (derived from test.c): $EXPECT; last registered: $NAME"
  stratum_ok pre
  record "stratum $STRATUM_SRC at $STRATUM_PIN; tracked edits exactly '$STRATUM_DIRTY_OK'; no untracked build input"
  [ "${GOFORK:-}" = "$GOFORK_PIN_DIR" ] || { echo "REFUSING: GOFORK must be $GOFORK_PIN_DIR (got '${GOFORK:-}')"; exit 3; }
  export GOFORK
  [ "$(git -C "$GOFORK" rev-parse HEAD 2>/dev/null)" = "$GOFORK_PIN" ] || { echo "REFUSING: GOFORK HEAD is not $GOFORK_PIN"; exit 3; }
  [ -z "$(git -C "$GOFORK" status --porcelain)" ] || { echo "REFUSING: GOFORK has uncommitted changes"; exit 3; }
  record "go fork $GOFORK at $GOFORK_PIN, clean"
  [ -z "${DRYRUN:-}" ] || { record "DRYRUN: the bake preconditions hold; nothing built"; exit 0; }
  [ "$(free_mb)" -ge 8192 ] || { echo "REFUSING: $(free_mb) MiB free, under the 8192 floor"; exit 3; }

  # UBSan first, the ci image last: the two kernels share one ramfs/pool pair
  # (neither carries the pool key), and the pair on disk is the last bake's.
  lease_ok mac ubsan
  tools/build.sh kernel --config ci --sanitize=ubsan > "$OUT/ubsan-build.log" 2>&1 || { echo "ubsan build failed"; tail -20 "$OUT/ubsan-build.log"; exit 1; }
  /usr/bin/grep -qF "sanitize='undefined'" "$OUT/ubsan-build.log" || { echo "UBSAN: the build never selected the sanitizer"; exit 1; }
  built_from_canonical UBSAN "$OUT/ubsan-build.log"
  lease_ok mac ci
  tools/build.sh kernel --config ci > "$OUT/ci-build.log" 2>&1 || { echo "ci build failed"; tail -20 "$OUT/ci-build.log"; exit 1; }
  if /usr/bin/grep -qF "$QUAL_LINE" "$OUT/ci-build.log"; then echo "CI: the default build saw the qualification lever"; exit 1; fi
  built_from_canonical CI "$OUT/ci-build.log"
  stratum_ok post-build
  [ "$(git rev-parse HEAD)" = "$COMMIT" ] && [ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] \
    || { echo "HEAD moved or the tree changed during the bake"; exit 8; }
  for k in build/kernel/thylacine.elf build/kernel-undefined/thylacine.elf; do
    carries_tests "$k" "$NAME" || { echo "$k does not carry the test registry ('$NAME')"; exit 1; }
  done
  [ "$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)" != "$(shasum -a 256 build/kernel-undefined/thylacine.bin | cut -d' ' -f1)" ] \
    || { echo "the UBSan kernel equals the default one"; exit 1; }
  for f in $TWINS; do
    cmp -s "$f" "$f.baked-snapshot" || { echo "$f.baked-snapshot is not the bake's $f"; exit 1; }
  done
  record "snapshot twins equal their sources: $TWINS"
  # shellcheck disable=SC2086
  shasum -a 256 $ARTS > "$OUT/manifest.txt"
  printf 'commit %s\nexpect %s\nlast %s\nstratum %s\ngofork %s\n' "$COMMIT" "$EXPECT" "$NAME" "$STRATUM_PIN" "$GOFORK_PIN" > "$OUT/provenance.txt"
  cat "$OUT/manifest.txt" >> "$OUT/stages.txt"
  echo "$OUT" > "$LAST"
  record "BAKE DONE: $COMMIT -> $OUT. Release the mac now."
}

judge() { # judge <run dir> <expect>: every kept boot of every config, read from its log
  _bad=0
  for cfg in $PI_CONFIGS; do
    _l=pi-$(echo "$cfg" | cut -d: -f1)
    _rc=$(cat "$1/$_l.rc" 2>/dev/null || echo missing)
    _sum=$(/usr/bin/grep -aE "^== $_l: [0-9]+ PASS" "$1/$_l.out" 2>/dev/null | tail -1)
    record "$_l: smp-multiboot rc $_rc; ${_sum:-NO SUMMARY LINE}"
    [ "$_rc" = 0 ] || _bad=1
    [ -n "$_sum" ] || _bad=1
    _k=0
    for i in $(seq 1 "$PI_N"); do
      _b=$1/build/multiboot-logs/$_l-$i.log
      [ -f "$_b" ] || { record "  $_l boot $i: NO LOG"; _bad=1; continue; }
      _k=$((_k + 1))
      _why=""
      if /usr/bin/grep -aq '^EXTINCTION:' "$_b"; then _why=$(/usr/bin/grep -a -m1 '^EXTINCTION:' "$_b" | cut -c1-120); fi
      set -- "$1" "$2" $(tally "$_b")
      [ "${3:-}" = "$2" ] && [ "${4:-}" = "$2" ] || _why="${_why:+$_why; }tally '${3:-} ${4:-}' != $2/$2"
      occurs "$_b" 1 "$D7_LINE" > /dev/null || _why="${_why:+$_why; }D7 line not exactly once"
      if [ -n "$_why" ]; then record "  $_l boot $i: RED -- $_why"; _bad=1; fi
    done
    [ "$_k" = "$PI_N" ] || { record "  $_l: $_k of $PI_N boot logs kept"; _bad=1; }
  done
  return $_bad
}

run() {
  OUT=work/oct22-merge/pi-run-$STAMP
  mkdir -p "$OUT"
  trap 'record "exit $?"' EXIT
  [ -f "$LAST" ] || { echo "REFUSING: no bake recorded in $LAST"; exit 3; }
  BAKE=$(cat "$LAST")
  COMMIT=$(sed -n 's/^commit //p' "$BAKE/provenance.txt")
  EXPECT=$(sed -n 's/^expect //p' "$BAKE/provenance.txt")
  NAME=$(sed -n 's/^last //p' "$BAKE/provenance.txt")
  echo "$BAKE" > "$OUT/bake.txt"
  record "bake $BAKE: commit $COMMIT, expect $EXPECT"
  # A sabotage or a later bake leaves its kernel in build/ (the M-PIN): the
  # artifacts must still be the bake's, byte for byte.
  shasum -a 256 -c --quiet "$BAKE/manifest.txt" || { echo "REFUSING: build/ no longer matches the bake's manifest"; exit 3; }
  carries_tests build/kernel/thylacine.elf "$NAME" || { echo "REFUSING: build/kernel does not carry the tests"; exit 3; }
  record "build/ matches the manifest; the default ELF carries '$NAME'"
  case "$PI_TREE" in projects/thylacine-corona) ;; *) echo "REFUSING: PI_TREE '$PI_TREE' is not corona's"; exit 3 ;; esac
  [ -n "${DRYRUN:-}" ] || lease_ok pi pre
  pi true || { echo "REFUSING: $PI_HOST unreachable (a FREE lease is not reachability)"; exit 6; }
  _q=$(pi "ps -eo comm= | grep -c '^qemu-system' || true")
  [ "$_q" = 0 ] || { echo "REFUSING: $_q qemu-system process(es) already on the pi (single-flight)"; exit 6; }
  _pf=$(pi "df -Pm ~ | awk 'NR==2 {print \$4}'")
  [ "$_pf" -ge 8192 ] || { echo "REFUSING: the pi has $_pf MiB free, under 8192"; exit 6; }
  record "pi $PI_HOST reachable, no qemu running, $_pf MiB free"
  [ -z "${DRYRUN:-}" ] || { record "DRYRUN: the run preconditions hold; nothing synced"; exit 0; }

  pi "rm -rf ~/$PI_TREE && mkdir -p ~/$PI_TREE/build/kernel ~/$PI_TREE/build/kernel-undefined ~/$PI_TREE/build/fixtures"
  git archive "$COMMIT" | pi "tar -x -C ~/$PI_TREE"
  for f in $ARTS; do
    [ "$f" = build/fixtures/pool.img ] && continue
    scp -q -o BatchMode=yes "$f" "$PI_HOST:$PI_TREE/$f"
  done
  # The pool is sparse (2.5 GiB apparent): gzip on the wire, holes on landing,
  # then the exact length -- the manifest check below is the proof.
  _ps=$(wc -c < build/fixtures/pool.img | tr -d ' ')
  gzip -1 -c build/fixtures/pool.img | pi "gunzip -c | dd of=$PI_TREE/build/fixtures/pool.img bs=1M iflag=fullblock conv=sparse status=none && truncate -s $_ps $PI_TREE/build/fixtures/pool.img"
  pi "cd $PI_TREE && sha256sum -c --quiet -" < "$BAKE/manifest.txt" || { echo "the pi's copy does not match the manifest"; exit 1; }
  record "synced $COMMIT + the manifest's $(wc -l < "$BAKE/manifest.txt" | tr -d ' ') artifacts to $PI_HOST:~/$PI_TREE; sha256 verified on the pi"
  for f in $TWINS; do pi "cp --sparse=always $PI_TREE/$f $PI_TREE/$f.baked-snapshot"; done
  for f in $TWINS; do /usr/bin/grep -F "  $f" "$BAKE/manifest.txt" | /usr/bin/grep -c . | /usr/bin/grep -qx 1 || { echo "the manifest has no single line for $f"; exit 1; }; done
  for f in $TWINS; do /usr/bin/grep -E "  $f\$" "$BAKE/manifest.txt" | sed 's/$/.baked-snapshot/'; done | pi "cd $PI_TREE && sha256sum -c --quiet -" \
    || { echo "the twins made on the pi do not match the manifest"; exit 1; }
  record "snapshot twins made on the pi from the verified pool and key; sha256 verified against the manifest"

  # The driver runs detached on the pi and leaves DONE behind it.
  {
    echo 'cd ~/'"$PI_TREE"' || exit 9'
    echo 'echo $$ > driver.pid'
    for cfg in $PI_CONFIGS; do
      _l=pi-$(echo "$cfg" | cut -d: -f1); _c=$(echo "$cfg" | cut -d: -f2)
      _s=$(echo "$cfg" | cut -d: -f3); _to=$(echo "$cfg" | cut -d: -f4)
      echo "SMP_KEEP_LOGS=1 BOOT_TIMEOUT=$_to bash tools/smp-multiboot.sh $_l $_c $PI_N $_s > $_l.out 2>&1; echo \$? > $_l.rc"
    done
    echo 'date -u +%Y-%m-%dT%H:%M:%SZ > DONE'
  } > "$OUT/pi-driver.sh"
  scp -q -o BatchMode=yes "$OUT/pi-driver.sh" "$PI_HOST:$PI_TREE/pi-driver.sh"
  pi "cd ~/$PI_TREE && nohup setsid sh pi-driver.sh > pi-driver.log 2>&1 < /dev/null &"
  echo "$OUT" > "$LASTRUN"
  record "driver started on the pi: $PI_CONFIGS, N=$PI_N"
  wait_run
}

wait_run() {
  # The run's own bake, not LAST-BAKE: a later bake must not re-judge this run.
  EXPECT=$(sed -n 's/^expect //p' "$(cat "$OUT/bake.txt")/provenance.txt")
  [ -n "$EXPECT" ] || { echo "REFUSING: no expected count for $OUT"; exit 3; }
  # A dropped tunnel is a failed poll, not a failed run.
  _misses=0
  until pi "test -f ~/$PI_TREE/DONE" 2>/dev/null; do
    if pi true 2>/dev/null; then _misses=0; else _misses=$((_misses + 1)); fi
    [ "$_misses" -lt 30 ] || { record "the pi was unreachable for 30 polls in a row; re-attach with: sh work/oct22-merge/pi-smp.sh wait"; exit 7; }
    sleep 60
  done
  record "the pi reports DONE at $(pi "cat ~/$PI_TREE/DONE")"
  pi "cd ~/$PI_TREE && tar -czf - pi-driver.log *.out *.rc build/multiboot-logs build/multiboot-fails 2>/dev/null" | tar -xzf - -C "$OUT"
  record "logs pulled into $OUT"
  if judge "$OUT" "$EXPECT"; then
    record "PI AXIS GREEN: every boot of every config ($PI_CONFIGS, N=$PI_N) at $EXPECT/$EXPECT, D7 once, no extinction"
  else
    record "PI AXIS RED: see the lines above"; exit 1
  fi
}

case "$PHASE" in
  bake) bake ;;
  run) run ;;
  wait) OUT=$(cat "$LASTRUN"); wait_run ;;
  # Re-judge a pulled run directory, or a fixture built to test the judge.
  judge) OUT=${2:?usage: judge <run dir> <expect>}; judge "$2" "${3:?usage: judge <run dir> <expect>}" ;;
  *) echo "usage: sh work/oct22-merge/pi-smp.sh bake|run|wait|judge"; exit 2 ;;
esac
