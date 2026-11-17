#!/usr/bin/env python3
"""TEST_LEVERS (vote 136, #880): build.sh's display-lever strip and its two checks, on the host.

build_userspace_rust runs against a stub cargo that records every invocation and answers
`cargo tree` the way resolver 2 resolves this workspace's lever crates (tapestryd's
test-mode turns lictor's on; corvus and caps-probe take lictor without defaults).
test_levers_verdict runs on fixture binaries both ways. test_mode_packages also runs
against the REAL cargo on the real workspace, the oracle the stub stands in for.
--mutants: each build.sh mutant below must turn its case red.
Exit 0 all pass, 1 a failure, 3 the real cargo or its target is missing (TOOLING)."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_SH = os.path.join(REPO, "tools", "build.sh")
TARGET = "aarch64-unknown-none"

STUB_CARGO = r'''#!/bin/bash
# A stand-in for cargo: records each call; models resolver 2 for the lever crates.
echo "$*" >> "$STUB_LOG"
cmd=$1; shift
members="hello-rs corvus caps-probe kaua-term-probe tapestryd halcyond lictor"
sel=(); excl=(); ws=0; nodef=0; feats=","
while [ $# -gt 0 ]; do
    case $1 in
        -p) sel+=("$2"); shift 2 ;;
        --exclude) excl+=("$2"); shift 2 ;;
        --workspace) ws=1; shift ;;
        --no-default-features) nodef=1; shift ;;
        --features) feats="$feats$2,"; shift 2 ;;
        --target|-e|--prefix|-f) shift 2 ;;
        *) shift ;;
    esac
done
roots=()
if [ ${#sel[@]} -gt 0 ]; then roots=("${sel[@]}")
else for m in $members; do keep=1; for x in ${excl[@]+"${excl[@]}"}; do [ "$x" = "$m" ] && keep=0; done; [ $keep = 1 ] && roots+=("$m"); done; fi
isroot() { for r in "${roots[@]}"; do [ "$r" = "$1" ] && return 0; done; return 1; }
tm() {  # does lever crate $1 get test-mode?
    [ -n "${STUB_NO_TESTMODE:-}" ] && return 1
    local c=$1
    case $c in tapestryd|halcyond|lictor) ;; *) return 1 ;; esac
    if isroot "$c"; then
        [ $nodef = 0 ] && return 0
        case $feats in *",$c/test-mode,"*) return 0 ;; esac
    fi
    if [ "$c" = lictor ] && [ -z "${STUB_ROOTS_ONLY:-}" ]; then tm tapestryd && return 0; fi
    return 1
}
present() { isroot "$1" && return 0; [ "$1" = lictor ] && { isroot tapestryd || isroot corvus || isroot caps-probe; }; }
if [ "$cmd" = tree ]; then
    for c in hello-rs corvus caps-probe kaua-term-probe tapestryd halcyond lictor; do
        present "$c" || continue
        f=guest; tm "$c" 2>/dev/null && f="$f,test-mode"
        line="$c v0.1.0 (/stub/usr/$c)|$f"
        if [ "$c" = lictor ] && [ -n "${STUB_STAR_ONLY:-}" ]; then echo "$line (*)"
        else echo "$line"; [ "$c" = lictor ] && echo "$line (*)"; fi
    done
    exit 0
fi
if [ "$cmd" = build ]; then
    d="$USR_RS_BUILD/$USR_RS_TARGET/release"; mkdir -p "$d"
    for c in tapestryd halcyond lictor hello-rs; do
        isroot "$c" || continue
        if tm "$c" 2>/dev/null; then printf 'x\0test levers compiled in\0' > "$d/$c"; else printf 'x\0' > "$d/$c"; fi
    done
    # A member failed after the lever crates built: only the exit status says so.
    [ -n "${STUB_BUILD_FAIL:-}" ] && [ ${#sel[@]} = 0 ] && exit 101
    exit 0
fi
echo "stub cargo: unexpected $cmd" >&2; exit 2
'''

# Each mutant: (name, anchor, replacement, the case that must turn red).
MUTANTS = [
    ("SM1 strip keeps default features",
     '          --no-default-features --features "$strip_features" ) || return 1\n',
     '          --features "$strip_features" ) || return 1\n', "A2"),
    ("SM2 workspace build keeps the lever crates",
     '        for lb in "${TEST_LEVER_BINS[@]}"; do ws_sel+=(--exclude "$lb"); strip_sel+=(-p "$lb"); done\n',
     '        for lb in "${TEST_LEVER_BINS[@]}"; do strip_sel+=(-p "$lb"); done\n', "A2"),
    ("YC no dependency-level control",
     '        if [[ "$tm_pkgs " != *" lictor "* ]]; then\n',
     '        if false; then\n', "A7"),
    ("SF3 the duplicate marker not stripped",
     'sub(/ \\(\\*\\)$/, "", $2); ', '', "A8"),
    ("SR1 no verdict in build_ramfs",
     '    test_levers_verdict "$ramfs_bin" "staged in the ramfs; rebuild userspace under this config" 0 || exit 1\n',
     '    true\n', "S1"),
    ("RET the workspace build's failure ignored",
     '${hi_features[@]+"${hi_features[@]}"} ) || return 1\n',
     '${hi_features[@]+"${hi_features[@]}"} )\n', "A10"),
    ("RQ readiness allowed under n",
     '    1) if (( levers == 0 )); then\n               echo "THYLACINE_READINESS_QUALIFICATION=1 cannot combine',
     '    1) if false; then\n               echo "THYLACINE_READINESS_QUALIFICATION=1 cannot combine', "A5"),
]


def lift(src):
    """The helpers (TEST_LEVER_BINS .. build_ramfs) and build_userspace_rust, as one sourceable file."""
    a = src.index("\nTEST_LEVER_BINS=")
    b = src.index("\nbuild_ramfs() {", a)
    c = src.index("\nbuild_userspace_rust() {")
    d = src.index("\n}\n", c) + 3
    return src[a:b] + "\n" + src[c:d]


class Run:
    def __init__(self, src, real_ok):
        self.src = src
        self.real_ok = real_ok
        self.t = tempfile.mkdtemp(prefix="test-levers-")
        os.makedirs(os.path.join(self.t, "bin"))
        os.makedirs(os.path.join(self.t, "usr"))
        stub = os.path.join(self.t, "bin", "cargo")
        with open(stub, "w") as f:
            f.write(STUB_CARGO)
        os.chmod(stub, 0o755)
        self.lib = os.path.join(self.t, "lib.sh")
        with open(self.lib, "w") as f:
            f.write(lift(src))

    def bash(self, body, env=None, stub=True, root=None):
        log = os.path.join(self.t, "cargo.log")
        if os.path.exists(log):
            os.remove(log)
        rsb = os.path.join(self.t, "build", "usr-rs")
        shutil.rmtree(rsb, ignore_errors=True)
        e = {k: v for k, v in os.environ.items() if not k.startswith(("THYLACINE_", "STUB_"))}
        e.update({"STUB_LOG": log, "USR_RS_BUILD": rsb, "USR_RS_TARGET": TARGET})
        if stub:
            e["PATH"] = os.path.join(self.t, "bin") + ":" + e["PATH"]
        e.update(env or {})
        script = ('set -uo pipefail\nREPO_ROOT=%s\nUSR_RS_TARGET=%s\nUSR_RS_BUILD=%s\nverbose=""\n'
                  'ledger() { echo "LEDGER: $*"; }\nsource %s\n%s\n') % (
                      root or self.t, TARGET, rsb, self.lib, body)
        p = subprocess.run(["bash", "-c", script], env=e, capture_output=True, text=True)
        calls = open(log).read().splitlines() if os.path.exists(log) else []
        return p.returncode, p.stdout + p.stderr, calls

    def rust(self, **env):
        rc, out, calls = self.bash('build_userspace_rust; echo "RC=$?"', env)
        m = re.search(r"RC=(\d+)", out)
        return (int(m.group(1)) if m else 99), out, [c for c in calls if c.startswith("build")], calls

    def cases(self):
        """name -> (ok, detail). Every case is a claim that can be made to fail."""
        r = {}
        builds = lambda calls: [c for c in calls if c.startswith("build")]
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="1")
        r["A1"] = (rc == 0 and b == ["build --release"] and "LEDGER: test levers: COMPILED IN" in out,
                   "y: one default workspace build, COMPILED IN (rc %d, builds %s)" % (rc, b))
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="0")
        want = ["build --release --workspace --exclude tapestryd --exclude halcyond --exclude lictor",
                "build --release -p tapestryd -p halcyond -p lictor --no-default-features "
                "--features tapestryd/guest,halcyond/guest,lictor/backend"]
        r["A2"] = (rc == 0 and b == want and "LEDGER: test levers: STRIPPED" in out,
                   "n: the excluded workspace build then the stripped three, STRIPPED (rc %d, builds %s, tail %r)"
                   % (rc, b, out.strip().splitlines()[-1:] if out.strip() else ""))
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="0", THYLACINE_INTERACTION_QUALIFICATION="1")
        r["A3"] = (rc == 0 and len(b) == 2 and "interaction-qualification" not in b[0]
                   and b[1].endswith(",halcyond/interaction-qualification"),
                   "n + interaction qualification: the feature rides the strip only (builds %s)" % b)
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="1", THYLACINE_INTERACTION_QUALIFICATION="1")
        r["A4"] = (rc == 0 and b == ["build --release --features halcyond/interaction-qualification"],
                   "y + interaction qualification on the workspace build (builds %s)" % b)
        rc, out, b, calls = self.rust(THYLACINE_TEST_LEVERS="0", THYLACINE_READINESS_QUALIFICATION="1")
        r["A5"] = (rc != 0 and "cannot combine with THYLACINE_TEST_LEVERS=0" in out and not calls,
                   "n + readiness refused before any cargo call (rc %d, calls %s)" % (rc, calls))
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="1", STUB_NO_TESTMODE="1")
        r["A6"] = (rc != 0 and "does not resolve test-mode on for tapestryd" in out and not b,
                   "y: a resolver that reports no test-mode fails the control (rc %d)" % rc)
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="1", STUB_ROOTS_ONLY="1")
        r["A7"] = (rc != 0 and "lictor as a dependency" in out and not b,
                   "y: a check that sees only selected crates fails the dependency control (rc %d)" % rc)
        rc, out, _ = self.bash("test_mode_packages --workspace --exclude lictor", {"STUB_STAR_ONLY": "1"})
        r["A8"] = (rc == 0 and out.split() == ["halcyond", "lictor", "tapestryd"],
                   "a test-mode crate listed only with cargo's ' (*)' marker is still seen (got %r)" % out.strip())
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="2")
        r["A9"] = (rc != 0 and "THYLACINE_TEST_LEVERS must be 0 or 1" in out and not b, "a bad value refused")
        rc, out, b, _ = self.rust(THYLACINE_TEST_LEVERS="1", STUB_BUILD_FAIL="1")
        r["A10"] = (rc != 0 and "LEDGER" not in out,
                    "a failed workspace build stops the function without errexit (rc %d)" % rc)
        # The verdict, both ways, on fixture binaries.
        d = os.path.join(self.t, "fx")
        for sub, text in (("in", b"x\0test levers compiled in\0"), ("out", b"x\0none\0")):
            os.makedirs(os.path.join(d, sub), exist_ok=True)
            for lb in ("tapestryd", "halcyond", "lictor"):
                with open(os.path.join(d, sub, lb), "wb") as f:
                    f.write(text)
        os.makedirs(os.path.join(d, "none"), exist_ok=True)

        def v(levers, sub, req):
            rc, out, _ = self.bash('test_levers_verdict "%s/%s" t %d; echo "RC=$?"' % (d, sub, req),
                                   {"THYLACINE_TEST_LEVERS": levers})
            return int(re.search(r"RC=(\d+)", out).group(1)), out
        cases_v = [("V1", "1", "in", 1, 0, ""), ("V2", "0", "out", 1, 0, ""),
                   ("V3", "0", "in", 1, 1, "tapestryd (t) still carries 'test levers compiled in' -- the strip did not take"),
                   ("V4", "1", "out", 1, 1, "tapestryd (t) lacks 'test levers compiled in'"),
                   ("V5", "0", "none", 0, 0, ""), ("V6", "1", "none", 0, 0, ""),
                   ("V7", "0", "none", 1, 1, "was not built")]
        for name, lv, sub, req, want_rc, want_txt in cases_v:
            rc, out = v(lv, sub, req)
            r[name] = (rc == want_rc and want_txt in out,
                       "verdict levers=%s dir=%s require=%d -> rc %d want %d" % (lv, sub, req, rc, want_rc))
        # The ramfs consumer: the verdict runs on the staged binaries, after the copy.
        a = self.src.find("\nbuild_ramfs() {\n")
        nxt = re.compile(r"\n[a-z_]+\(\) \{\n").search(self.src, a + 1) if a >= 0 else None
        body = self.src[a:nxt.start()] if a >= 0 and nxt else ""
        cp = body.find('cp "$src" "$ramfs_bin/$bin"')
        call = body.find('test_levers_verdict "$ramfs_bin"')
        r["S1"] = (cp >= 0 and call > cp and "|| exit 1" in body[call:body.find("\n", call)],
                   "build_ramfs judges the staged lever binaries after the copy (copy at %d, call at %d)" % (cp, call))
        # The oracle: the real cargo on the real workspace.
        if self.real_ok:
            real = os.path.join(REPO)

            def tmp(args):
                rc, out, _ = self.bash("test_mode_packages %s" % args, stub=False, root=real)
                return rc, out.split()
            for name, args, want in (
                    ("R1", "", ["halcyond", "lictor", "tapestryd"]),
                    ("R2", "--workspace --exclude tapestryd --exclude halcyond --exclude lictor", []),
                    ("R3", "-p tapestryd -p halcyond -p lictor --no-default-features "
                           "--features tapestryd/guest,halcyond/guest,lictor/backend", []),
                    ("R4", "--workspace --exclude lictor", ["lictor", "tapestryd"])):
                rc, got = tmp(args)
                ok = rc == 0 and (got == want if name != "R4" else "lictor" in got)
                r[name] = (ok, "real cargo tree %r -> %s" % (args or "(the build's selection)", got))
            rc, got = tmp("--workspace --exclude halcyond --features halcyond/interaction-qualification")
            r["R5"] = (rc != 0, "a selection cargo rejects fails the check (rc %d)" % rc)
        return r


def real_cargo_ok():
    if not shutil.which("cargo"):
        return False
    p = subprocess.run(["rustup", "target", "list", "--installed"], capture_output=True, text=True)
    return p.returncode == 0 and TARGET in p.stdout.split()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mutants", action="store_true", help="also run each build.sh mutant")
    args = ap.parse_args()
    if not real_cargo_ok():
        print("TOOLING: cargo or the %s target is missing, so the oracle cases cannot run" % TARGET)
        return 3
    src = open(BUILD_SH).read()
    run = Run(src, True)
    fails = 0
    res = run.cases()
    for name in res:
        ok, detail = res[name]
        print("%-4s %-4s %s" % ("ok" if ok else "FAIL", name, detail))
        fails += 0 if ok else 1
    if args.mutants:
        for mname, anchor, repl, case in MUTANTS:
            if src.count(anchor) != 1:
                print("FAIL mutant %s: anchor found %d times" % (mname, src.count(anchor)))
                fails += 1
                continue
            mres = Run(src.replace(anchor, repl), False).cases()
            killed = not mres[case][0]
            print("%-4s mutant %s: case %s %s" % ("ok" if killed else "FAIL", mname, case,
                                                  "turns red" if killed else "stays green -- the case cannot see it"))
            fails += 0 if killed else 1
    print("test-test-levers: %s (%d case(s)%s)" % ("ALL PASS" if not fails else "%d FAIL" % fails, len(res),
                                                   ", %d mutant(s)" % len(MUTANTS) if args.mutants else ""))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
