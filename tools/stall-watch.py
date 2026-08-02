#!/usr/bin/env python3
"""tools/stall-watch.py -- #125: a NON-UART observer for a silent guest.

THE POINT: the UART cannot be the instrument when the UART is the suspect.
#125 is a boot that goes silent mid-line with `uart_putc`'s byte-drop signature
and never reaches login. Two live explanations that console output CANNOT
separate, because in both of them the console is exactly what stops:

  H-A  the guest keeps running; every byte is dropped at a full TX FIFO
       (the login prompt WAS printed and lost) -- a dead delivery channel.
  H-B  the guest stopped making progress.

This observer separates them from OUTSIDE the guest, over QMP, touching no
guest code at all. It samples each vCPU's whole register file while the
console is quiet:

  any CPU's registers change -> H-A: the guest is alive; the channel died.
  every CPU frozen           -> H-B: the guest is wedged -- AND WHERE.

LIVENESS IS THE REGISTER DIGEST, NOT THE PC. This is not a stylistic choice;
an earlier draft of this file used the PC and a positive control FALSIFIED it.
Under TCG the visible PC is only synced at TRANSLATION-BLOCK boundaries, so a
guest spinning in a tight loop reports a CONSTANT PC while executing
perfectly. Measured on a 17-instruction `add x0,x0,#1` loop: PC pinned at
0x40080000 across every sample while X00 ran 0x55e46fc0 -> 0x10fd913f0 ->
0x1c0df97a0. A PC-based verdict called that provably-running guest
"H-B: FROZEN, interrupt-dead" -- the exact false conclusion that would send
the next reader hunting a guest wedge that does not exist.

So: the digest decides liveness; the PC is kept only to say WHERE a genuinely
frozen CPU is stopped. `info irq` was evaluated as a third signal and
REJECTED -- it returns empty on arm/virt (the monitor's "if available" is a
real caveat, not boilerplate).

The zero-guest-change property is load-bearing twice over: a guest-side
heartbeat would (a) have to reach the host through some channel, and every
channel is a fresh suspect, and (b) perturb the very timing that produces the
bug. `info registers` reads state QEMU already holds.

WHY NOT `info cpus`: it reports only `thread_id` on aarch64 -- no PC. A
"cheap continuous PC sample" built on it logs nothing but thread IDs and
concludes nothing. `info registers -a` is the only PC source. Verified against
a live QEMU 10.0.2 before this file was written, not assumed.

PSTATE is the second half of the payload, and it is why this beats a memory
counter: bits [9:6] are DAIF, so a frozen PC with I masked is an
INTERRUPT-DEAD SPIN -- the #126 class -- named on sight rather than inferred.

Symbolization is best-effort: the KASLR offset is read from the guest's own
boot banner (which prints long before the stall), subtracted from the sampled
PC, and resolved against the kernel ELF's symbol table. A miss degrades to a
raw address; it never fails the observation.

usage: stall-watch.py --sock SOCK --log LOG [--out OUT] [--elf ELF]
                      [--quiet-s N] [--deadline-s N] [--sample-s N]
"""

import argparse
import bisect
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

CPU_RE = re.compile(r"^CPU#(\d+)", re.M)
PC_RE = re.compile(r"\bPC=([0-9a-fA-F]+)")
PSTATE_RE = re.compile(r"\bPSTATE=([0-9a-fA-F]+)\s*(\S*)\s*(EL\d[ht]?)?")
# The banner line the kernel prints during late bring-up (TOOLING.md sect 10):
#   kernel base: 0xADDR (KASLR offset 0xADDR)
KASLR_RE = re.compile(rb"kernel base:\s*0x([0-9a-fA-F]+)\s*\(KASLR offset\s*0x([0-9a-fA-F]+)")


def now():
    return time.time()


class Log:
    def __init__(self, path):
        self.path = path
        self.fp = open(path, "a", buffering=1) if path else None

    def __call__(self, msg):
        t = now()
        line = "[%s.%03d] %s" % (
            time.strftime("%H:%M:%S", time.localtime(t)), int(t % 1 * 1000), msg)
        print(line, flush=True)
        if self.fp:
            self.fp.write(line + "\n")


class Qmp:
    """Minimal QMP client. Shape borrowed from tools/qmp-inject-key.py: connect,
    drain greeting, negotiate capabilities, then skip async events when reading
    a command response."""

    def __init__(self, sock_path, deadline, log):
        self.s = None
        self.f = None
        self.log = log
        while now() < deadline:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.settimeout(5.0)
                s.connect(sock_path)
                f = s.makefile("rw", buffering=1)
                f.readline()                      # greeting
                f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
                f.flush()
                self.s, self.f = s, f
                self._resp()
                log("qmp: connected")
                return
            except OSError:
                s.close()
                time.sleep(0.1)
        raise SystemExit("stall-watch: QMP socket never became reachable")

    def _resp(self):
        while True:
            line = self.f.readline()
            if not line:
                raise OSError("qmp: connection closed")
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if "return" in obj or "error" in obj:
                return obj

    def cmd(self, obj):
        self.f.write(json.dumps(obj) + "\n")
        self.f.flush()
        return self._resp()

    def hmp(self, cmdline):
        r = self.cmd({"execute": "human-monitor-command",
                      "arguments": {"command-line": cmdline}})
        return r.get("return", "") if r else ""

    def status(self):
        r = self.cmd({"execute": "query-status"})
        return r.get("return", {}) if r else {}


class Symbols:
    """Best-effort kernel symbolization. Sorted link-time addresses + names from
    `nm`; a sampled PC is de-slid by the KASLR offset the guest itself printed,
    then resolved by predecessor search. Any missing piece degrades to a raw
    address -- symbolization is ergonomics, never a gate."""

    def __init__(self, elf, log):
        self.addrs, self.names = [], []
        self.slide = None
        if not elf or not os.path.exists(elf):
            return
        for tool in ("nm", "llvm-nm", "gnm"):
            try:
                out = subprocess.run([tool, "-n", elf], capture_output=True,
                                     text=True, timeout=60)
            except (FileNotFoundError, subprocess.SubprocessError):
                continue
            if out.returncode != 0:
                continue
            pairs = []
            for line in out.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1].lower() in "tw":
                    try:
                        pairs.append((int(parts[0], 16), parts[2]))
                    except ValueError:
                        pass
            if pairs:
                pairs.sort()
                self.addrs = [p[0] for p in pairs]
                self.names = [p[1] for p in pairs]
                log("symbols: %d text symbols from %s" % (len(pairs), tool))
            return

    def resolve(self, pc):
        if not self.addrs or self.slide is None:
            return None
        link = pc - self.slide
        if link < 0:
            return None
        i = bisect.bisect_right(self.addrs, link) - 1
        if i < 0:
            return None
        off = link - self.addrs[i]
        if off > 0x100000:                    # implausibly far into a symbol
            return None
        return "%s+0x%x" % (self.names[i], off)


def parse_registers(text):
    """-> {cpu: {'pc': int, 'pstate': int, 'flags': str, 'el': str, 'digest': str}}.

    QEMU emits a per-CPU block headed `CPU#N`; PC and PSTATE live inside it.
    `digest` covers the WHOLE block (every X register, SP, PSTATE) because that
    -- not the PC -- is what actually moves when a TCG guest is executing."""
    out = {}
    marks = [(m.start(), int(m.group(1))) for m in CPU_RE.finditer(text)]
    for i, (start, cpu) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        blk = text[start:end]
        pc = PC_RE.search(blk)
        ps = PSTATE_RE.search(blk)
        if not pc:
            continue
        e = {"pc": int(pc.group(1), 16),
             "digest": hashlib.sha1(blk.encode()).hexdigest()[:12]}
        if ps:
            e["pstate"] = int(ps.group(1), 16)
            e["flags"] = ps.group(2) or ""
            e["el"] = ps.group(3) or ""
        out[cpu] = e
    return out


def daif_str(pstate):
    """DAIF is PSTATE[9:6]. A frozen PC with I set is an interrupt-dead spin."""
    d = (pstate >> 6) & 0xF
    return "".join(c for c, b in zip("DAIF", (8, 4, 2, 1)) if d & b) or "-"


def fmt_cpus(regs, syms):
    parts = []
    for cpu in sorted(regs):
        e = regs[cpu]
        s = "cpu%d=0x%x" % (cpu, e["pc"])
        sym = syms.resolve(e["pc"])
        if sym:
            s += "(%s)" % sym
        if "pstate" in e:
            s += "[%s %s]" % (daif_str(e["pstate"]), e.get("el", "?"))
        parts.append(s)
    return " ".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", required=True)
    ap.add_argument("--log", required=True, help="the guest console log to watch")
    ap.add_argument("--out", default=None, help="observer log (also to stdout)")
    ap.add_argument("--elf", default=None, help="kernel ELF for symbolization")
    ap.add_argument("--quiet-s", type=float, default=20.0,
                    help="console silence that triggers sampling")
    ap.add_argument("--sample-s", type=float, default=2.0)
    ap.add_argument("--deadline-s", type=float, default=420.0)
    ap.add_argument("--stop-marker", default="Thylacine login:",
                    help="observing ends once this appears in the log")
    a = ap.parse_args()

    log = Log(a.out)
    log("stall-watch: start sock=%s log=%s quiet=%.0fs deadline=%.0fs"
        % (a.sock, a.log, a.quiet_s, a.deadline_s))

    deadline = now() + a.deadline_s
    syms = Symbols(a.elf, log)
    q = Qmp(a.sock, deadline, log)

    size = 0
    last_growth = now()
    quiet_since = None
    prev_pcs = None
    frozen_for = 0
    samples = 0
    saw_stop = False
    banner_done = False

    while now() < deadline:
        # --- console progress (bytes, not lines: a stall can land mid-line) ---
        try:
            st = os.stat(a.log)
            if st.st_size != size:
                if syms.slide is None and not banner_done:
                    with open(a.log, "rb") as fp:
                        m = KASLR_RE.search(fp.read())
                    if m:
                        syms.slide = int(m.group(2), 16)
                        banner_done = True
                        log("symbols: KASLR offset 0x%x (base 0x%s)"
                            % (syms.slide, m.group(1).decode()))
                if not saw_stop and size:
                    with open(a.log, "rb") as fp:
                        fp.seek(max(0, size - 256))
                        if a.stop_marker.encode() in fp.read():
                            saw_stop = True
                            log("stall-watch: stop marker seen; guest reached login")
                size = st.st_size
                last_growth = now()
                if quiet_since is not None:
                    log("console: RESUMED after %.1fs quiet (size=%d)"
                        % (now() - quiet_since, size))
                    quiet_since = None
                    prev_pcs = None
                    frozen_for = 0
        except FileNotFoundError:
            pass

        if saw_stop:
            log("stall-watch: done (guest reached login; %d sample(s) taken)" % samples)
            return 0

        quiet = now() - last_growth
        if quiet < a.quiet_s:
            time.sleep(0.25)
            continue

        # --- the console has gone quiet: observe the guest from outside ---
        if quiet_since is None:
            quiet_since = last_growth
            log("console: QUIET for %.1fs at size=%d -- sampling vCPUs" % (quiet, size))
            try:
                raw = q.hmp("info registers -a")
                log("--- raw info registers -a (first quiet sample) ---\n" + raw.strip())
            except OSError as e:
                log("qmp: raw dump failed: %s" % e)

        try:
            st = q.status()
            regs = parse_registers(q.hmp("info registers -a"))
        except OSError as e:
            log("qmp: sample failed (%s) -- QEMU gone?" % e)
            return 2

        samples += 1
        # Liveness = the register DIGEST, per CPU. A CPU parked in WFI is
        # legitimately frozen, so "the guest is executing" means ANY CPU moved.
        digs = {c: regs[c]["digest"] for c in regs}
        moved = sorted(c for c in digs if prev_pcs is not None
                       and prev_pcs.get(c) not in (None, digs[c]))
        if prev_pcs is not None and digs == prev_pcs:
            frozen_for += 1
        else:
            frozen_for = 0
        prev_pcs = digs

        verdict = ""
        if frozen_for >= 2:
            masked = [c for c in regs
                      if "pstate" in regs[c] and (regs[c]["pstate"] >> 6) & 0x2]
            verdict = ("  <-- H-B: EVERY CPU's register file frozen across %d "
                       "samples (guest not executing)" % (frozen_for + 1))
            if masked:
                verdict += "; IRQ-MASKED on cpu%s (interrupt-dead)" % (
                    ",".join(str(c) for c in sorted(masked)))
        elif moved:
            verdict = ("  <-- H-A: cpu%s EXECUTING (guest alive; the console "
                       "channel is the suspect)"
                       % ",".join(str(c) for c in moved))

        log("sample %d q=%.0fs status=%s %s%s"
            % (samples, quiet, st.get("status", "?"), fmt_cpus(regs, syms), verdict))
        time.sleep(a.sample_s)

    log("stall-watch: deadline reached (%d sample(s) taken)" % samples)
    return 0 if samples == 0 else 3


if __name__ == "__main__":
    sys.exit(main())
