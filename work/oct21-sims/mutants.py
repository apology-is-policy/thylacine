#!/usr/bin/env python3
# Both ways for the sims' admission witness (admission_follows_the_kernels_order
# in usr/lib/libt/tests/loom_client/test_adapter.c and
# usr/lib/loom-client/src/tests/native.rs). The unmutated tree must pass it in
# both languages; each mutant undoes ONE of the fixes in one sim and must fail it
# at the assertion that pins that fix, read from the failure's file:line, not
# just from the exit status. A mutant whose build fails proves nothing and fails
# this run. Every edit is an exact replacement whose `old` occurs once; the
# file is restored whatever happens.
# Usage (repo root, under a mac lease): python3 -I work/oct21-sims/mutants.py <logdir>
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CSIM = ROOT / "usr/lib/libt/tests/loom_client/sim_kernel.c"
CTEST = ROOT / "usr/lib/libt/tests/loom_client/test_adapter.c"
RSIM = ROOT / "usr/lib/loom-client/src/tests/native/sim.rs"
RTEST = ROOT / "usr/lib/loom-client/src/tests/native.rs"
NAME = "admission_follows_the_kernels_order"
LLVM = Path(os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm"))
CORES = "adapter fixed_requests lease_ledger owned_mapping paired_cq request_book retirement_executor ring_payload".split()

# (label, file, old, new, the witness statement that must fail: C names the
# statement BEFORE the failing assert, Rust the failing assert_eq itself)
MUTANTS = [
    ("c-order", CSIM,
     "        /* PREPARE, the payload (loom_private_io_prepare, loom_provided_prepare). */\n",
     "        /* PREPARE, the payload (loom_private_io_prepare, loom_provided_prepare). */\n"
     "        if ((q->opcode == T_LOOM_OP_READ || q->opcode == T_LOOM_OP_WRITE) &&\n"
     "            !(src->open & (q->opcode == T_LOOM_OP_READ ? 1u : 2u)))\n"
     "            return SIM_EACCES;\n",
     "    q = adm_read(1, 5, 4);"),
    ("c-clunking", CSIM, "            src->clunking = 1;\n", "",
     "    q = adm_sqe(T_LOOM_OP_CLUNK, 8);\n    assert(sim_admit(s, &q) == 0);"),
    ("c-walk-claim", CSIM, " || walk_claimed(s, dest.slot)) return SIM_EBUSY;",
     " || (walk_claimed(s, dest.slot) && 0)) return SIM_EBUSY;",
     "    q = adm_walk(0, 3, 0, 0);"),
    ("c-dest-scope", CSIM, "if (d->scope != scope) return SIM_ECANCELED;", "if (d->scope != scope) return SIM_ENOENT;",
     "    q = adm_walk(1, 5, 0, 0);"),
    ("c-bounds", CSIM, "                if (s->pools[pool.slot][m].range.length < q->len) return SIM_EINVAL;\n",
     "                (void)m;\n", "    q = adm_pooled(2, 8, 8);"),
    ("c-clunk-fid", CSIM, "if (src->kind != SIM_FID || src->state != SIM_READY) return SIM_ENOENT;",
     "if (src->state != SIM_READY) return SIM_ENOENT;", "    q = adm_sqe(T_LOOM_OP_CLUNK, 0);"),
    ("r-order", RSIM,
     "                let payload = if o == OP_WALK as u32 {\n",
     "                if (o == OP_READ as u32 || o == OP_WRITE as u32)\n"
     "                    && src.open.unwrap_or(0) & (if o == OP_READ as u32 { 1 } else { 2 }) == 0\n"
     "                {\n"
     "                    return EACCES;\n"
     "                }\n"
     "                let payload = if o == OP_WALK as u32 {\n",
     "    assert_eq!(s.admit_one(read(1, 5, 4)), EINVAL);"),
    ("r-clunking", RSIM, "                    self.slots[source.slot as usize].as_mut().unwrap().clunking = true;\n", "",
     "    assert_eq!(s.admit_one(sqe(OP_CLUNK, 8)), ENOENT);"),
    ("r-walk-claim", RSIM, "if d.state != RESERVED || self.walk_claimed(dest.slot as usize) {",
     "if d.state != RESERVED {", "    assert_eq!(s.admit_one(walk(0, 3, 0, 0)), EBUSY);"),
    ("r-dest-scope", RSIM, "                        if d.scope != scope {\n                            return ECANCELED;",
     "                        if d.scope != scope {\n                            return ENOENT;",
     "    assert_eq!(s.admit_one(walk(1, 5, 0, 0)), ECANCELED);"),
    ("r-bounds", RSIM, ".any(|m| m.range.length < s.len as u64)", ".any(|_| false)",
     "    assert_eq!(s.admit_one(pooled(2, 8, 8)), EINVAL);"),
    ("r-clunk-fid", RSIM, "if src.kind != SlotKind::Fid || src.state != READY {", "if src.state != READY {",
     "    assert_eq!(s.admit_one(sqe(OP_CLUNK, 0)), ENOENT);"),
]


def line_of(path, stmt, after):
    t = path.read_text()
    assert t.count(stmt) == 1, (path.name, stmt)
    return t[: t.index(stmt)].count("\n") + 1 + stmt.count("\n") + after


def run(cmd, log, cwd=None):
    with open(log, "w") as out:
        return subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=cwd).returncode


def c_cores(out):
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()
    flags = ["-isysroot", sdk, "-Wall", "-Wextra", "-Werror", "-std=c99", "-O1", "-DT_REQUEST_TEST",
             f"-I{ROOT}/usr/lib/libt/include"]
    objs = []
    for c in CORES:
        o = out / f"{c}.o"
        assert run([str(LLVM / "bin/clang"), *flags, "-c", str(ROOT / f"usr/lib/libt/src/loom_client/{c}.c"),
                    "-o", str(o)], out / f"{c}.build.log") == 0, c
        objs.append(str(o))
    hf = out / "host_fatal.o"
    assert run([str(LLVM / "bin/clang"), *flags[:-3], "-std=c11", f"-I{ROOT}/usr/lib/libt/include", "-c",
                str(CTEST.parent / "host_fatal.c"), "-o", str(hf)], out / "host_fatal.build.log") == 0
    return sdk, objs + [str(hf)]


def c_leg(label, out, sdk, objs):
    exe = out / f"{label}.test_adapter"
    cmd = [str(LLVM / "bin/clang"), "-isysroot", sdk, "-Wall", "-Wextra", "-Werror", "-std=c11", "-O1",
           "-DT_REQUEST_TEST", f"-I{ROOT}/usr/lib/libt/include", str(CTEST), str(CSIM), *objs, "-lpthread",
           "-o", str(exe)]
    if run(cmd, out / f"{label}.build.log"):
        return "build", ""
    rc = run([str(exe), NAME], out / f"{label}.log")
    return rc, (out / f"{label}.log").read_text()


def r_leg(label, out):
    triple = re.search(r"^host: (\S+)", subprocess.run(["rustc", "-vV"], capture_output=True, text=True).stdout,
                       re.M).group(1)
    rc = run(["cargo", "test", "-p", "loom-client", "--lib", "--target", triple, NAME], out / f"{label}.log",
             cwd=ROOT / "usr")
    text = (out / f"{label}.log").read_text()
    if rc and ("error[" in text or "could not compile" in text):
        return "build", text
    return rc, text


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for _, f, old, _, _ in MUTANTS:
        assert f.read_text().count(old) == 1, (f.name, old[:60])
    sdk, objs = c_cores(out)
    bad = []
    rc, text = c_leg("control", out, sdk, objs)
    ok = rc == 0 and f"C native adapter scenario {NAME} PASS" in text
    print(f"{'PASS' if ok else 'FAIL'} c control: rc {rc}", flush=True)
    bad += [] if ok else ["c-control"]
    rc, text = r_leg("r-control", out)
    ok = rc == 0 and f"{NAME} ... ok" in text and "1 passed" in text
    print(f"{'PASS' if ok else 'FAIL'} rust control: rc {rc}", flush=True)
    bad += [] if ok else ["r-control"]
    for label, f, old, new, stmt in MUTANTS:
        before = f.read_text()
        try:
            f.write_text(before.replace(old, new))
            if label.startswith("c-"):
                # glibc prints file:line, Darwin "file F, line N."
                want = rf"test_adapter\.c(:|, line ){line_of(CTEST, stmt, 1)}\b"
                rc, text = c_leg(label, out, sdk, objs)
            else:
                want = rf"native\.rs:{line_of(RTEST, stmt, 0)}:"
                rc, text = r_leg(label, out)
        finally:
            f.write_text(before)
        killed = rc not in (0, "build") and re.search(want, text) is not None
        print(f"{'KILLED' if killed else 'SURVIVED'} {label}: rc {rc}; wanted the failure at {want}", flush=True)
        if not killed:
            bad.append(label)
    subprocess.run(["git", "-C", str(ROOT), "diff", "--exit-code", "--stat", "--", str(CSIM), str(RSIM)], check=True)
    if bad:
        print("FAILED:", " ".join(bad))
        sys.exit(1)
    print(f"ALL KILLED: {len(MUTANTS)} mutants; both controls pass")


if __name__ == "__main__":
    main()
