#!/bin/sh
# AS-R9 guest verification -- the lease window, in order. PREPARED OFF-LEASE so
# the scarce Mac lease is spent executing, not exploring.
#
# This base PREDATES main's disk_floor_check (grep count 0 in tools/build.sh),
# so the floor is enforced HERE instead: an unguarded bake on a shared volume is
# what broke every agent's shell at 10:46Z. Refuse rather than ENOSPC.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
# The floor is not here to protect ME -- it is here to stop my bake pushing the
# shared volume below MAIN's floor (THYLACINE_MIN_FREE_GB=6), which would make
# THEIR build.sh refuse mid-landing. So: 6 (their floor) + ~1 GB (my expected
# delta) + margin. THE DELTA, RE-MEASURED rather than recalled: a sanitizer
# build goes to its OWN directory (build.sh:232 -> build/kernel-undefined), so
# ci-smp-gate's second flavour is ADDITIVE -- but build/kernel is only 33M, so
# both flavours together are ~70-100M. The real consumer is the pool, and it is
# smaller than it looks: pool.img is SPARSE (2684354560 bytes logical, 315M on
# disk), so a re-mint materialises only the blocks it writes, plus the
# .baked-snapshot twins the bake refreshes -- call it ~600M, not the ~200M
# first estimated here. Free was 7.4 GiB at 21:0xZ and 9 GiB at 22:3xZ: the
# volume moves by whole GiB on main's activity, so re-read it at EVERY stage.
# A `floor` refusal is a CORRECT outcome, not an obstacle -- lowering FLOOR_GB
# to get a run is exactly how the shared volume reaches main's own 6 GiB floor
# and breaks THEIR build mid-landing.
FLOOR_GB=${FLOOR_GB:-8}

free_gb() { df -g . | awk 'NR==2 {print $4}'; }

# Overridable only so the HEAD-equality refusal below has a testable arm.
ASTRA_TREE=${ASTRA_TREE:-../thylacine-astra}

# AMBUSH FORK PIN (aux, yip 0179). build.sh bakes from ${AMBUSHFORK:-~/projects/ambush}
# and THIS base predates main's cf296caa1, so ambush_fork_check still requires
# held_on_thylacine.go to declare launchHeld = true. aux is moving shared master
# to c3c7914, which deletes that file -- so consuming the shared default would
# break this bake the moment they land it. Pin to the worktree aux left at
# 073faaa, and REFUSE rather than fall back: a silent fallback to a moved master
# would fail later, inside the build, with a message about the fork instead of
# about the pin.
AMBUSHFORK=${AMBUSHFORK:-$HOME/projects/ambush-pin-073faaa}
_af_held="$AMBUSHFORK/pkg/proc/native/held_on_thylacine.go"
if [ ! -f "$_af_held" ]; then
  echo "REFUSING: AMBUSHFORK=$AMBUSHFORK has no pkg/proc/native/held_on_thylacine.go;"
  echo "  build.sh:126 needs it. Point AMBUSHFORK at a fork at/behind 073faaa."
  exit 4
fi
if ! grep -q '^const launchHeld = true$' "$_af_held"; then
  echo "REFUSING: $_af_held does not declare launchHeld = true (build.sh:125)."
  exit 4
fi
export AMBUSHFORK
PROV=work/oct5-as-r9/provenance.log
# astra, 0169 t4 + 0161 t9: retain the exact source/config and PAIRED IMAGE
# hashes with each result. A verdict without them cannot be attributed to a
# specific image later, and "I rebuilt it" is not a hash. The pool and the
# key-bearing ramfs are a CRYPTOGRAPHIC PAIR -- recording both hashes together
# is what makes "paired" checkable rather than asserted.
provenance() {
  {
    echo "=== $1 -- $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
    echo "my HEAD        : $(git rev-parse HEAD)"
    echo "my base        : $(git rev-parse 5ff62b788)"
    echo "astra HEAD     : $(git -C ../thylacine-astra rev-parse HEAD 2>/dev/null || echo n/a)"
    echo "free GiB       : $(free_gb)"
    # THE EXTERNAL STRATUM PIN. build.sh consumes $STRATUM_SRC (default
    # ~/projects/stratum/v2) read-only, and NOTHING recorded which tree or which
    # commit went into stratumd. That omission is precisely why D7 cost an hour
    # of attribution: astra's image was built from stratum-astra @61dde37 (the
    # session-DEK leases D7's same-user overlap requires) and mine from the
    # shared tree @ac519fc, which lacks them -- an unequal input invisible in
    # every hash I recorded.
    echo "STRATUM_SRC    : ${STRATUM_SRC:-$HOME/projects/stratum/v2}"
    _ss="${STRATUM_SRC:-$HOME/projects/stratum/v2}"
    echo "stratum HEAD   : $(git -C "$_ss" rev-parse HEAD 2>/dev/null || echo n/a)"
    echo "stratum dirty  : $([ -z "$(git -C "$_ss" status --porcelain 2>/dev/null)" ] && echo no || echo YES)"
    # The SANITIZER elf is hashed too: ci-smp-gate boots it for 2 of its 5
    # rows, so a provenance record naming only the default kernel describes
    # less than half the gate (astra, 0161 note 8).
    for f in build/.config build/kernel/thylacine.elf build/kernel-undefined/thylacine.elf \
             build/ramfs.cpio build/fixtures/pool.img; do
      [ -f "$f" ] && echo "$(shasum -a 256 "$f" | cut -c1-16)  $f" || echo "(absent)          $f"
    done
  } >> "$PROV"
  echo "-- provenance recorded: $1 (-> $PROV)"
}

# NO BLIND RETRY (astra, 0161 t9: "diagnose any failure before retrying"). A
# re-run after a red, with nothing changed and nothing understood, converts a
# finding into a flake -- which is the dismissal this project treats as a bug in
# itself. Every exit path below is therefore terminal: stop, keep the log, hand
# the machine back, and diagnose OFF the lease, since reading needs no cores.
floor() {
  g=$(free_gb)
  [ "$g" -ge "$FLOOR_GB" ] || { echo "REFUSING at stage '$1': ${g} GiB free < ${FLOOR_GB} GiB floor"; exit 1; }
  echo "-- stage '$1': ${g} GiB free (floor ${FLOOR_GB})"
}

# PRESERVATION IS A STEP, NOT A MEMORY. Every 2026-10-07 private-owner verdict
# attached to four boot inputs that lived ONLY in build/, and a bake destroys
# all four: build.sh:440 wipes the ramfs staging tree and mkcpio.py (:812)
# rewrites ramfs.cpio in place, :3482 deletes pool.img and system.key, :4554
# refreshes the .baked-snapshot twins, and this script's own invalidation
# removes both kernel trees. That is how the ubsan flat binary and the ramfs
# were lost (astra, 0161 t43); only the kernel half could be reconstructed, and
# only because a flat binary is a pure function of a retained ELF. The ramfs was
# unrecoverable. So the inputs are cloned OUT of build/ by the script, at the
# one moment they are all present, instead of depending on anyone remembering.
#
# QEMU BOOTS THE FLAT BINARY, NEVER THE ELF (run-vm.sh:35), and the PRE-BOOT
# pool is the .baked-snapshot, NOT the live pool.img: smp-multiboot.sh restores
# the snapshot before every boot, so the live file is what the last boot LEFT.
# Preserving the live one would preserve an artifact no boot ever read.
#
# BOUNDED ON PURPOSE. cp -c costs no blocks today, but a clone's shared blocks
# become REAL the moment the original is rebaked (~283 MiB for the sparse pool),
# so an unbounded history would leak into the very floor this script guards.
PRESERVE_DIR=work/oct5-as-r9/boot-inputs
KEEP_INPUT_GENS=${KEEP_INPUT_GENS:-2}
preserve_boot_inputs() { # preserve_boot_inputs <label>
  _d="$PRESERVE_DIR/$1"
  rm -rf "$_d"
  mkdir -p "$_d" || { echo "REFUSING: cannot create $_d"; return 1; }
  _kept=0
  _missing=
  # BOTH flavours share the basename thylacine.elf/.bin, so the destination
  # name carries the flavour. A flat copy would have one overwrite the other
  # and the manifest would then claim four files while holding two.
  for _pair in \
    "build/kernel/thylacine.elf:thylacine.elf" \
    "build/kernel/thylacine.bin:thylacine.bin" \
    "build/kernel/.config:.config" \
    "build/kernel-undefined/thylacine.elf:thylacine-undefined.elf" \
    "build/kernel-undefined/thylacine.bin:thylacine-undefined.bin" \
    "build/ramfs.cpio:ramfs.cpio" \
    "build/fixtures/pool.img.baked-snapshot:pool.img.baked-snapshot" \
    "build/fixtures/system.key:system.key" ; do
    _src=${_pair%%:*}
    _dst=${_pair##*:}
    if [ ! -f "$_src" ]; then _missing="$_missing $_src"; continue; fi
    # A preservation that silently drops a file it FOUND is the exact failure
    # this step exists to prevent, so a failed copy is fatal. An input that is
    # merely ABSENT is recorded as absent and never substituted.
    cp -c "$_src" "$_d/$_dst" 2>/dev/null || cp "$_src" "$_d/$_dst" || {
      echo "REFUSING: found $_src but could not preserve it to $_d/$_dst"
      return 1; }
    _kept=$((_kept + 1))
  done
  _h=$(mktemp)
  ( cd "$_d" && for _g in .config *; do
      [ -f "$_g" ] || continue
      printf '%s  %s\n' "$(shasum -a 256 "$_g" | cut -c1-16)" "$_g"
    done ) > "$_h"
  mv "$_h" "$_d/HASHES.txt"
  echo "-- preserved $_kept boot input(s) -> $_d"
  [ -n "$_missing" ] && echo "   ABSENT, recorded and NOT substituted:$_missing"
  # Prune oldest generations. Newest-first, keep KEEP_INPUT_GENS.
  _n=0
  for _old in $(ls -1dt "$PRESERVE_DIR"/*/ 2>/dev/null); do
    _n=$((_n + 1))
    [ "$_n" -gt "$KEEP_INPUT_GENS" ] && rm -rf "$_old" && \
      echo "   pruned old generation $_old (keeping $KEEP_INPUT_GENS)"
  done
  return 0
}

floor start
provenance "stage-start (nothing built yet)"

# Stage 0 -- THE SPEC OBLIGATION, and it is INDEPENDENT of everything below.
# CLAUDE.md: any change to a mechanism modelled in specs/ re-runs that spec's
# buggy cfgs. Two apply:
#   burrow.tla   -- models the dual-refcount lifecycle (I-7) whose {0,0}
#                   decision this repair RELOCATED into the settled drops.
#                   3 buggy cfgs: free_on_close (66), free_on_unmap (54),
#                   never_free (43) -- each must still violate NoUseAfterFree.
#   capacity.tla -- one of the specs that models the CHARGE accounting (I-32),
#                   which is the mechanism AS-R9 is about.
# These need NO image, NO ramfs, NO pool and none of astra's artifacts -- only
# java + TLC. So they run on the lease regardless of how yip 0169 is ruled, and
# they are the first thing to do when the lease lands.
# TRAP: ~/tla2tools.jar is STALE -- use the jar SPEC-POLICY.md names.
if [ "${SPECS:-1}" = 1 ]; then
  # The jar lives in /tmp, which the 2026-10-06 reboot CLEARED -- and
  # ~/tla2tools.jar is STALE (it reports violated temporal properties
  # differently). Fetch SPEC-POLICY's pinned release, then PRINT the version,
  # because the whole point of pinning it is that the output format differs.
  JAR=/tmp/tla2tools.jar
  # FAIL LOUDLY IF THE TOOL IS ABSENT. First run of this stage printed "Unable to
  # locate a Java Runtime" five times and CONTINUED, reporting no verdict at all
  # -- a stage that cannot run must not look like a stage that passed. That is the
  # gauge-reading-zero trap inside my own harness, and it would have let me report
  # "specs re-run" on a run where TLC never started.
  # /usr/bin/java on macOS is a STUB that reports "Unable to locate a Java
  # Runtime" even when a JDK is installed -- brew's openjdk is not linked into
  # /usr/libexec, so java_home does not see it either. Prefer the brew JDK
  # explicitly; derive the path, never assume PATH has been fixed.
  for cand in /opt/homebrew/opt/openjdk/bin/java \
              /opt/homebrew/opt/openjdk@21/bin/java \
              /opt/homebrew/opt/openjdk@17/bin/java; do
    [ -x "$cand" ] && { JAVA="$cand"; break; }
  done
  JAVA="${JAVA:-java}"
  echo "   java: $JAVA"
  if ! "$JAVA" -version >/dev/null 2>&1; then
    echo "   STAGE 0 BLOCKED: no working Java runtime (/usr/bin/java is the macOS"
    echo "   stub; /usr/libexec/java_home reports none). TLC cannot run, so the"
    echo "   burrow.tla + capacity.tla obligation is UNDISCHARGED -- not passed."
    echo "   This needs no lease to fix; do it off-lease and re-run SPECS=1."
    [ "${SPECS_MAY_BLOCK:-0}" = 1 ] || exit 3
    echo "   SPECS_MAY_BLOCK=1 -- continuing to the guest stages with the spec"
    echo "   obligation RECORDED AS OUTSTANDING. It is not satisfied."
  else
    # SSL_CERT_FILE: a stale one makes curl fail with (77). /tmp is wiped on reboot.
    [ -f "$JAR" ] || SSL_CERT_FILE=/etc/ssl/cert.pem curl -sL -o "$JAR" \
      https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
    # Print the version: a pinned tool whose version goes unprinted is not pinned,
    # and ~/tla2tools.jar is a stale 2.19 that reports temporal violations WITHOUT
    # the property name, which breaks every by-name verdict.
    "$JAVA" -cp "$JAR" tlc2.TLC 2>&1 | tee "$ROOT/work/oct5-as-r9/spec-tlc-version.txt" \
        | grep -m1 'TLC2 Version' || true
    SPECS_RAN=1
  fi

  # CLEAN-cfg runs are SUSPENDED (SPEC-POLICY, since 2026-05-21). The binding
  # obligation is the BUGGY cfgs: each must STILL produce its counterexample.
  # A buggy cfg that now PASSES is a FINDING, not a convenience -- it means the
  # spec stopped constraining the thing it was written to catch, which is
  # exactly how a repair can silently void its own proof.
  if [ "${SPECS_RAN:-0}" = 1 ]; then
  # RETAIN THE FULL TLC OUTPUT, not just the lines this stage greps for. The
  # first version captured stdout into `$out` and printed three matched lines,
  # so the only durable record of a spec run was those lines in whatever log
  # happened to hold the stage's stdout -- a gate keeping its VERDICT and
  # discarding its EVIDENCE, which is the same defect I had already fixed in
  # tools/smp-multiboot.sh and did not recognise here (astra, 0161 review R2:
  # she went looking for the TLC verdicts and found none). Writing each cfg's
  # whole output to its own file costs nothing and makes the claim checkable
  # by someone who was not in the room.
  # PER-RUN DIRECTORY, AND NOTHING IS EVER DELETED. The first version did
  # `rm -rf "$SPECDIR"` before running, which would have destroyed the previous
  # run's evidence -- including a FAILING run's -- on the next invocation
  # (astra, 0161 R4). That is the same archive-never-delete rule I had already
  # applied in tools/smp-multiboot.sh's retention and then broke here: a
  # retention path that clears itself is a retention path with a one-run memory.
  SPECDIR="$ROOT/work/oct5-as-r9/spec-logs/$(date -u +%Y%m%dT%H%M%SZ)"
  [ -e "$SPECDIR" ] && { echo "REFUSING: $SPECDIR already exists -- refusing to write over a prior run"; exit 3; }
  mkdir -p "$SPECDIR" || { echo "REFUSING: cannot create $SPECDIR"; exit 3; }
  echo "-- model logs for this run: $SPECDIR (prior runs are kept, never cleared)"
  cd specs
  # burrow.tla -- I-7, the dual-refcount lifecycle whose {0,0} decision this
  # repair RELOCATED into the settled drops. Each must violate NoUseAfterFree.
  for c in burrow_buggy_free_on_close burrow_buggy_free_on_unmap burrow_buggy_never_free; do
    echo "-- $c (expect: NoUseAfterFree VIOLATED)"
    # TLC EXITS 12 ON A VIOLATION -- the EXPECTED result here. Under `set -e`
    # that aborted the stage silently after printing only the header, so capture
    # with || true and judge by CONTENT.
    out=$("$JAVA" -cp "$JAR" tlc2.TLC -workers auto -deadlock -config "$c.cfg" burrow.tla 2>&1 || true)
    printf '%s\n' "$out" > "$SPECDIR/$c.log" || { echo "   REFUSING: could not retain $c output"; exit 3; }
    # NOT `[ -s ]`: `printf '%s\n' "$out"` on empty output writes a single
    # NEWLINE, so the file is 1 byte and -s calls it non-empty -- the guard could
    # not fire on the one condition it exists for (measured in replica, 10-07).
    # Ask for a non-whitespace character instead.
    grep -q '[^[:space:]]' "$SPECDIR/$c.log" \
      || { echo "   REFUSING: $SPECDIR/$c.log has no content -- TLC printed nothing"; exit 3; }
    echo "$out" | grep -E 'is violated|states generated|Model checking completed' | head -3
    # burrow's cfgs declare `INVARIANTS Invariants` -- ONE CONJUNCTION (TypeOk
    # /\ RefcountConsistent /\ NoUseAfterFree) -- so TLC names the CONJUNCTION,
    # never the member. Grepping for NoUseAfterFree FAILS A CORRECT RUN, which is
    # what my first version did. Assert what TLC actually emits.
    echo "$out" | grep -q 'Invariant Invariants is violated' \
      || { echo "   FAIL: $c produced NO violation. A buggy cfg that no longer"; \
           echo "   violates is a FINDING -- the spec stopped constraining what it"; \
           echo "   was written to catch. Diagnose; do not retry."; exit 3; }
    echo "   OK: violation reported"
  done
  # capacity.tla -- the I-32 charge accounting itself. detach_no_refund is
  # literally AS-R9's second arm: the holder that frees finds the record
  # cleared and refunds nothing. DISCRIMINATING form: each must violate
  # NoOrphan *with ChargeConserved listed ahead of it and HOLDING*. A run that
  # reported the COUNTER violated instead would mean the model no longer says
  # the counter is blind -- a different finding, not a pass.
  for c in capacity_buggy_detach_no_refund capacity_buggy_replace_orphans; do
    echo "-- $c (expect: NoOrphan VIOLATED, ChargeConserved HOLDING)"
    out=$("$JAVA" -cp "$JAR" tlc2.TLC -workers auto -deadlock -config "$c.cfg" capacity.tla 2>&1 || true)
    printf '%s\n' "$out" > "$SPECDIR/$c.log" || { echo "   REFUSING: could not retain $c output"; exit 3; }
    # NOT `[ -s ]`: `printf '%s\n' "$out"` on empty output writes a single
    # NEWLINE, so the file is 1 byte and -s calls it non-empty -- the guard could
    # not fire on the one condition it exists for (measured in replica, 10-07).
    # Ask for a non-whitespace character instead.
    grep -q '[^[:space:]]' "$SPECDIR/$c.log" \
      || { echo "   REFUSING: $SPECDIR/$c.log has no content -- TLC printed nothing"; exit 3; }
    echo "$out" | grep -E 'is violated|states generated|Model checking completed' | head -3
    # capacity's cfgs declare TypeOk, ChargeConserved and NoOrphan SEPARATELY, so
    # TLC DOES name the specific one -- which is why SPEC-TO-CODE requires NoOrphan
    # violated with ChargeConserved ahead of it and HOLDING. Both halves checkable.
    echo "$out" | grep -q 'Invariant NoOrphan is violated' \
      || { echo "   FAIL: $c did not violate NoOrphan. Diagnose; do not retry."; exit 3; }
    if echo "$out" | grep -q 'Invariant ChargeConserved is violated'; then
      echo "   FAIL: $c reported ChargeConserved instead. The model no longer says"
      echo "   the counter is blind -- a DIFFERENT finding, not a pass. Diagnose."
      exit 3
    fi
    echo "   OK: NoOrphan violated, ChargeConserved holding"
  done
  cd "$ROOT"
  fi
fi
floor post-specs

# SPECS_ONLY=1 stops here. The spec stage is the only part of this runbook that
# needs nothing built, so making it independently runnable means its own
# retention path can be verified on a SHORT lease instead of riding a 44-minute
# matrix that astra explicitly said not to rerun. Without this the only way to
# exercise the stage was to re-run everything, which is how a retention fix goes
# unverified: the cheapest honest check was more expensive than the work.
if [ "${SPECS_ONLY:-0}" = 1 ]; then
  echo "== SPECS_ONLY=1 -- stopping after the model stage, nothing built, nothing booted =="
  exit 0
fi

# Stage 1 -- the warm cache.
#
# ASTRA'S CLONE APPROVAL IS SPENT (0161 t17). The cache-copy acknowledgement she
# conditioned it on was received and accepted (0169 t5/t6), her build/ is NO
# LONGER held stable for me, and she asked for a NEW coordination check before
# any re-clone because her tree may have moved since. So CLONE_APPROVED=1 is no
# longer authority to read her tree: a discharged approval is not a standing one.
# The normal path is MY OWN cache, which this tree has.
if [ -d build ]; then
  echo "-- cache: using MY OWN build/ (no clone -- astra's approval is spent)"
elif [ "${CLONE_RECHECKED:-0}" = 1 ]; then
  echo "-- cache: re-cloning astra's build/ under a FRESH coordination check"
  cp -Rc ../thylacine-astra/build build
  rsync -a --ignore-existing ../thylacine-astra/third_party/rust/ third_party/rust/ 2>/dev/null || true
else
  echo "REFUSING: this tree has no build/, and astra's clone approval is SPENT"
  echo "   (0161 t17 -- her tree may have changed and is not held stable for me)."
  echo "   Ask her on yip, then set CLONE_RECHECKED=1. Re-reading a peer's tree"
  echo "   on an approval that has already been discharged is not consent."
  exit 4
fi

# INVALIDATION RUNS UNCONDITIONALLY. It used to sit inside the clone branch, so
# the own-cache path -- now the NORMAL one -- invalidated nothing and would have
# reused a stale kernel tree, stale CMake caches and the stale staged daemon.
# What must be rebuilt does not depend on how build/ arrived.
# The STAGED daemon goes too, not just its CMake tree: build_ramfs installs
# build/pouch/progs/stratumd into the ramfs (build.sh:780-784), so a binary left
# there from an earlier build would be baked in even if this run never rebuilt
# it. Deleting it converts a SILENT stale daemon into a LOUD absence.
rm -rf build/kernel build/usr build/pouch/stratumd-cmake build/kernel-undefined build/host-stratum
rm -f build/pouch/progs/stratumd build/ramfs-src/bin/stratumd

# CACHE INVALIDATION BY SOURCE DIFFERENCE, not by path-boundness. Astra raised
# this and she is right: our HEADs are equal, but her WORKING TREE is dirty, so
# HEAD equality does not make her objects equivalent to mine. Her dirty files
# ARE the source delta between us. An object she compiled from her uncommitted
# usr/halcyond/src/layout.rs could be judged fresh against my committed copy
# (older mtime) and never rebuild -- putting HER unreviewed code in MY image and
# making any failure of mine unattributable. Derive the set, never hardcode it.
echo "-- invalidating cache entries whose SOURCE differs from mine:"
git -C ../thylacine-astra status --porcelain \
  | awk '{print $2}' | grep -E '^(usr|lib)/' > /tmp/astra-dirty-src.txt || true
if [ -s /tmp/astra-dirty-src.txt ]; then
  while IFS= read -r f; do
    if [ -e "$f" ]; then
      touch "$f"            # newer than her object -> cargo/ninja MUST rebuild it
      echo "   invalidated: $f (dirty in her tree; rebuilding from MY source)"
    else
      echo "   NOTE: $f dirty in her tree but absent in mine -- inspect before trusting the cache"
    fi
  done < /tmp/astra-dirty-src.txt
else
  echo "   none -- no uncommitted usr/ or lib/ source in her tree"
fi
# And prove the only deltas are her dirty files: identical HEAD + her status.
aHEAD=$(git -C "$ASTRA_TREE" rev-parse HEAD 2>/dev/null || echo unknown)
myBASE=$(git rev-parse 5ff62b788)
echo "   her HEAD: $aHEAD  my base: $myBASE"
# ASSERTED, not merely printed -- the third time that distinction has caught
# something here. The dirty-file invalidation above rests on a premise: that her
# objects differ from mine ONLY by her uncommitted files. That premise is FALSE
# the moment her HEAD moves off my base, because an object she compiled from a
# commit I do not have never appears in her `status` output, so the invalidation
# cannot see it and her unreviewed source rides into my image -- which is
# exactly what makes a failure of mine unattributable. The printed pair sat here
# for two runs with nothing comparing it.
if [ "$aHEAD" != "$myBASE" ]; then
  if [ "${ASTRA_HEAD_MOVED_OK:-0}" = 1 ]; then
    echo "   her HEAD has MOVED off my base -- continuing on ASTRA_HEAD_MOVED_OK=1"
  elif [ "$aHEAD" = unknown ]; then
    echo "   REFUSING: could not read a HEAD from $ASTRA_TREE at all, so nothing"
    echo "   below can claim her dirty files are the whole delta."
    exit 4
  else
    echo "   REFUSING: her HEAD has MOVED off my base, so her dirty-file set is no"
    echo "   longer the whole delta between our trees -- an object built from a"
    echo "   commit I do not have is invisible to it. Coordinate with her, then"
    echo "   set ASTRA_HEAD_MOVED_OK=1 if the cache is still safe to inherit."
    exit 4
  fi
fi
floor post-clone

# Stage 2 -- MY kernel from MY source. The only thing the cache must not supply.
#
# THE EXTERNAL STRATUM PIN, which the original handoff did not state and this
# runbook did not set -- the cause of the D7 red. docs/SRV-SESSION-REGISTRY-DESIGN.md
# 285-290: "The same-user acceptance also requires Stratum to retain one
# authenticated DEK lease per connection/dataset pair. Each new connection proves
# UNWRAP even if another session has installed the key... Implementation is
# isolated in `stratum-astra`." Without it, the overlapping same-user session
# cannot prove UNWRAP and install-dek returns eaccess -- exactly the observed red.
export STRATUM_SRC="${STRATUM_SRC:-$HOME/projects/stratum-astra/v2}"
STRATUM_PIN="${STRATUM_PIN:-61dde37}"
# ASSERT the pin; never infer it from the path. A directory named stratum-astra
# is not evidence that it is AT the commit D7 needs.
#
# AND NOTE WHICH HALF ACTUALLY DISCRIMINATES: `rev-parse 61dde37` SUCCEEDS even
# in the shared tree, because that tree has the OBJECT (fetched) without the
# commit being in its history. So rev-parse alone is a check that cannot fail --
# it only proves the sha is spellable. The HEAD-EQUALITY check below is the
# load-bearing half; verified both ways (stratum-astra accepted, the shared
# tree refused). There is deliberately NO ancestry test here any more -- see
# the next block for why one would not pin a controlled run.
sh=$(git -C "$STRATUM_SRC" rev-parse --short "$STRATUM_PIN" 2>/dev/null || true)
if [ -z "$sh" ]; then
  echo "REFUSING: $STRATUM_SRC does not contain $STRATUM_PIN."
  echo "D7's same-user overlap needs the session-DEK leases from that commit;"
  echo "building without it reproduces the eaccess red by construction."
  exit 4
fi
# EXACT EQUALITY, not ancestry (astra, 0161 t13 -- and she is right). An
# ancestry test PASSES for every later descendant, so it cannot enforce a
# CONTROLLED experimental pin: if stratum-astra advances, the guard would
# silently accept a different source than the one the experiment names. Require
# HEAD to EQUAL the full commit resolved from the pin.
STRATUM_PIN_FULL="61dde3727921e70e2c72fbd3c9e2044a192f4a54"
shead=$(git -C "$STRATUM_SRC" rev-parse HEAD 2>/dev/null || true)
if [ "$shead" != "$STRATUM_PIN_FULL" ]; then
  echo "REFUSING: $STRATUM_SRC HEAD is $shead"
  echo "          the controlled run requires exactly $STRATUM_PIN_FULL"
  echo "An ancestry test would have accepted a descendant; this run needs THE pin."
  exit 4
fi
# A DIRTY external source defeats the pin. The equality check proves which
# commit HEAD NAMES; it says nothing about what is on disk, and build.sh
# consumes the WORKING TREE. This is the same proxy-for-the-thing error that made
# D7 unattributable -- a recorded identity standing in for the bytes consumed --
# so refuse rather than record a pin the build may not have used.
if [ -n "$(git -C "$STRATUM_SRC" status --porcelain 2>/dev/null)" ]; then
  echo "REFUSING: $STRATUM_SRC has uncommitted changes. The pin names a commit,"
  echo "but build.sh consumes the working tree, so the image would not be the"
  echo "pinned source. Commit, stash, or point STRATUM_SRC at a clean tree."
  git -C "$STRATUM_SRC" status --porcelain | head -5
  exit 4
fi
echo "-- stratum pinned: $STRATUM_SRC @ $(git -C "$STRATUM_SRC" rev-parse --short HEAD) (contains $STRATUM_PIN, tree CLEAN)"
# A timestamp taken BEFORE the build: `-nt` against it proves an artifact was
# written by THIS run rather than inherited from a previous one.
STAMP=$(mktemp)
tools/build.sh kernel --config ci
floor post-build


# VERIFY THE PIN IN THE OUTPUT, NOT ONLY IN THE INPUT. The HEAD equality above
# proves which source I SELECTED; it says nothing about what the build
# CONSUMED. build.sh calls build_stratumd in the all-flow (build.sh:394) and
# copies the result to $progs_out (build.sh:3347), whence build_ramfs installs
# it (build.sh:780-784) -- but if that step is skipped or fails, a previously
# staged binary is baked in and this script would report the pin honoured while
# the image ran the OLD Stratum. That is the same unequal input that cost an
# hour of attribution, so the check has to close on the artifact.
# CMakeCache is the file astra read by hand to find the mismatch; assert on it.
CC=build/pouch/stratumd-cmake/CMakeCache.txt
if [ ! -f "$CC" ]; then
  echo "REFUSING: $CC absent -- stratumd never configured, so the ramfs daemon"
  echo "          cannot be attributed to the pinned source."
  exit 4
fi
if ! grep -qF -- "$STRATUM_SRC" "$CC"; then
  echo "REFUSING: $CC does not name $STRATUM_SRC -- the configure consumed a"
  echo "          different tree than the pin selected:"
  grep -E 'SOURCE_DIR' "$CC" | head -5
  exit 4
fi
for b in build/pouch/progs/stratumd build/ramfs-src/bin/stratumd; do
  if [ ! -f "$b" ]; then
    echo "REFUSING: $b missing after the build -- the daemon did not reach the"
    echo "          ramfs staging, so the guest would run without it."
    exit 4
  fi
  if [ ! "$b" -nt "$STAMP" ]; then
    echo "REFUSING: $b is OLDER than this run -- a stale daemon survived and"
    echo "          would be baked in under the pinned source's name."
    exit 4
  fi
  echo "-- fresh from this run: $(shasum -a 256 "$b" | cut -c1-16)  $b"
done
echo "-- pin VERIFIED IN THE OUTPUT: stratumd configured from $STRATUM_SRC"
provenance "post-build (my kernel, paired images)"

# PRESERVE THIS RUN'S BOOT INPUTS, and HERE is where it belongs -- after the
# Stratum pin is verified in the OUTPUT, before any later stage can overwrite
# them. The placement is load-bearing in BOTH directions, which is why it moved:
# earlier (immediately after the build) a run about to be REJECTED for wrong
# provenance would still take a generation slot, and against a bounded history
# two rejected runs would evict both genuinely qualified sets -- the exact loss
# this step exists to prevent. Later (after the suite or the gate) a run that
# went RED would preserve nothing, and a failing run's inputs are precisely what
# diagnosis needs.
preserve_boot_inputs "run-$(date -u '+%Y%m%dT%H%M%SZ')" || exit 1

# Stage 3 -- verify the image by CONTENT, not by the build's exit code. This is
# the BAKE-TRAP class: failure looks like absent content plus a green ledger.
# A stale or mis-baked kernel cannot make the suite total rise by exactly 4.
ELF=build/kernel/thylacine.elf
# THE DENOMINATOR FIRST. If `strings` finds nothing at all -- wrong path, empty
# or truncated ELF -- every name below reads as ABSENT and the loop would blame
# my tests for a broken search. So prove the search searched something, using a
# name that exists at my BASE and that I did not add.
if ! strings "$ELF" | grep -qF 'burrow.refcount_lifecycle'; then
  echo "   CONTROL ABSENT: burrow.refcount_lifecycle is not in $ELF."
  echo "   The SEARCH is broken, not the tests -- check the path and the bake. STOP"
  exit 1
fi
echo "   control present: burrow.refcount_lifecycle (the search works)"
# grep -F: the '.' in a test name is a literal, not a regex any-char.
echo "-- the four witnesses must be REGISTERED in the built ELF:"
for t in settled_drop_retains_nonfinal_charge settled_drop_exact_payer \
         settled_mapping_drop_defers_free unmap_failure_leaves_mapping_attached; do
  strings "$ELF" | grep -qF "burrow.$t" \
    && echo "   present: burrow.$t" \
    || { echo "   ABSENT: burrow.$t -- the ELF does not carry my tests; STOP"; exit 1; }
done

# Stage 4 -- the suite. The 4 new tests execute for the FIRST time here. A
# failing suite extincts the boot (main.c: extinction("kernel test suite
# failed")), so a red is loud, not silent.
# CAPTURE THE STATUS, AND PRESERVE THE LOG BEFORE ANY ASSERTION CAN EXIT.
# A blanket `| tee` hides EVERY boot failure behind tee's exit 0, not just
# D7's (astra, 0161 note 8) -- and the first thing a failing assertion does is
# exit, which is exactly how I lost the first failing boot log. So: the real
# status through a sentinel, the log copied immediately, assertions after.
TESTRC=$(mktemp)
( set +e; tools/test.sh 2>&1; echo $? > "$TESTRC" ) | tee work/oct5-as-r9/guest-test.log
test_rc=$(cat "$TESTRC")
mkdir -p work/oct5-as-r9/boot-logs
BOOTLOG=work/oct5-as-r9/boot-logs/boot-confirm-$(date -u '+%H%M%SZ').log
if [ -f build/test-boot.log ]; then
  cp build/test-boot.log "$BOOTLOG"
else
  cp work/oct5-as-r9/guest-test.log "$BOOTLOG"
fi
echo "-- test.sh exit status: $test_rc; boot log PRESERVED at $BOOTLOG"
# AN ELF NAME IS NOT AN EXECUTION WITNESS (astra, 0169 turn 4). The stage-3 grep
# proves the tests are COMPILED IN; only the boot log proves they RAN. The suite
# prints "    [test] <name> ... " per test, so require a PASS record for each of
# the four BY NAME -- that is the witness, and the total alone is not.
# THE ORACLE IS THE BOOT LOG, NOT test.sh's STDOUT. This check asserted against
# guest-test.log and therefore FAILED ON BOTH RUNS SO FAR (10-06 and 10-07),
# each time with "absent from the log entirely" -- on 10-07 against a GREEN boot
# whose four PASS records were sitting in the boot log all along. test.sh prints
# a summary plus a ~20-line log TAIL; the suite's 1835 `[test]` lines live only
# in build/test-boot.log, preserved above as $BOOTLOG. The stage's own comment
# said "only the boot log proves they RAN" while the code read the other file --
# a comment true about the wrong thing, and it is why ci-smp-gate has never run.
# DENOMINATOR FIRST, so the mirror-image failure cannot happen either: if the
# oracle carries no [test] lines at all, the SEARCH is broken, not the tests.
ntest=$(grep -c '\[test\] ' "$BOOTLOG" || true)
echo "-- witness oracle: $BOOTLOG carries $ntest [test] lines"
[ "$ntest" -gt 0 ] || { echo "   NO [test] LINES IN THE ORACLE -- the suite output is not here; STOP"; exit 1; }
echo "-- EXECUTION witness: each of the four must have its own PASS record:"
for t in settled_drop_retains_nonfinal_charge settled_drop_exact_payer \
         settled_mapping_drop_defers_free unmap_failure_leaves_mapping_attached; do
  if grep -E "\[test\] burrow\.$t \.\.\..*(PASS|ok)" "$BOOTLOG" >/dev/null; then
    echo "   RAN+PASSED: burrow.$t"
  else
    echo "   NO PASS RECORD: burrow.$t -- compiled in is not run; show its line:"
    grep -F "burrow.$t" "$BOOTLOG" || echo "     (absent from the oracle entirely)"
    exit 1
  fi
done
echo "-- suite total must be base+4; a skip is NOT coverage (OPEN-BUGS: 17 ramfs"
echo "   probe tests pass when their initrd file is missing):"
# ASSERTED, not merely printed (astra, note 8): a `|| true` on the tally is a
# number nobody checks.
# EXPECT_TESTS IS DERIVED FROM THE REGISTRATION TABLE, never typed. It was
# pinned to 1834 and went stale the moment the private-owner port added its two
# tests (astra, 0161 t43): a guard pinned to a NAMED number is re-pointed by
# hand, one pinned to a DERIVED value cannot go stale.
# /usr/bin/grep BY ABSOLUTE PATH, not bare `grep`: Claude Code's embedded ugrep
# 7.8.4 silently undercounts THIS pattern on THIS file -- 757 of 1836, exit 0,
# no stderr (OPEN-BUGS). A derivation is only as sound as its counter.
# DENOMINATOR CONTROL: a count that collapses must refuse, never quietly lower
# the bar it exists to hold.
if [ -z "${EXPECT_TESTS:-}" ]; then
  EXPECT_TESTS=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
  [ "${EXPECT_TESTS:-0}" -ge 1000 ] || {
    echo "   REFUSING: derived only ${EXPECT_TESTS:-0} registrations from"
    echo "   kernel/test/test.c -- the DERIVATION is broken, not the suite."; exit 1; }
  echo "   expectation DERIVED from kernel/test/test.c: $EXPECT_TESTS registrations"
fi
tally=$(grep -E '  tests: [0-9]+/[0-9]+' "$BOOTLOG" | tail -1)
[ -n "$tally" ] || { echo "   NO SUITE TALLY AT ALL -- the suite never reported; STOP"; exit 1; }
echo "  $tally"
ran=$(echo "$tally" | sed -E 's#.*tests: ([0-9]+)/([0-9]+).*#\1#')
tot=$(echo "$tally" | sed -E 's#.*tests: ([0-9]+)/([0-9]+).*#\2#')
[ "$ran" = "$tot" ] || { echo "   only $ran of $tot passed; STOP"; exit 1; }
[ "$tot" = "$EXPECT_TESTS" ] || {
  echo "   total $tot != expected $EXPECT_TESTS (derived from the registration"
  echo "   table in kernel/test/test.c unless EXPECT_TESTS overrode it)."
  echo "   A total that MOVED means the test SET changed: account for it, do not"
  echo "   adjust the expectation to match the observation."; exit 1; }
nskip=$(grep -c '\[skip\]' "$BOOTLOG" || true)
echo "   [skip] lines: $nskip (must be 0 on the gate image)"
[ "$nskip" = 0 ] || { echo "   A SKIP IS NOT COVERAGE; STOP"; exit 1; }

# Stage 4b -- D7 on its OWN axis. Agreed with astra (0161): the controlled
# rebuild re-equalises the Stratum input, so whatever D7 does is evidence about
# THAT input, never about the charge-settlement repair. NON-FATAL by design --
# stage 5 must run whatever happens here.
echo "-- D7 axis (separate verdict): $BOOTLOG"
set +e
sh work/oct5-as-r9/d7-compare.sh "$BOOTLOG"
D7RC=$?
set -e
echo "-- D7 verdict code: $D7RC  (0 cured / 20 UNCHANGED known red / 22 refused but"
echo "   trace DIFFERS / 21 changed shape / 4 control failed)"
echo "   This is NOT the AS-R9 verdict -- stage 5 decides that one."
# D7 IS THE ONLY TOLERATED RED BOOT, and only because its cause is an external
# input this run deliberately re-equalises. Every OTHER boot failure stays an
# explicit failure (astra, note 8): collecting later evidence is not
# qualification of a red boot. So a nonzero test.sh passes here ONLY when the
# log's extinctions are joey's and nothing else.
if [ "$test_rc" != 0 ]; then
  n_ext=$(grep -c 'EXTINCTION' "$BOOTLOG" || true)
  n_joey=$(grep -c 'EXTINCTION: joey' "$BOOTLOG" || true)
  echo "-- test.sh RED: $n_ext extinction(s) in the log, of which joey: $n_joey"
  # EXACTLY 20, never "nonzero" (astra, 0161 note 10 -- and she is right). The
  # exception exists for the UNCHANGED KNOWN red and nothing else, but `!= 0`
  # also admitted 21 (changed shape -- a second cause in play), 4 (controls
  # failed -- the experiment is broken), 22 (refused with a DIFFERENT trace) and
  # any shell error. None of those establishes the understood failure, so none
  # of them earns a red boot a pass to the next stage.
  if [ "$n_joey" -gt 0 ] && [ "$n_ext" = "$n_joey" ] && [ "$D7RC" = 20 ]; then
    echo "   TOLERATED as the UNCHANGED known D7 signature (code 20, trace"
    echo "   byte-identical to the baseline) -- continuing to the SMP gate, which"
    echo "   is what AS-R9 is blocked on. NOT a qualification of a red boot."
  else
    echo "   FATAL: a red boot that is NOT the known D7 signature. STOP."
    grep 'EXTINCTION' "$BOOTLOG" | sed 's/^/     /' || true
    exit 1
  fi
fi


# Stage 5 -- the one that matters. AS-R9 is an SMP race: a single-CPU green
# proves little, and the Oct 1-2 single-boot waiver has expired.
floor pre-smp
# NARROWING THE MATRIX IS VERIFYING AROUND THE HAZARD (CLAUDE.md): a subset
# still prints a PASS, and AS-R9 is precisely an SMP race.
if [ -n "${SMP_GATE_CONFIGS:-}" ]; then
  echo "REFUSING: SMP_GATE_CONFIGS='$SMP_GATE_CONFIGS' narrows the 5-row matrix"; exit 5
fi
if [ "${SMP_GATE_N:-10}" -lt 10 ]; then
  echo "REFUSING: SMP_GATE_N=${SMP_GATE_N:-10} < 10 -- a race needs the full N"; exit 5
fi
# RETAIN EVERY BOOT'S LOG. build/test-boot.log is overwritten by the next boot,
# so a PASSING boot's evidence is gone the moment the next one starts -- and
# then no per-boot question can be answered after the fact. smp-multiboot.sh
# keeps all of them when this is set (default off, so no peer's gate changes).
export SMP_KEEP_LOGS=1
# MY OWN freshness datum for the logs I am about to read. The gate stamps each
# kept file too, but that only proves the file is from the run that WROTE it --
# if retention never ran here, an earlier run's directory could still hold
# exactly N files and satisfy my count with stale evidence. A reader asks the
# question for itself.
GATESTAMP=$(mktemp)
# Compared NUMERICALLY via stat, not with `-nt`: /bin/sh here is bash 3.2, whose
# `[ a -nt b ]` truncates to whole seconds (measured -- a file 2.5 ms newer than
# the stamp read as NOT newer), while bash 5.3's `[[ -nt ]]` is sub-second. A
# guard whose correctness rests on the gate being slow is a guard resting on a
# premise nobody states, so it rests on a number instead.
GATESTAMP_M=$(stat -f %m "$GATESTAMP")
SMPRC=$(mktemp)
( set +e; tools/ci-smp-gate.sh 2>&1; echo $? > "$SMPRC" ) | tee work/oct5-as-r9/guest-smp.log
smp_rc=$(cat "$SMPRC")
# A GATE HAS TWO HALVES -- VERDICT AND CAPTURE. The tee is capture only: the
# pipeline's status is tee's, so set -e cannot see this gate fail. Assert it.
echo "-- ci-smp-gate exit status: $smp_rc (0 = every config passed)"
[ "$smp_rc" = 0 ] || { echo "   SMP GATE RED. Logs: build/multiboot-fails/. STOP, do not retry blind."; exit 1; }
grep -q 'ci-smp-gate: PASS' work/oct5-as-r9/guest-smp.log \
  || { echo "   exit 0 but NO PASS line -- the gate never reached its verdict"; exit 1; }
# ENUMERATE the rows, never count them: a count the remaining rows satisfy
# cannot see a missing row. Format is `  PASS  <label>` (ci-smp-gate.sh:181).
for lbl in default-smp1 default-smp4 default-smp8 ubsan-smp4 ubsan-smp8; do
  grep -qE "^  PASS  $lbl *$" work/oct5-as-r9/guest-smp.log \
    && echo "   row PASS: $lbl" \
    || { echo "   ROW MISSING OR RED: $lbl -- the matrix did not run in full"; exit 1; }
done
# FIVE PASS ROWS ARE THE SCRIPT'S ACCEPTANCE, NOT 50 CLEAN BOOTS (astra, 0161
# note 8). smp-multiboot's own verdict is `corrupt==0 && extkill==0 &&
# other==0` (smp-multiboot.sh:347), so a row can PASS with nonzero TIMING or
# INJECT-MISS boots. For a race fix that is the signal, not the noise: a
# "timing (benign host-fragility)" boot is a boot that did not come up clean,
# and host-fragility is the non-explanation this project forbids me to accept.
# Every boot carries exactly one classification, so pass == N is precisely
# "all six other categories are zero" -- one check, no enumeration gap.
echo "-- per-label tallies, every category (the row verdict is not enough):"
SMP_N="${SMP_GATE_N:-10}"
unclean=0
for lbl in default-smp1 default-smp4 default-smp8 ubsan-smp4 ubsan-smp8; do
  line=$(grep -E "^== $lbl: [0-9]+ PASS / " work/oct5-as-r9/guest-smp.log | tail -1)
  [ -n "$line" ] || { echo "   NO TALLY LINE for $lbl -- it never reported; STOP"; exit 1; }
  echo "   $line"
  p=$(echo "$line" | sed -E 's/^== [^:]+: ([0-9]+) PASS .*/\1/')
  [ "$p" = "$SMP_N" ] || { echo "     ^^ only $p of $SMP_N boots CLEAN"; unclean=1; }
done
if [ "$unclean" != 0 ]; then
  echo "   NOT A CLEAN QUALIFICATION: a label passed its row verdict with fewer"
  echo "   than $SMP_N clean boots. timing/inject-miss are tolerated by the gate,"
  echo "   not by me. Diagnose them -- build/multiboot-fails/ has each log."
  exit 1
fi
echo "   all five labels: $SMP_N/$SMP_N CLEAN boots"

# PER-BOOT D7 WITNESSES. "The 5x10 matrix exercises D7 fifty times" is an
# ASSUMPTION, and astra refused it (0161 note 17): the D7 close condition is
# actual per-boot PASS witnesses in the RETAINED logs, never an inference from
# five green rows. The retention has its own DENOMINATOR CONTROL, and the
# count is trustworthy because retention itself now fails the label loudly on
# any archive/copy error and stamps each kept file as being from THIS run
# (astra, 0161 note 19 -- a swallowed mv left stale evidence satisfying the
# count). A label
# with fewer than N kept logs means the EVIDENCE is missing, and zero D7 reds
# read off missing evidence is the gauge-reading-zero dodge, so it REFUSES.
KEEP=build/multiboot-logs
echo "-- per-boot D7 witnesses in the retained logs ($KEEP):"
d7_bad=0; d7_att_total=0; d7_pass_total=0
for lbl in default-smp1 default-smp4 default-smp8 ubsan-smp4 ubsan-smp8; do
  n_logs=$(find "$KEEP" -maxdepth 1 -name "$lbl-*.log" ! -name '*-harness.log' 2>/dev/null | wc -l | tr -d ' ')
  if [ "$n_logs" != "$SMP_N" ]; then
    echo "   REFUSING on $lbl: $n_logs retained boot logs, expected $SMP_N."
    echo "   Per-boot evidence missing, so a clean D7 reading here would prove"
    echo "   nothing. Check SMP_KEEP_LOGS reached tools/smp-multiboot.sh."
    exit 1
  fi
  att=0; pas=0; red=0
  for f in "$KEEP/$lbl-"*.log; do
    case "$f" in *-harness.log) continue ;; esac
    if [ ! -r "$f" ]; then
      echo "   REFUSING on $lbl: $f is not readable -- a search that cannot read"
      echo "   its oracle reports ABSENT, which is the dodge, not a measurement."
      exit 1
    fi
    f_m=$(stat -f %m "$f" 2>/dev/null || echo 0)
    if [ "$f_m" -lt "$GATESTAMP_M" ]; then
      echo "   REFUSING on $lbl: $f predates this gate run ($f_m < $GATESTAMP_M)"
      echo "   -- it is an EARLIER run's evidence under a current boot's name."
      exit 1
    fi
    grep -q 'D7 three distinct login sessions simultaneously ready' "$f" && att=$((att+1)) || true
    grep -q 'D7 overlapping login probe PASS' "$f" && pas=$((pas+1)) || true
    grep -q 'D7 overlapping login probe FAILED' "$f" && red=$((red+1)) || true
  done
  echo "   $lbl: D7 ladder reached in $att/$n_logs boots, probe PASS in $pas, FAILED in $red"
  [ "$red" = 0 ] || d7_bad=1
  [ "$att" = "$pas" ] || d7_bad=1
  d7_att_total=$((d7_att_total + att)); d7_pass_total=$((d7_pass_total + pas))
done
if [ "$d7_bad" != 0 ]; then
  echo "   D7 IS NOT CLEAN ACROSS THE MATRIX: a boot reached the overlapping-login"
  echo "   ladder and did not report PASS. That is the 10-06 red's own class, on a"
  echo "   build whose Stratum input is pinned -- STOP and diagnose, do not retry."
  exit 1
fi
echo "   D7 TOTALS: ladder reached in $d7_att_total boots, probe PASS in $d7_pass_total"
if [ "$d7_att_total" = 0 ]; then
  echo "   *** D7 COVERAGE NOT MET: no boot in this matrix reached the ladder. The"
  echo "   *** SMP verdict above stands, but the D7 queue entry CANNOT be closed"
  echo "   *** on this run -- it needs boots that actually exercise the overlap."
fi

# Stage 6 -- THE SECOND AXIS, and for a race fix it is not optional padding.
# Everything above runs on one memory model (Apple M2 under HVF). AS-R9 is an
# SMP race, so a green on one silicon is one reading; thyla-pi is the only
# non-Apple ARM64 in the loop (4x Cortex-A72, real KVM) and #214 was closed on
# it. Two causes can share one reading -- only a second axis separates them.
#
# pi is FREE per `yip resources` and has NO reservation protocol, but keep QEMU
# single-flight: ssh thyla-pi 'ps -eo pid,args | grep "[q]emu-system"'.
#
# PAIRING TRAP (docs/agent/THYLA-PI.md): pool.img and the key-bearing
# ramfs.cpio ship TOGETHER or the guest gets STM_EBADTAG -> stratumd rc=-201 ->
# EXTINCTION: joey exited non-zero. A rebuilt ramfs carries a fresh key that no
# longer matches an already-synced pool. So sync BOTH from this tree, and do
# NOT re-bake either one on the far side.
# 4 GB RAM: ONE 2048 MiB guest at a time.
if [ "${PI_AXIS:-1}" = 1 ]; then
  # I CLAIMED THE UBSAN BUILD CLOBBERS THE DEFAULT ELF. THAT WAS WRONG, and
  # astra caught it (0161 note 8) against build.sh:231-232: a sanitizer build
  # goes to build/kernel-${san} -- build/kernel-undefined -- for exactly the
  # stated reason that it must not clobber the production build. So
  # build/kernel/thylacine.elf is STILL the default kernel after the gate, and
  # the rebuild I had put here was not merely unnecessary: it re-mints pool.img
  # with a fresh key (build.sh:3598), spending lease minutes and ~600M of disk
  # to restore something that was never disturbed.
  #
  # What IS shared and therefore last-writer-wins: build/ramfs.cpio and
  # build/fixtures/ are NOT sanitizer-scoped, so the pair on disk is whatever
  # the gate's final bake produced. That pair is internally consistent, and the
  # kernel carries no key, so sync it as found -- but NAME what is synced
  # rather than assume it.
  echo "-- kernel flavours present (the synced axis is named, not assumed):"
  for k in build/kernel/thylacine.elf build/kernel-undefined/thylacine.elf; do
    [ -f "$k" ] && echo "   $(shasum -a 256 "$k" | cut -c1-16)  $k"
  done
  # A yip PI LEASE IS REQUIRED, and FREE IS NOT REACHABILITY (astra, note 8;
  # the operator last reported pi offline). My earlier claim that pi has no
  # reservation protocol is stale -- `yip resources` lists it. Acquire the
  # lease yourself and release it in a finally; this script will not take a
  # lease on your behalf, because a script with many exit paths leaks one.
  if [ "${PI_LEASE_OK:-0}" != 1 ]; then
    echo "   REFUSING the pi sync: set PI_LEASE_OK=1 only while you HOLD the yip"
    echo "   pi lease. PI_AXIS=0 skips the second axis deliberately and says so."
    exit 6
  fi
  ssh -o ConnectTimeout=10 -o BatchMode=yes thyla-pi true 2>/dev/null \
    || { echo "   REFUSING: thyla-pi unreachable. FREE in yip is a lease state,"
         echo "   not a reachability measurement."; exit 6; }
  provenance "stage-6 pre-sync (no rebuild -- flavours recorded as found)"
  echo "-- second axis: syncing this tree's kernel + PAIRED pool/ramfs to thyla-pi"
  WARP_HOST=thyla-pi tools/warp-host.sh sync
  echo "-- then run the SMP boots there under real KVM, A72 weak memory"
fi

provenance "post-ci-smp-gate (the qualifying verdict)"
echo "DONE. Report SHAs + evidence to astra on yip 0161 BEFORE any integration."
echo "OWED THE MOMENT THE CLONE COMPLETED: the cache-copy acknowledgement to astra"
echo "on 0169 -- she is holding her build/ unchanged until she gets it."
echo "Report BOTH axes separately: M2/HVF and A72/KVM. A race fix green on one"
echo "memory model is one reading, not a qualification."
