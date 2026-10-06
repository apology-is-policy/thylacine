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
# delta: two kernel flavours ~70M, a pool+ramfs re-bake ~200M, cargo deltas ~0
# because no userspace source changed) + margin. Measured 7.4 GiB free at
# 21:0xZ while main's CI legs ran, down from 9.47 at 20:49Z -- the volume is
# TIGHTENING, so re-read it at every stage rather than once at the start.
FLOOR_GB=${FLOOR_GB:-8}

free_gb() { df -g . | awk 'NR==2 {print $4}'; }

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
    for f in build/.config build/kernel/thylacine.elf build/ramfs.cpio build/fixtures/pool.img; do
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
    "$JAVA" -cp "$JAR" tlc2.TLC 2>&1 | grep -m1 'TLC2 Version' || true
    SPECS_RAN=1
  fi

  # CLEAN-cfg runs are SUSPENDED (SPEC-POLICY, since 2026-05-21). The binding
  # obligation is the BUGGY cfgs: each must STILL produce its counterexample.
  # A buggy cfg that now PASSES is a FINDING, not a convenience -- it means the
  # spec stopped constraining the thing it was written to catch, which is
  # exactly how a repair can silently void its own proof.
  if [ "${SPECS_RAN:-0}" = 1 ]; then
  cd specs
  # burrow.tla -- I-7, the dual-refcount lifecycle whose {0,0} decision this
  # repair RELOCATED into the settled drops. Each must violate NoUseAfterFree.
  for c in burrow_buggy_free_on_close burrow_buggy_free_on_unmap burrow_buggy_never_free; do
    echo "-- $c (expect: NoUseAfterFree VIOLATED)"
    # TLC EXITS 12 ON A VIOLATION -- the EXPECTED result here. Under `set -e`
    # that aborted the stage silently after printing only the header, so capture
    # with || true and judge by CONTENT.
    out=$("$JAVA" -cp "$JAR" tlc2.TLC -workers auto -deadlock -config "$c.cfg" burrow.tla 2>&1 || true)
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

# Stage 1 -- the warm cache. ONLY with astra's ruling on yip 0169; her tree is
# at our shared base 5ff62b788 and config-equivalent to --config ci. Drop every
# CMake tree whose cache names HER path, per reference-ci-image-worktree-recipe.
if [ "${CLONE_APPROVED:-0}" = 1 ]; then
  [ -d build ] || cp -Rc ../thylacine-astra/build build
  rm -rf build/kernel build/usr build/pouch/stratumd-cmake build/kernel-undefined build/host-stratum
  rsync -a --ignore-existing ../thylacine-astra/third_party/rust/ third_party/rust/ 2>/dev/null || true

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
  git -C ../thylacine-astra rev-parse HEAD > /tmp/astra-head.txt
  echo "   her HEAD: $(cat /tmp/astra-head.txt)  my base: $(git rev-parse 5ff62b788)"
  floor post-clone
else
  echo "CLONE_APPROVED!=1 -- not cloning astra's build/. Her artifacts, her ruling (yip 0169)."
  exit 2
fi

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
# it only proves the sha is spellable. The `merge-base --is-ancestor` below is
# the load-bearing half; verified both ways against both trees.
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
# A DIRTY external source defeats the pin. The ancestor check proves the COMMIT
# is in history; it says nothing about what is actually on disk, and build.sh
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
tools/build.sh kernel --config ci
floor post-build
provenance "post-build (my kernel, paired images)"

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
tools/test.sh 2>&1 | tee work/oct5-as-r9/guest-test.log
# AN ELF NAME IS NOT AN EXECUTION WITNESS (astra, 0169 turn 4). The stage-3 grep
# proves the tests are COMPILED IN; only the boot log proves they RAN. The suite
# prints "    [test] <name> ... " per test, so require a PASS record for each of
# the four BY NAME -- that is the witness, and the total alone is not.
echo "-- EXECUTION witness: each of the four must have its own PASS record:"
for t in settled_drop_retains_nonfinal_charge settled_drop_exact_payer \
         settled_mapping_drop_defers_free unmap_failure_leaves_mapping_attached; do
  if grep -E "\[test\] burrow\.$t \.\.\..*(PASS|ok)" work/oct5-as-r9/guest-test.log >/dev/null; then
    echo "   RAN+PASSED: burrow.$t"
  else
    echo "   NO PASS RECORD: burrow.$t -- compiled in is not run; show its line:"
    grep -F "burrow.$t" work/oct5-as-r9/guest-test.log || echo "     (absent from the log entirely)"
    exit 1
  fi
done
echo "-- suite total must be base+4; a skip is NOT coverage (OPEN-BUGS: 17 ramfs"
echo "   probe tests pass when their initrd file is missing):"
grep -E '  tests: [0-9]+/[0-9]+' work/oct5-as-r9/guest-test.log || true
echo "   [skip] lines (must be 0 on the gate image, which always carries the probe set):"
grep -c '\[skip\]' work/oct5-as-r9/guest-test.log || true

# Stage 5 -- the one that matters. AS-R9 is an SMP race: a single-CPU green
# proves little, and the Oct 1-2 single-boot waiver has expired.
floor pre-smp
tools/ci-smp-gate.sh 2>&1 | tee work/oct5-as-r9/guest-smp.log

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
