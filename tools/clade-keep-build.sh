#!/usr/bin/env bash
# Drive the Clade device toolchain on the PERMANENT GCP builder (`thyla-keep`).
#
# WHY A SECOND BUILDER TOOL. `clade-gcp-build.sh` creates a disposable spot VM,
# builds, fetches, and DELETES it -- correct when the artifact you want is the
# multicall binary. It is the wrong shape once the artifact you want is the
# BUILD TREE: CL-7 (Mesa/llvmpipe) links against the cross-LLVM's 207 static
# libraries, and re-creating ~45 GiB of LLVM from scratch for every iteration is
# hours per attempt. So this tool drives a machine that is STOPPED, never
# deleted, whose /build disk survives -- turning a 24-minute cold build into an
# incremental ninja.
#
# STOP, NEVER DELETE. There is deliberately no `down`/`delete` verb here, and
# the instance name sits OUTSIDE `clade-gcp-build.sh`'s `clade-builder-*`
# prefix so that tool's teardown provably cannot reach it. Stopped, the machine
# costs disk only; running, it also costs compute -- so `stop` when idle.
#
# NO CONFIG IS DUPLICATED. Stage 1's recipe is `tools/clade-stage1.sh` (shared
# verbatim with clade-gcp-build.sh -- one original, two callers). Stage 2 runs
# the real `tools/build.sh` targets with the working-copy build.sh overlaid.
# Stage 3 is the only recipe original here, and it is a target list, not a
# configuration.
#
# EVERY STAGE ASSERTS ON ARTIFACTS, NOT EXIT STATUS. `build.sh` skips silently
# and returns 0 when it cannot find the fork toolchain, so `set -euo pipefail`
# cannot catch a run that built nothing -- that exact green-but-empty stage 2 is
# what motivated the assertions below.
#
# Usage:
#   tools/clade-keep-build.sh start          # boot the (stopped) builder
#   tools/clade-keep-build.sh sync           # push patches + build.sh + stage1
#   tools/clade-keep-build.sh stage1         # the fork host toolchain
#   tools/clade-keep-build.sh stage2         # sysroot + libcxx + clade
#   tools/clade-keep-build.sh stage3         # + the JIT/ORC static libraries
#   tools/clade-keep-build.sh all            # sync -> 1 -> 2 -> 3 (detached)
#   tools/clade-keep-build.sh log            # tail the remote log
#   tools/clade-keep-build.sh inventory      # what is actually built up there
#   tools/clade-keep-build.sh fetch          # pull bin/llvm + bin/clangd down
#   tools/clade-keep-build.sh stop           # halt (keeps the disks + the tree)
#   tools/clade-keep-build.sh status         # running? costing compute?
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PROJECT="${CLADE_GCP_PROJECT:-wired-epsilon-337013}"
ZONE="${CLADE_KEEP_ZONE:-europe-west4-a}"
# The name is load-bearing: it must NOT match clade-gcp-build.sh's
# NAME_PREFIX="clade-builder", whose teardown deletes anything it does match.
INSTANCE="${CLADE_KEEP_INSTANCE:-thyla-keep}"

# Rooted on the persistent disk, not $HOME -- $HOME lives on the boot disk.
KEEP_ROOT="${CLADE_KEEP_ROOT:-/build}"
FORK_SRC="$KEEP_ROOT/src/llvm-thylacine"
FORK_BUILD="$KEEP_ROOT/out/stage1"
THYLA_SRC="$KEEP_ROOT/src/thylacine"

LLVM_TAG="${CLADE_LLVM_TAG:-llvmorg-22.1.8}"
FORK_DIR="${LLVMFORK:-$HOME/projects/llvm-thylacine}"

say() { printf '==> %s\n' "$*"; }
die() { printf 'clade-keep: %s\n' "$*" >&2; exit 1; }

gc() { gcloud --project="$PROJECT" "$@"; }
# --strict-host-key-checking=no: the instance keeps its host key across a
# stop/start, but a rebuild of the boot disk would change it.
rsh() { gc compute ssh "$INSTANCE" --zone="$ZONE" --strict-host-key-checking=no "$@"; }

assert_not_disposable() {
    # A name typo that landed inside the disposable prefix would put this
    # machine in reach of a teardown that deletes. Fail closed.
    case "$INSTANCE" in
        clade-builder-*) die "instance '$INSTANCE' matches clade-gcp-build.sh's disposable prefix -- refusing" ;;
    esac
}

instance_status() {
    gc compute instances describe "$INSTANCE" --zone="$ZONE" \
        --format='value(status)' 2>/dev/null || echo ABSENT
}

require_running() {
    local s; s="$(instance_status)"
    case "$s" in
        RUNNING) ;;
        ABSENT)  die "instance '$INSTANCE' does not exist -- it was never created, or (worse) deleted" ;;
        *)       die "instance '$INSTANCE' is $s -- run 'start' first" ;;
    esac
}

cmd_start() {
    assert_not_disposable
    local s; s="$(instance_status)"
    case "$s" in
        RUNNING) say "already running"; return 0 ;;
        ABSENT)  die "instance '$INSTANCE' does not exist; this tool does not create it (see memory/reference_gcp_permanent_builder.md)" ;;
    esac
    say "starting $INSTANCE"
    gc compute instances start "$INSTANCE" --zone="$ZONE"
    say "waiting for ssh"
    local i
    for i in $(seq 1 40); do
        if rsh --command=true -- -o ConnectTimeout=10 >/dev/null 2>&1; then
            say "ssh up"
            # The persistent disk mounts from fstab with `nofail`, so a missing
            # disk boots the machine WITHOUT /build rather than wedging it --
            # which means a green boot is not evidence the tree is there.
            rsh --command="mountpoint -q $KEEP_ROOT" >/dev/null 2>&1 \
                || die "$KEEP_ROOT is not mounted (fstab uses nofail, so boot succeeded anyway) -- check the disk"
            say "$KEEP_ROOT mounted"
            return 0
        fi
        sleep 15
    done
    die "ssh never came up"
}

cmd_stop() {
    assert_not_disposable
    local s; s="$(instance_status)"
    [[ "$s" == "RUNNING" ]] || { say "not running ($s) -- nothing to stop"; return 0; }
    say "stopping $INSTANCE (disks and the build tree are kept)"
    gc compute instances stop "$INSTANCE" --zone="$ZONE"
    say "stopped -- compute billing ends; disks continue"
}

cmd_sync() {
    assert_not_disposable; require_running
    local payload; payload="$(mktemp -d)"
    trap 'rm -rf "$payload"' RETURN

    say "packing the fork patch series + working-copy build.sh + the stage recipes"
    # The fork commits only (~50 KB). The VM already holds the upstream tree.
    ( cd "$FORK_DIR" && git format-patch --output-directory "$payload/patches" "$LLVM_TAG..HEAD" ) >/dev/null
    local n; n="$(ls "$payload/patches" | wc -l | tr -d ' ')"
    [[ "$n" -gt 0 ]] || die "no fork patches produced from $LLVM_TAG..HEAD"
    say "  $n fork patches"

    # The WORKING COPY of build.sh -- it may carry uncommitted recipe edits, and
    # running the exact file under review is the whole anti-drift story.
    cp "$REPO_ROOT/tools/build.sh"        "$payload/build.sh"
    cp "$REPO_ROOT/tools/clade-stage1.sh" "$payload/clade-stage1.sh"
    # CL-7a: the Mesa cross-build inputs. The shim llvm-config is not a
    # convenience -- meson RUNS it during configure, so it is as much a build
    # input on this machine as build.sh is.
    cp "$REPO_ROOT/tools/clade-llvm-config-cross.sh" "$payload/clade-llvm-config-cross.sh"
    cp "$REPO_ROOT/tools/clade-mesa-cross.sh"        "$payload/clade-mesa-cross.sh"
    write_remote_driver > "$payload/keep-stage.sh"

    tar -C "$payload" -czf "$payload/payload.tgz" patches build.sh clade-stage1.sh \
        clade-llvm-config-cross.sh clade-mesa-cross.sh keep-stage.sh
    gc compute scp "$payload/payload.tgz" "$INSTANCE:~/payload.tgz" \
        --zone="$ZONE" --strict-host-key-checking=no >/dev/null

    rsh --command="set -e; cd ~; rm -rf payload; mkdir payload
        tar -xzf payload.tgz -C payload
        chmod +x payload/keep-stage.sh payload/clade-stage1.sh payload/build.sh
        mkdir -p $THYLA_SRC/tools
        cp payload/build.sh                    $THYLA_SRC/tools/build.sh
        cp payload/clade-stage1.sh             $THYLA_SRC/tools/clade-stage1.sh
        cp payload/clade-llvm-config-cross.sh  $THYLA_SRC/tools/clade-llvm-config-cross.sh
        cp payload/clade-mesa-cross.sh         $THYLA_SRC/tools/clade-mesa-cross.sh
        chmod +x $THYLA_SRC/tools/build.sh $THYLA_SRC/tools/clade-stage1.sh \
                 $THYLA_SRC/tools/clade-llvm-config-cross.sh $THYLA_SRC/tools/clade-mesa-cross.sh
        echo synced"
    say "synced"
}

# Run a stage detached, so a dropped ssh cannot kill an hour of build.
#
# Two things are required to detach and fixing only the first leaves the hang
# exactly as it was: (1) `< /dev/null`, or the child holds the ssh channel's
# stdin and sshd keeps the session open; (2) `;` between the setup steps, NOT
# `&&` -- in `A && B & echo`, the `&` binds to the whole `&&` LIST, so the
# entire chain becomes one backgrounded subshell that runs nohup in ITS
# foreground and waits, with the channel still attached. The symptom of getting
# this wrong is deceptive: the build runs fine and only the launcher looks hung.
run_stage() {
    local stage="$1"
    assert_not_disposable; require_running
    say "launching stage '$stage' (detached; poll with 'log')"
    rsh --command="cd ~; rm -f ~/keep.status
        nohup payload/keep-stage.sh $stage < /dev/null > ~/keep.log 2>&1 &
        echo \"launched pid \$!\""
}

cmd_log() {
    require_running
    rsh --command='tail -n 40 ~/keep.log 2>/dev/null || echo "(no log yet)"
        echo "---"; cat ~/keep.status 2>/dev/null || echo "STATUS: running"'
}

cmd_inventory() {
    require_running
    # What is ACTUALLY built, read off the machine -- not inferred from a log.
    # The staged bin/ holds executables only, so counting there and concluding
    # "there is no linkable LLVM" is exactly the mistake #108 corrected.
    rsh --command="
        echo '=== fork host toolchain ($FORK_BUILD) ==='
        ls $FORK_BUILD/bin/clang $FORK_BUILD/bin/lld 2>/dev/null || echo '  (absent)'
        echo \"=== \\\$fork/build resolves to ===\"
        readlink -f $FORK_SRC/build 2>/dev/null || echo '  (no symlink)'
        echo '=== cross-LLVM static libraries ==='
        L=$THYLA_SRC/build/clade/llvm-build
        if [ -d \"\$L/lib\" ]; then
            echo \"  total .a: \$(ls \$L/lib/*.a 2>/dev/null | wc -l)\"
            echo '  JIT/ORC:'
            # '\.a\$' matters: without it the ExecutionEngine *directory* lists
            # as though it were a library.
            ls \$L/lib/ 2>/dev/null | grep -iE '(orc|executionengine|jitlink|mcjit|runtimedyld).*\.a\$' | sed 's/^/    /'
        else
            echo '  (no clade build tree)'
        fi
        echo '=== staged binaries ==='
        if [ -d \"\$L/bin\" ]; then
            echo \"  total: \$(ls \$L/bin/ 2>/dev/null | wc -l)\"
            # Only the ones the arc actually consumes. The rest are collateral
            # from an earlier stage 3 whose empty target list made ninja build
            # every target in the tree.
            for b in llvm clang clang++ clangd lld; do
                [ -e \"\$L/bin/\$b\" ] && ls -la \"\$L/bin/\$b\" | sed 's/^/    /'
            done
        else echo '  (none)'; fi
        echo '=== disk ==='
        df -h $KEEP_ROOT | tail -1"
}

cmd_fetch() {
    require_running
    local dest="$REPO_ROOT/build/clade/llvm-build"
    local st
    st="$(rsh --command='cat ~/keep.status 2>/dev/null || true' 2>/dev/null || true)"
    case "$st" in
        *OK*) ;;
        *) die "last stage is not OK (status: ${st:-unknown}) -- refusing to fetch a partial tree" ;;
    esac
    mkdir -p "$dest/bin"
    say "fetching into $dest"
    rsh --command="cd $THYLA_SRC/build/clade/llvm-build && tar -czf ~/artifacts.tgz bin/llvm bin/clangd lib/clang" >/dev/null
    gc compute scp "$INSTANCE:~/artifacts.tgz" "$REPO_ROOT/build/clade/artifacts.tgz" \
        --zone="$ZONE" --strict-host-key-checking=no >/dev/null
    tar -xzf "$REPO_ROOT/build/clade/artifacts.tgz" -C "$dest"
    rm -f "$REPO_ROOT/build/clade/artifacts.tgz"
    say "got: $(ls -la "$dest/bin/llvm" "$dest/bin/clangd" 2>/dev/null | awk '{print $NF" "$5}' | tr '\n' ' ')"
}

cmd_status() {
    say "instance $INSTANCE in $PROJECT/$ZONE: $(instance_status)"
    gc compute instances list --filter="name=$INSTANCE" \
        --format="table(name,zone,machineType.basename(),status)" || true
    gc compute disks list --filter="users~$INSTANCE OR name~^thyla" \
        --format="table(name,sizeGb,type.basename(),status)" || true
}

# The script that runs ON the builder. Emitted rather than kept remotely so the
# reviewed copy in git is the one that executes.
write_remote_driver() {
cat <<REMOTE
#!/usr/bin/env bash
set -euo pipefail
KEEP_ROOT=$KEEP_ROOT
FORK_SRC=$FORK_SRC
FORK_BUILD=$FORK_BUILD
THYLA_SRC=$THYLA_SRC
LLVM_TAG=$LLVM_TAG
REMOTE
cat <<'REMOTE'
JOBS="$(nproc)"
status() { echo "STATUS: $*" > ~/keep.status; }
phase()  { echo; echo "######## $* ########"; date -u +%FT%TZ; }

# -E (errtrace) is load-bearing, not decoration: bash does NOT inherit an ERR
# trap into shell functions, and every stage below IS a function. Without it a
# mid-stage failure exits silently, leaving keep.status at "running" -- which a
# poller cannot distinguish from a live build, so it waits for a terminal state
# that will never arrive. Observed exactly that.
set -eEuo pipefail
trap 'status "FAILED (see keep.log)"' ERR
# Belt and braces for the same hazard: any exit that did not reach a TERMINAL
# state must say so, whatever path it took -- a trap-less `exit 1` (the stage
# asserts use exactly that) or a signal. It tests for OK-or-FAILED, not just OK,
# so it fills in a verdict rather than overwriting the ERR trap's better-attributed
# one; `$?` here would read 0 anyway, since the ERR trap's own last command
# succeeded.
trap 'rc=$?; grep -qE "STATUS: (OK|FAILED)" ~/keep.status 2>/dev/null || status "FAILED rc=$rc (see keep.log)"' EXIT
status "running"

mountpoint -q "$KEEP_ROOT" || { echo "FATAL: $KEEP_ROOT not mounted"; exit 1; }

do_deps() {
  phase "deps"
  export DEBIAN_FRONTEND=noninteractive
  sudo apt-get update -qq
  sudo apt-get install -y -qq clang lld cmake ninja-build git python3 zlib1g-dev >/dev/null
}

do_stage1() {
  do_deps
  phase "stage 1: the fork HOST toolchain"
  mkdir -p "$(dirname "$FORK_SRC")" "$(dirname "$FORK_BUILD")"
  if [[ ! -d "$FORK_SRC/.git" ]]; then
    git clone --depth 1 --branch "$LLVM_TAG" \
        https://github.com/llvm/llvm-project.git "$FORK_SRC"
  fi
  # The shared original -- see tools/clade-stage1.sh. It also establishes the
  # $FORK_SRC/build -> $FORK_BUILD symlink that build.sh's hardcoded
  # "$fork/build/bin/..." lookups require.
  "$THYLA_SRC/tools/clade-stage1.sh" \
      --src "$FORK_SRC" --build "$FORK_BUILD" --jobs "$JOBS" \
      --patches ~/payload/patches
}

do_stage2() {
  phase "stage 2: sysroot + libcxx + clade"
  cd "$THYLA_SRC"
  export LLVMFORK="$FORK_SRC"
  # ONE toolchain version end to end: the fork's own LLVM supplies clang AND
  # the binutils AND ld.lld. This path goes through the symlink on purpose --
  # it must be the SAME string build.sh derives from LLVMFORK, because a
  # divergence there is what made the first stage 2 skip everything silently.
  export LLVM_PREFIX="$FORK_SRC/build"
  export LLD_PREFIX="$FORK_SRC/build"
  # build_clade sizes -j from `sysctl -n hw.memsize`, which does not exist on
  # Linux; its fallback assumes 8 GiB and would give -j2 on a 125 GiB machine.
  export CLADE_JOBS="$JOBS"
  export PATH="$HOME/.cargo/bin:$PATH"

  # build.sh's "fork clang not built -- skipping" path RETURNS 0. Assert the
  # precondition here rather than trusting a green exit.
  [[ -x "$LLVM_PREFIX/bin/clang" ]] \
      || { echo "PRECONDITION FAILED: no fork clang at $LLVM_PREFIX/bin/clang (run stage1)"; exit 1; }

  tools/build.sh sysroot
  [[ -d build/sysroot/include ]] || { echo "ASSERT FAILED: sysroot produced no headers"; exit 1; }

  tools/build.sh libcxx
  [[ -d build/sysroot/include/c++ ]] || { echo "ASSERT FAILED: libcxx produced no headers"; exit 1; }

  tools/build.sh clade
  [[ -x build/clade/llvm-build/bin/clang ]] || { echo "ASSERT FAILED: clade produced no clang"; exit 1; }

  phase "stage 2 verify"
  # `| head -6` here would be the same SIGPIPE-under-pipefail race as stage 3's
  # verify; sed reads to EOF, so the writer never sees a closed pipe.
  "$LLVM_PREFIX/bin/llvm-readelf" -h build/clade/llvm-build/bin/clang | sed -n '1,6p'
}

# Stage 3 (#108): the cross-LLVM already builds 127+ static libraries -- clang
# and lld simply never needed a JIT, so the ORC/ExecutionEngine slice was never
# requested. Mesa's gallivm links exactly that slice.
#
# The original of this stage passed a DETECTED target list to ninja, and when
# detection returned nothing `ninja $EMPTY` silently became `ninja` -- build
# EVERYTHING (~1400 edges), while the verify loop iterated zero times and
# reported success. An unquoted empty variable WIDENED the build and emptied the
# check. Hence: an explicit list, a required/optional split, and a hard refusal
# to invoke ninja with nothing to build.
do_stage3() {
  local L="$THYLA_SRC/build/clade/llvm-build"
  local readelf="$FORK_SRC/build/bin/llvm-readelf"
  [[ -f "$L/build.ninja" ]] || { echo "ASSERT FAILED: no clade build tree at $L (run stage2)"; exit 1; }
  [[ -x "$readelf" ]] || { echo "ASSERT FAILED: no llvm-readelf at $readelf (run stage1)"; exit 1; }

  # Required: gallivm's ORC path cannot link without these.
  #
  # LLVMMCJIT is REQUIRED, not optional, and that is counter-intuitive enough to
  # spell out: Mesa's meson names 'mcjit' in `llvm_modules` UNCONDITIONALLY
  # (meson.build:1877 at 26.1.6), and the `llvm-orcjit` option only raises the
  # minimum LLVM version and sets -DGALLIVM_USE_ORCJIT. So an ORC-only Mesa
  # still needs the MCJIT component LINKABLE, even though it will not use it.
  # Had MCJIT stayed in the optional list, a future LLVM that drops it would
  # print "note: optional absent -- skipping", pass stage 3, and only fail much
  # later at Mesa configure -- the same failure-reports-success shape as the
  # empty-ninja bug above.
  local required="LLVMOrcJIT LLVMExecutionEngine LLVMJITLink LLVMOrcShared LLVMOrcTargetProcess LLVMRuntimeDyld LLVMMCJIT LLVMInterpreter"
  # Optional: genuinely version-dependent, and nothing links against it.
  local optional="LLVMOrcDebugging"

  phase "stage 3: JIT/ORC libraries -j$JOBS"
  cd "$L"
  local known; known="$(ninja -t targets all | cut -d: -f1)"
  local want=""
  local t
  for t in $required; do
    grep -qx "lib/lib${t}.a" <<< "$known" \
      || { echo "ASSERT FAILED: required target lib/lib${t}.a is unknown to this LLVM configuration"; exit 1; }
    want="$want $t"
  done
  for t in $optional; do
    if grep -qx "lib/lib${t}.a" <<< "$known"; then want="$want $t"
      else echo "  note: optional $t absent in this LLVM version -- skipping"; fi
  done
  # The load-bearing guard: never hand ninja an empty list.
  [[ -n "${want// /}" ]] || { echo "ASSERT FAILED: empty target list -- refusing to invoke ninja (it would build everything)"; exit 1; }
  echo "building:$want"
  ninja -j"$JOBS" $want

  phase "stage 3 verify: present AND AArch64"
  local fail=0 f m hdr
  for t in $want; do
    f="$L/lib/lib${t}.a"
    if [[ ! -f "$f" ]]; then echo "  ASSERT FAILED: $t missing"; fail=1; continue; fi
    # Capture the header dump FIRST, then filter a here-string. Piping readelf
    # straight into an early-exiting reader (`grep -m1`, `head`) is a race under
    # `pipefail`: on a multi-member archive readelf has megabytes left to write
    # when the reader exits, takes SIGPIPE, and the pipeline yields 141 -- which
    # `set -e` turns into a stage abort even though the value was extracted
    # correctly. It reproduces intermittently (output to a file loses the race
    # that output to a pty wins), so this is a race to remove, not to survive.
    hdr="$("$readelf" -h "$f" 2>/dev/null || true)"
    m="$(awk '/[Mm]achine:/ {print $2; exit}' <<< "$hdr")"
    [[ "$m" == "AArch64" ]] || { echo "  ASSERT FAILED: $t is '$m', not AArch64"; fail=1; continue; }
    printf '  %-24s %8s KB  %s\n' "$t" "$(( $(stat -c%s "$f") / 1024 ))" "$m"
  done
  [[ "$fail" -eq 0 ]] || { echo "STAGE 3 FAILED"; exit 1; }
  echo "total LLVM .a libs: $(ls "$L"/lib/*.a | wc -l)"

  # Complete the set Mesa will actually LINK -- WITHOUT executing the cross tree's
  # own llvm-config.
  #
  # NEVER RUN $L/bin/llvm-config ON THE BUILDER. Measured (CL-7a): it is an
  # aarch64-thylacine static binary, and on this Linux host it does not fail --
  # it SPINS. Two instances burned 1343s and 348s of CPU in state R before being
  # killed, with the stage reporting "running" the whole time. The previous
  # version of this stage BUILT that binary and then ran `--shared-mode` on it,
  # which is exactly how it hung. It had never executed before today: the cross
  # tree carried no bin/llvm-config, so every prior run took the early-return
  # below and the stage was recorded as working on the strength of a path that
  # never ran.
  #
  # The shim (tools/clade-llvm-config-cross.sh) answers about this tree using the
  # HOST llvm-config for the component graph plus the tree's own CMakeCache for
  # facts, so it runs on the builder by construction. Its --libs assert also asks
  # a TIGHTER question than `--shared-mode` did: which archives are missing for
  # the modules Mesa actually requests, rather than for all ~130 components. That
  # builds what is needed instead of everything.
  phase "stage 3b: complete the set Mesa links (via the cross llvm-config shim)"
  local shim="$THYLA_SRC/tools/clade-llvm-config-cross.sh"
  if [[ ! -x "$shim" ]]; then
    echo "  ASSERT FAILED: no shim at $shim -- run 'sync' first"; exit 1
  fi
  # Mesa 26.1.6's llvm_modules for a llvmpipe+osmesa+orcjit configure, read off a
  # real cross configure (LLVM-DESIGN 16.20) rather than guessed. 'orcjit' is ours
  # (upstream omits it, and a static ORC link needs it).
  local mesa_modules="bitwriter core engine executionengine instcombine mcdisassembler
                      mcjit native scalaropts transformutils coroutines lto orcjit"
  local shimlog shimerr miss
  shimlog="$(mktemp)"; shimerr="$(mktemp)"
  CLADE_LLVM_TARGET_TREE="$L" CLADE_LLVM_CONFIG_LOG="$shimlog" \
      "$shim" --link-static --libs $mesa_modules >/dev/null 2>"$shimerr" || true
  # The shim's machine-readable contract: one "MISSING ARCHIVES: libFoo.a ..." line.
  #
  # `|| true` on the substitution is load-bearing, not decoration: a filter that
  # matches nothing exits 1, and under `set -e` with `pipefail` that aborts the
  # whole stage -- so "nothing is missing", the SUCCESS case, would read as a
  # stage failure. (It did, first run.)
  miss="$( { sed -n 's/^MISSING ARCHIVES://p' "$shimlog" \
             | tr ' ' '\n' | sed 's/^lib//; s/\.a$//' | sort -u | tr '\n' ' '; } || true )"
  if [[ -n "${miss// /}" ]]; then
    echo "  completing $(wc -w <<< "$miss") absent libraries:$miss"
    ninja -j"$JOBS" $miss
  else
    echo "  every archive Mesa links is already present"
  fi
  if ! CLADE_LLVM_TARGET_TREE="$L" "$shim" --link-static --libs $mesa_modules \
         >/dev/null 2>"$shimerr"; then
    # Show the shim's own words. Discarding them is how a precise error becomes
    # "something went wrong in llvm-config somewhere".
    echo "ASSERT FAILED: the shim still cannot answer --libs. It said:"
    sed 's/^/    /' "$shimerr"
    rm -f "$shimlog" "$shimerr"; exit 1
  fi
  rm -f "$shimlog" "$shimerr"
  echo "  archives Mesa links: $(CLADE_LLVM_TARGET_TREE="$L" "$shim" --link-static --libs $mesa_modules | tr ' ' '\n' | grep -c '^-l')"
  echo "  --has-rtti: $(CLADE_LLVM_TARGET_TREE="$L" "$shim" --has-rtti)  (Mesa requires YES for gallivm's ORC backend)"
}

case "${1:-}" in
  stage1) do_stage1 ;;
  stage2) do_stage2 ;;
  stage3) do_stage3 ;;
  all)    do_stage1; do_stage2; do_stage3 ;;
  *)      echo "keep-stage: usage: $0 {stage1|stage2|stage3|all}"; exit 2 ;;
esac
status "OK"
echo "STAGE OK"
REMOTE
}

assert_not_disposable
case "${1:-}" in
    start)     cmd_start ;;
    stop)      cmd_stop ;;
    sync)      cmd_sync ;;
    stage1)    run_stage stage1 ;;
    stage2)    run_stage stage2 ;;
    stage3)    run_stage stage3 ;;
    all)       cmd_sync; run_stage all ;;
    log)       cmd_log ;;
    inventory) cmd_inventory ;;
    fetch)     cmd_fetch ;;
    status)    cmd_status ;;
    down|delete)
        die "this builder is STOP-not-DELETE by design: /build holds the LLVM tree. Use 'stop'." ;;
    *) die "usage: $0 {start|stop|sync|stage1|stage2|stage3|all|log|inventory|fetch|status}" ;;
esac
