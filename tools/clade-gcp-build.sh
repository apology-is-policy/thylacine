#!/usr/bin/env bash
# Cross-build the Clade device toolchain on a disposable GCP VM.
#
# WHY THIS EXISTS. `tools/build.sh clade` cross-builds a static
# aarch64-thylacine clang+lld multicall (CL-4) and, since CL-6, clangd. On the
# dev box that is impractical: build_clade sizes its job count as
# (host RAM GiB / 3) because the worst LLVM TU peaks ~2.46 GiB RSS, so an
# 8 GiB laptop gets -j2 -- many hours, at real OOM risk, monopolizing the
# machine that also runs the QEMU gates. A 16-vCPU / 64 GiB VM runs -j16 and
# is torn down when it is done.
#
# The build is TWO stages, and the first is not optional: stage 1 builds the
# FORK's own host clang, because a stock clang does not know the
# `aarch64-thylacine` triple -- teaching it that is exactly what CL-3 landed.
# Stage 2 then cross-builds with it.
#
# NO CONFIG IS DUPLICATED HERE. The VM clones this repo and runs the real
# `tools/build.sh` (targets: sysroot, libcxx, clade), with the local working
# copy of build.sh overlaid on top. Hand-copying build_clade's cmake args into
# this script would create two mirrors of one configuration, which is the
# failure mode that has bitten this tree before (the `struct t_stat` mirrors,
# #100): the copy passes its own checks while silently disagreeing with the
# original. The only things this script sets are the env overrides build.sh
# already exposes for a non-Darwin host.
#
# COST DISCIPLINE (standing): spot, smallest machine that fits, zero idle,
# torn down immediately. Every mutating gcloud call is confined to an instance
# whose name starts with $NAME_PREFIX; this script will refuse to delete
# anything else, so it can never reach cora-bbs or treeso-net.
#
# The VM also arms its OWN dead-man halt (see the remote script). Spot VMs have
# no maximum runtime, and every other safeguard here -- this script, a monitor,
# the operator -- depends on a session that can end. `down` is still the normal
# path; the dead-man is what makes forgetting merely wasteful instead of open-
# ended.
#
# Usage:
#   tools/clade-gcp-build.sh all      # up -> run -> fetch -> down (the normal path)
#   tools/clade-gcp-build.sh up       # create the VM only
#   tools/clade-gcp-build.sh run      # launch the build (detached; poll with `log`)
#   tools/clade-gcp-build.sh log      # tail the remote build log
#   tools/clade-gcp-build.sh fetch    # pull the artifacts into build/clade/llvm-build
#   tools/clade-gcp-build.sh down     # delete the VM (ALWAYS run this)
#   tools/clade-gcp-build.sh status   # is anything of ours still alive + costing money?
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PROJECT="${CLADE_GCP_PROJECT:-wired-epsilon-337013}"
ZONE="${CLADE_GCP_ZONE:-europe-west4-a}"
# t2a = Ampere Altra (ARM). 16 vCPU / 64 GiB -> -j16 with 4 GiB per job, which
# clears the 2.46 GiB worst-TU peak with room. The CL-0 census used the same
# shape (docs/LLVM-DESIGN.md section 16).
MACHINE="${CLADE_GCP_MACHINE:-t2a-standard-16}"
# Two Release LLVM trees (host ~15 GiB + cross ~30 GiB) plus sources.
DISK_GB="${CLADE_GCP_DISK_GB:-120}"
IMAGE_FAMILY="ubuntu-2404-lts-arm64"
IMAGE_PROJECT="ubuntu-os-cloud"

# Every instance this script touches carries this prefix. The teardown refuses
# to act on anything else -- a name typo must fail closed, not delete a VM that
# matters.
NAME_PREFIX="clade-builder"
STATE_FILE="$REPO_ROOT/build/clade/.gcp-instance"

# The upstream base of the Thylacine LLVM fork. `git describe`d from the fork:
# the 6 Thylacine commits sit directly on this tag.
LLVM_TAG="${CLADE_LLVM_TAG:-llvmorg-22.1.8}"
FORK_DIR="${LLVMFORK:-$HOME/projects/llvm-thylacine}"

say() { printf '==> %s\n' "$*"; }
die() { printf 'clade-gcp: %s\n' "$*" >&2; exit 1; }

instance_name() {
    [[ -f "$STATE_FILE" ]] || die "no instance recorded (run 'up' first)"
    cat "$STATE_FILE"
}

# Guard EVERY mutating call. A name that does not start with our prefix is a
# bug or a typo, and the right response to either is to stop.
assert_ours() {
    case "$1" in
        "$NAME_PREFIX"-*) ;;
        *) die "refusing to touch instance '$1' -- not a $NAME_PREFIX-* instance" ;;
    esac
}

gc() { gcloud --project="$PROJECT" "$@"; }

cmd_up() {
    [[ -f "$STATE_FILE" ]] && die "an instance is already recorded ($(cat "$STATE_FILE")); 'down' it first"
    local name="$NAME_PREFIX-$(date +%Y%m%d-%H%M%S)"
    assert_ours "$name"
    mkdir -p "$(dirname "$STATE_FILE")"

    say "creating $name ($MACHINE spot, $DISK_GB GB, $ZONE)"
    gc compute instances create "$name" \
        --zone="$ZONE" \
        --machine-type="$MACHINE" \
        --provisioning-model=SPOT \
        --instance-termination-action=DELETE \
        --image-family="$IMAGE_FAMILY" \
        --image-project="$IMAGE_PROJECT" \
        --boot-disk-size="${DISK_GB}GB" \
        --boot-disk-type=pd-balanced \
        --labels=purpose=clade-builder,ephemeral=true
    # Record the name only after a successful create, so a failed create cannot
    # leave a state file pointing at an instance that does not exist.
    echo "$name" > "$STATE_FILE"
    say "waiting for ssh"
    local i
    for i in $(seq 1 40); do
        if gc compute ssh "$name" --zone="$ZONE" --command=true \
             -- -o ConnectTimeout=10 -o StrictHostKeyChecking=no >/dev/null 2>&1; then
            say "ssh up"
            return 0
        fi
        sleep 15
    done
    die "ssh never came up -- 'down' the instance"
}

cmd_run() {
    local name; name="$(instance_name)"; assert_ours "$name"

    say "uploading the fork patch series + the local build.sh"
    local payload; payload="$(mktemp -d)"
    trap 'rm -rf "$payload"' RETURN
    # The 6 Thylacine commits (~35 KB). The VM clones upstream $LLVM_TAG itself
    # -- far faster from GCP than pushing an LLVM tree up a home connection.
    ( cd "$FORK_DIR" && git format-patch --output-directory "$payload/patches" "$LLVM_TAG..HEAD" ) >/dev/null
    local n; n="$(ls "$payload/patches" | wc -l | tr -d ' ')"
    [[ "$n" -gt 0 ]] || die "no fork patches produced from $LLVM_TAG..HEAD"
    say "  $n fork patches"
    # The WORKING COPY of build.sh, which may carry uncommitted recipe edits.
    # This is the whole anti-drift story: the VM runs this exact file.
    cp "$REPO_ROOT/tools/build.sh" "$payload/build.sh"
    # Stage 1's recipe, shared verbatim with clade-keep-build.sh. It used to be
    # inlined in the remote script below; two builder drivers now need it, and a
    # hand-copied second mirror is the #100 failure mode.
    cp "$REPO_ROOT/tools/clade-stage1.sh" "$payload/clade-stage1.sh"
    write_remote_script > "$payload/remote-build.sh"

    tar -C "$payload" -czf "$payload/payload.tgz" patches build.sh clade-stage1.sh remote-build.sh
    gc compute scp "$payload/payload.tgz" "$name:~/payload.tgz" --zone="$ZONE" >/dev/null

    say "launching the build (detached; it survives an ssh drop)"
    # TWO separate things are required to detach, and fixing only the first
    # leaves the hang exactly as it was:
    #
    #  1. `< /dev/null` -- otherwise the child holds the ssh channel's STDIN
    #     and sshd keeps the session open.
    #  2. `;` between the setup steps, NOT `&&`. In `A && B && C & echo`, the
    #     `&` binds to the whole `&&` LIST: the entire chain becomes one
    #     backgrounded subshell that runs nohup in ITS foreground and waits,
    #     with the ssh channel still attached to that subshell's stdout/stderr.
    #     The build detaches correctly and the launcher still blocks for its
    #     whole duration. With `;` separators the `&` binds to the nohup alone.
    #
    # The symptom of getting this wrong is deceptive: the build is genuinely
    # running and healthy, so only the launcher looks broken.
    gc compute ssh "$name" --zone="$ZONE" --command \
        'set -e; cd ~; rm -rf payload; mkdir payload; tar -xzf payload.tgz -C payload;
         chmod +x payload/remote-build.sh;
         nohup payload/remote-build.sh < /dev/null > ~/build.log 2>&1 &
         echo "launched pid $!"'
    say "poll with: tools/clade-gcp-build.sh log"
}

cmd_log() {
    local name; name="$(instance_name)"; assert_ours "$name"
    gc compute ssh "$name" --zone="$ZONE" --command 'tail -n 40 ~/build.log; echo "---"; cat ~/build.status 2>/dev/null || echo "STATUS: running"'
}

cmd_fetch() {
    local name; name="$(instance_name)"; assert_ours "$name"
    local status
    status="$(gc compute ssh "$name" --zone="$ZONE" --command 'cat ~/build.status 2>/dev/null || true' 2>/dev/null || true)"
    case "$status" in
        *OK*) ;;
        *) die "remote build is not OK yet (status: ${status:-running}) -- refusing to fetch a partial tree" ;;
    esac
    local dest="$REPO_ROOT/build/clade/llvm-build"
    mkdir -p "$dest/bin"
    say "fetching artifacts into $dest"
    gc compute ssh "$name" --zone="$ZONE" --command \
        'cd ~/thylacine/build/clade/llvm-build && tar -czf ~/artifacts.tgz bin/llvm bin/clangd lib/clang' >/dev/null
    gc compute scp "$name:~/artifacts.tgz" "$REPO_ROOT/build/clade/artifacts.tgz" --zone="$ZONE" >/dev/null
    tar -xzf "$REPO_ROOT/build/clade/artifacts.tgz" -C "$dest"
    rm -f "$REPO_ROOT/build/clade/artifacts.tgz"
    say "got: $(ls -la "$dest/bin/llvm" "$dest/bin/clangd" | awk '{print $NF" "$5}' | tr '\n' ' ')"
}

cmd_down() {
    [[ -f "$STATE_FILE" ]] || { say "no instance recorded -- nothing to delete"; return 0; }
    local name; name="$(cat "$STATE_FILE")"; assert_ours "$name"
    say "deleting $name"
    gc compute instances delete "$name" --zone="$ZONE" --quiet || true
    rm -f "$STATE_FILE"
    # Verify rather than assume: an instance that survived a failed delete is
    # one that keeps billing.
    if gc compute instances describe "$name" --zone="$ZONE" >/dev/null 2>&1; then
        die "instance $name STILL EXISTS after delete -- check the console"
    fi
    say "deleted; nothing of ours is running"
}

cmd_status() {
    say "instances matching $NAME_PREFIX-* in $PROJECT:"
    gc compute instances list --filter="name~^$NAME_PREFIX" \
        --format="table(name,zone,machineType.basename(),status)" || true
    [[ -f "$STATE_FILE" ]] && say "state file records: $(cat "$STATE_FILE")"
    return 0
}

# The script that runs ON the VM. It does the two stages and drops a status
# file the local side polls; every phase is echoed so a failure names itself.
#
# The heredoc is QUOTED so the remote body's own `$…` survive verbatim; the one
# value that must come from here is the LLVM tag, substituted on the way out.
write_remote_script() {
sed "s|TAG_PLACEHOLDER|$LLVM_TAG|g" <<'REMOTE'
#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
status() { echo "STATUS: $*" > ~/build.status; }
phase()  { echo; echo "######## $* ########"; date -u +%FT%TZ; }
trap 'status "FAILED (see build.log)"' ERR
status "running"

# DEAD-MAN SWITCH. A GCP *Spot* VM has no maximum runtime (unlike the retired
# preemptible 24h cap), so one forgotten after a dropped session bills until a
# human notices. Everything that would normally tear this down -- the monitor,
# the orchestrator, the operator's memory -- lives in a session that can end.
# This does not.
#
# HALT, not delete: it stops the CPU/RAM burn (the ~99% of the cost) while
# leaving the disk and therefore the built artifacts intact, so a late fetch is
# still possible by restarting the instance. Deleting would be tidier and
# strictly worse -- a dead-man switch must not destroy the work it is guarding.
# The window is deliberately far past any legitimate build.
sudo shutdown -c 2>/dev/null || true
sudo shutdown -h +"${DEADMAN_MIN:-360}" "clade-builder dead-man" 2>&1 | tail -1

phase "deps"
sudo apt-get update -qq
sudo apt-get install -y -qq git cmake ninja-build clang lld python3 zlib1g-dev >/dev/null

# Ubuntu 24.04 ships llvm-18; build.sh reaches for llvm-ar/nm/ranlib/strip/readelf
# under $LLVM_PREFIX/bin (a Homebrew path on the dev box).
LLVMP="$(ls -d /usr/lib/llvm-* 2>/dev/null | sort -V | tail -1)"
[[ -x "$LLVMP/bin/llvm-ar" ]] || { echo "no usable llvm toolchain under /usr/lib"; exit 1; }
echo "host llvm: $LLVMP"

phase "clone llvm-project @ TAG"
if [[ ! -d ~/llvm-thylacine ]]; then
  git clone --depth 1 --branch "TAG_PLACEHOLDER" \
      https://github.com/llvm/llvm-project.git ~/llvm-thylacine
fi
phase "stage 1: the fork's HOST toolchain (a stock clang cannot target aarch64-thylacine)"
# The recipe -- patch series, cmake, ninja target list, and the triple probe --
# lives in tools/clade-stage1.sh, shared verbatim with clade-keep-build.sh. It
# was inlined here until a second builder driver needed it; keeping two copies
# is the failure mode this file's header warns about (#100), so there is now one
# original and two callers. Build dir is the default ($src/build), which is what
# build.sh's hardcoded "$fork/build/bin/..." lookups expect.
chmod +x ~/payload/clade-stage1.sh
~/payload/clade-stage1.sh \
    --src ~/llvm-thylacine \
    --jobs "$(nproc)" \
    --patches ~/payload/patches --tag "TAG_PLACEHOLDER"

phase "clone thylacine + overlay the working-copy build.sh"
if [[ ! -d ~/thylacine ]]; then
  git clone --depth 1 https://github.com/apology-is-policy/thylacine.git ~/thylacine
fi
cp ~/payload/build.sh ~/thylacine/tools/build.sh
chmod +x ~/thylacine/tools/build.sh

phase "stage 2: sysroot + libcxx + clade (the REAL build.sh recipe)"
cd ~/thylacine
export LLVMFORK=~/llvm-thylacine
# ONE toolchain version end to end (see the stage-1 note): the fork's own
# 22.1.8 build supplies clang AND the binutils AND ld.lld, matching the dev
# box's stock 22.1.4 far more closely than the distro's 18 would.
export LLVM_PREFIX=~/llvm-thylacine/build
export LLD_PREFIX=~/llvm-thylacine/build
# build_clade derives its -j from `sysctl -n hw.memsize`, which does not exist
# on Linux; the fallback assumes 8 GiB and would give -j2 on a 64 GiB machine.
export CLADE_JOBS="$(nproc)"
echo "CLADE_JOBS=$CLADE_JOBS"
tools/build.sh sysroot
tools/build.sh libcxx
tools/build.sh clade

phase "verify"
ls -la ~/thylacine/build/clade/llvm-build/bin/llvm ~/thylacine/build/clade/llvm-build/bin/clangd
# Read the artifact with the toolchain that produced it.
# sed, not `head -12`: an early-exiting reader under `set -o pipefail` makes the
# writer die on SIGPIPE and the pipeline yield 141, aborting the run for no
# reason. It only fires when the writer still has output buffered, so it is
# intermittent -- pre-existing here, and the same race that did fire in the
# permanent builder's stage-3 verify.
~/llvm-thylacine/build/bin/llvm-readelf -h ~/thylacine/build/clade/llvm-build/bin/clangd | sed -n '1,12p'
status "OK"
echo "BUILD OK"
REMOTE
}

case "${1:-}" in
    up)     cmd_up ;;
    run)    cmd_run ;;
    log)    cmd_log ;;
    fetch)  cmd_fetch ;;
    down)   cmd_down ;;
    status) cmd_status ;;
    all)    cmd_up; cmd_run; say "build launched -- poll with 'log', then 'fetch' and ALWAYS 'down'" ;;
    *)      die "usage: $0 {up|run|log|fetch|down|status|all}" ;;
esac
