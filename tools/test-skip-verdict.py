#!/usr/bin/env python3
"""The kernel suite's skip verdict (vote 17), on the host.

tools/run-vm.sh runs to its exec under a stand-in qemu-system-aarch64 that
records its argv, once per launcher configuration: the bootargs it declares
must be exactly the instances that configuration attaches. It runs as a
byte-identical copy inside a scratch tree, so the sockets and directories the
launcher clears or makes under its own root never touch this checkout's,
where a boot may be running. tools/test.sh's
skip block, cut from the script, then reads one synthetic skip per kind under
the same configuration: a skip must pass (as NOT coverage) exactly when
run-vm.sh did not declare that kind, so the launcher's derivation and
test.sh's mirror of it are checked against each other. Then the block's own
refusals, each with its acceptance one variable away: ungated and torn shapes,
the image companions, a missing machine line.
"""
from pathlib import Path
import argparse
import collections
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BLOCK_START = "        # The kernel suite's skips (vote 17). A test skips only through a helper\n"
BLOCK_END = "        # #212: propagate the DISTRO D-5 / LINEAGE L-6c arc gates into the\n"
KINDS = ["virtio-blk", "virtio-net", "virtio-input", "virtio-gpu", "virtio-rng",
         "virtio-pci-rng", "virtio-pci-input", "virtio-pci-net", "virtio-pci-gpu", "virtio-pci-9p",
         "smp", "msi"]
# The kinds test.sh's mirror explains; the others no kernel test needs.
MIRRORED = ["virtio-blk", "virtio-net", "virtio-input", "virtio-gpu", "virtio-rng",
            "virtio-pci-rng", "virtio-pci-input", "smp", "msi"]
FULL = {"virtio-blk": 2, "virtio-net": 1, "virtio-pci-net": 1, "virtio-input": 1,
        "virtio-pci-input": 3, "virtio-gpu": 1, "virtio-pci-gpu": 1, "virtio-rng": 1,
        "virtio-pci-rng": 1, "smp": 1}


def drop(d, *kinds):
    return {k: v for k, v in d.items() if k not in kinds}


def configs(disk, pool):
    base = {"THYLACINE_ACCEL": "hvf", "THYLACINE_NO_AUDIO": "1",
            "THYLACINE_DISK_IMG": disk, "THYLACINE_POOL_IMG": pool}
    gic2 = dict(FULL, msi=1)
    return [
        ("hvf-full", dict(base), 4, gic2),
        ("no-net", dict(base, THYLACINE_NO_NET="1"), 4, drop(gic2, "virtio-net", "virtio-pci-net")),
        ("no-input", dict(base, THYLACINE_NO_INPUT="1"), 4, drop(gic2, "virtio-input", "virtio-pci-input")),
        ("no-gpu", dict(base, THYLACINE_NO_GPU="1"), 4, drop(gic2, "virtio-gpu", "virtio-pci-gpu")),
        ("no-rng", dict(base, THYLACINE_NO_RNG="1"), 4, drop(gic2, "virtio-rng", "virtio-pci-rng")),
        ("display-console", dict(base, THYLACINE_DISPLAY="console"), 4,
         drop(gic2, "virtio-gpu", "virtio-pci-gpu")),
        ("one-cpu", dict(base), 1, drop(gic2, "smp")),
        ("no-disk-no-pool", dict(base, THYLACINE_DISK_IMG="/nonexistent/disk.img",
                                 THYLACINE_POOL_IMG="/nonexistent/pool.img"), 4, drop(gic2, "virtio-blk")),
        ("disk-only", dict(base, THYLACINE_POOL_IMG="/nonexistent/pool.img"), 4, dict(gic2, **{"virtio-blk": 1})),
        ("hvf-gic3-auto", dict(base, THYLACINE_GIC="3"), 4, dict(FULL)),
        ("hvf-gic3-its-on", dict(base, THYLACINE_GIC="3", THYLACINE_ITS="on"), 4, dict(FULL)),
        ("gpu-gl-model", dict(base, THYLACINE_GPU_DEV="virtio-gpu-gl-pci"), 4, gic2),
        ("share", dict(base, SHARE="1"), 4, dict(gic2, **{"virtio-pci-9p": 1})),
        ("tcg-gic3-auto", dict(base, THYLACINE_ACCEL="tcg"), 4, dict(FULL, msi=1)),
        ("tcg-its-off", dict(base, THYLACINE_ACCEL="tcg", THYLACINE_ITS="off"), 4, dict(FULL)),
        ("tcg-its-on", dict(base, THYLACINE_ACCEL="tcg", THYLACINE_ITS="on"), 4, dict(FULL, msi=1)),
        # The pi: KVM with the host's GIC, whose MSI support the launcher cannot see.
        ("kvm-gic-host", dict(base, THYLACINE_ACCEL="kvm"), 4, dict(FULL)),
        # Extras are walked too, in QEMU's long option form and with driver= first.
        ("extra-device", dict(base, THYLACINE_QEMU_EXTRA="--device driver=virtio-net-pci,id=xn0 "
                                                         "-drive if=none,id=xd0,file=/dev/null"),
         4, dict(gic2, **{"virtio-pci-net": 2})),
    ]


# Extra arguments that make QEMU attach a device the walk cannot declare.
REFUSED = ["-nic user,model=virtio-net-pci", "-net nic,model=virtio", "-drive file=/dev/null,if=virtio",
           "-drive file=/dev/null", "-hda /dev/null", "--cdrom /dev/null",
           "-device id=xn0,driver=virtio-net-pci", '-device {"driver":"virtio-net-pci"}']


def launch(work, env, cpus):
    """run-vm.sh's -append words and its '==> qemu:' line under this env."""
    argv = work / "argv.txt"
    env = dict(env)
    share = env.pop("SHARE", None) == "1"
    full = {"PATH": f"{work / 'bin'}:{os.environ['PATH']}", "HOME": os.environ["HOME"],
            "FAKEQ_OUT": str(argv), "THYLACINE_BUILD_DIR": str(work / "kernel"), **env}
    # The launcher clears its QMP sockets under its own root on every run: a
    # sentinel at each sandbox path must be gone afterwards, or the clear
    # landed somewhere else.
    sockets = [work / "build/qmp.sock", work / "build/qmp-gate.sock"]
    for sock in sockets:
        sock.parent.mkdir(parents=True, exist_ok=True)
        sock.write_text("sentinel")
    r = subprocess.run([str(work / "tools/run-vm.sh")] + ([] if share else ["--no-share"]) + ["--cpus", str(cpus)],
                       env=full, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    assert not any(sock.exists() for sock in sockets), "run-vm.sh's copy cleared no socket in its own root"
    args = argv.read_text().split("\n")
    words = args[args.index("-append") + 1].split() if "-append" in args else []
    qemu = [l for l in (r.stdout + r.stderr).split("\n") if l.startswith("==> qemu: accel=")]
    assert len(qemu) == 1, r.stdout + r.stderr
    return words, qemu[0]


def refused(work, extra):
    """run-vm.sh's exit, its stderr, and whether it started QEMU, for these extras."""
    argv = work / "argv.txt"
    if argv.exists():
        argv.unlink()
    full = {"PATH": f"{work / 'bin'}:{os.environ['PATH']}", "HOME": os.environ["HOME"],
            "FAKEQ_OUT": str(argv), "THYLACINE_BUILD_DIR": str(work / "kernel"),
            "THYLACINE_ACCEL": "hvf", "THYLACINE_NO_AUDIO": "1", "THYLACINE_QEMU_EXTRA": extra}
    r = subprocess.run([str(work / "tools/run-vm.sh"), "--no-share"], env=full,
                       stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    return r.returncode, r.stderr, argv.exists()


def run_block(block, log_text, env, repo_root):
    log = block.parent / "boot.log"
    log.write_text(log_text)
    full = {"PATH": os.environ["PATH"], "HOME": os.environ["HOME"], **env}
    r = subprocess.run(["bash", str(block), str(log), str(repo_root)], env=full,
                       capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout + r.stderr


def skip_line(kind, nth=0):
    return f"[skip] thylacine.{kind}{f' x{nth + 1}' if nth else ''}: the launcher did not declare it"


def boot_log(qemu_line, *skips, counted=None):
    """A boot log; the suite's summary counts the skips unless told otherwise
    (None: as many as given; -1: no summary line at all)."""
    lines = [qemu_line] if qemu_line else []
    for i, s in enumerate(skips):
        lines += [f"    [test] t.{i} ...     {s}", "SKIP"]
    k = len(skips) if counted is None else counted
    if k >= 0:
        lines.append(f"  tests: {100 - k}/100 PASS" + (f" ({k} skipped)" if k else ""))
    return "".join(l + "\r\n" for l in lines)


def main():
    p = argparse.ArgumentParser()
    # Single-quoted: the gate kits find a driver's options by this spelling.
    p.add_argument('--logs', type=Path, required=True)
    a = p.parse_args()
    a.logs.mkdir(parents=True, exist_ok=True)
    src = (ROOT / "tools/test.sh").read_text()
    assert src.count(BLOCK_START) == 1 and src.count(BLOCK_END) == 1
    cut = src[src.index(BLOCK_START):src.index(BLOCK_END)]
    assert "skip_explained" in cut and cut.count("exit 1") == 3, "the cut is not the skip block"
    with tempfile.TemporaryDirectory(prefix="skip-verdict-") as tmp:
        work = Path(tmp)
        (work / "tools").mkdir()
        shutil.copy2(ROOT / "tools/run-vm.sh", work / "tools/run-vm.sh")
        assert (work / "tools/run-vm.sh").read_bytes() == (ROOT / "tools/run-vm.sh").read_bytes()
        (work / "bin").mkdir()
        fake = work / "bin/qemu-system-aarch64"
        # Answers -accel help as a host with every accelerator would, so
        # run-vm.sh keeps the accelerator each configuration asks for.
        fake.write_text('#!/bin/sh\n[ "$1 $2" = "-accel help" ] && { printf "tcg\\nhvf\\nkvm\\n"; exit 0; }\n'
                        'for a in "$@"; do printf "%s\\n" "$a"; done > "$FAKEQ_OUT"\n')
        fake.chmod(0o755)
        (work / "kernel").mkdir()
        (work / "kernel/thylacine.bin").write_bytes(b"\0" * 64)
        disk, pool = work / "disk.img", work / "pool.img"
        disk.write_bytes(b"\0" * 4096)
        pool.write_bytes(b"\0" * 4096)
        block = work / "skip-block.sh"
        block.write_text("#!/bin/bash\nset -euo pipefail\nLOG_FILE=$1\nREPO_ROOT=$2\n" + cut +
                         'echo "skip block: passed"\n')
        report = []

        for name, env, cpus, want in configs(str(disk), str(pool)):
            words, qemu = launch(work, env, cpus)
            assert f"accel={env['THYLACINE_ACCEL']} " in qemu, f"{name}: {qemu}"
            if env["THYLACINE_ACCEL"] == "kvm":
                assert " gic=vhost " in qemu, f"{name}: {qemu}"
            got = collections.Counter(w[len("thylacine."):] for w in words
                                      if w.startswith("thylacine.") and w[len("thylacine."):] in KINDS)
            assert dict(got) == want, f"{name}: run-vm.sh declared {dict(got)}, want {want}"
            for kind in MIRRORED:
                for nth in ((0, 1) if kind == "virtio-pci-input" else (0,)):
                    rc, out = run_block(block, boot_log(qemu, skip_line(kind, nth)), env, ROOT)
                    declared = got[kind] > nth
                    if declared:
                        assert rc == 1 and "the launcher dropped it undeclared" in out, \
                            f"{name}: a skip of declared {kind} x{nth + 1} passed: {out}"
                    else:
                        assert rc == 0 and "NOT coverage" in out and "skip block: passed" in out, \
                            f"{name}: a skip of undeclared {kind} x{nth + 1} was refused: {out}"
            report.append(f"PASS {name} ({qemu[len('==> qemu: '):]}): declares "
                          f"{sorted(got.elements())}; test.sh agrees on every kind")

        for extra in REFUSED:
            rc, err, started = refused(work, extra)
            assert rc == 2 and "attaches a device the launcher cannot declare" in err and not started, \
                f"extras '{extra}': exit {rc}, QEMU started {started}: {err}"
            report.append(f"PASS refused extras '{extra}': exit 2 before QEMU")

        env = {"THYLACINE_NO_NET": "1"}
        qemu = "==> qemu: accel=hvf cpu=host gic=v2 smp=4 its=auto"
        cases = [
            # (label, log, env, want rc, want text)
            ("clean", boot_log(qemu), {}, 0, "skip block: passed"),
            ("gated, explained", boot_log(qemu, skip_line("virtio-net")), env, 0, "1 skip line(s) -- NOT coverage"),
            ("gated, unexplained", boot_log(qemu, skip_line("virtio-net")), {}, 1, "dropped it undeclared"),
            ("ungated", boot_log(qemu, "[skip] ad-hoc"), env, 1, "without a gated helper"),
            ("old shape", boot_log(qemu, "[skip] thylacine.virtio-net: not attached (the launcher did not declare it)"),
             env, 1, "without a gated helper"),
            ("torn", boot_log(qemu, "[skip] thylacine.virtio-netcpu1: x: the launcher did not declare it"),
             env, 1, "without a gated helper"),
            ("unknown kind", boot_log(qemu, skip_line("virtio-foo")), env, 1, "dropped it undeclared"),
            ("one cpu, from the machine line", boot_log(qemu.replace("smp=4", "smp=1"), skip_line("smp")), {}, 0,
             "NOT coverage"),
            ("one cpu, no machine line", boot_log(None, skip_line("smp")), {}, 1, "dropped it undeclared"),
            ("gpu under cocoa", boot_log(qemu, skip_line("virtio-gpu")), {"THYLACINE_DISPLAY": "cocoa"}, 1,
             "dropped it undeclared"),
            ("gpu under vnc", boot_log(qemu, skip_line("virtio-gpu")), {"THYLACINE_DISPLAY": "vnc:0"}, 0,
             "NOT coverage"),
            ("probe, image with joey", boot_log(qemu, "[skip] /bin/pipe-probe not in ramfs, and neither is /bin/joey"),
             {}, 1, "dropped it undeclared"),
            ("tally agrees", boot_log(qemu, skip_line("virtio-net"), counted=1), env, 0, "NOT coverage"),
            ("tally counts more", boot_log(qemu, skip_line("virtio-net"), counted=2), env, 1,
             "counted 2 skip(s), but 1 [skip] line(s)"),
            ("marker torn", boot_log(qemu, "[sk", counted=1).replace("[sk\r\n", "[skcpu1 ip] thylacine.virtio-net: "
                                                                    "the launcher did not declare it\r\n"),
             env, 1, "counted 1 skip(s), but 0 [skip] line(s)"),
            ("no summary line", boot_log(qemu, skip_line("virtio-net"), counted=-1), env, 1,
             "counted 0 skip(s), but 1 [skip] line(s)"),
        ]
        for label, log, env, want_rc, want in cases:
            rc, out = run_block(block, log, env, ROOT)
            assert rc == want_rc and want in out, f"{label}: exit {rc}, want {want_rc} with '{want}': {out}"
            report.append(f"PASS {label}: exit {rc}")
        # The dlopen prover's companion, one variable away: a tree whose sysroot
        # has libc.so must ship the prover; one without it may skip.
        prover = boot_log(qemu, "[skip] /bin/pouch-hello-dlopen not in ramfs, and neither is /lib/libc.so")
        bare = work / "bare-root"
        bare.mkdir()
        rc, out = run_block(block, prover, {}, bare)
        assert rc == 0 and "NOT coverage" in out, f"prover, no sysroot libc.so: {out}"
        (bare / "build/sysroot/lib").mkdir(parents=True)
        (bare / "build/sysroot/lib/libc.so").write_bytes(b"\0")
        rc, out = run_block(block, prover, {}, bare)
        assert rc == 1 and "dropped it undeclared" in out, f"prover, sysroot libc.so present: {out}"
        report.append("PASS prover: skips without the sysroot's libc.so, refused with it")

    (a.logs / "report.txt").write_text("\n".join(report) + "\n")
    print("\n".join(report))
    print(f"test-skip-verdict: ALL {len(report)} PASS")


if __name__ == "__main__":
    main()
