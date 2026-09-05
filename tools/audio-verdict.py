#!/usr/bin/env python3
"""tools/audio-verdict.py -- the Nocturne wav witness (docs/NOCTURNE.md section 7, W-1).

QEMU's `wav` audio backend records what the guest played. The N-1 tone probe
(usr/nocturne-probe) writes 0.5 s of 1 kHz, then 0.5 s of 2 kHz, then silence.
This tool decides, from the FILE alone (never from a guest log line -- #186),
whether that signature is present:

  * the first active span is dominated by 1 kHz
  * the second by 2 kHz                               (the positive control:
                                                        a different tone lands
                                                        in a different bin)
  * 1 kHz precedes 2 kHz, in ONE contiguous span
  * the capture ENDS silent                           (the negative control:
                                                        an empty FIFO yields
                                                        silence, never a
                                                        repeated buffer or noise)

QEMU's wav backend appends only while the guest's stream runs, so the file
BEGINS with the first period the guest played -- there is no silent prefix to
check, and the RIFF/data sizes in the header stay 0 unless QEMU exits cleanly
(the harness kills it). The reader below therefore ignores the header sizes
and takes every frame after the `data` chunk header.

Stdlib only (wave + math; a Goertzel per bin per window -- no numpy), so it
runs wherever the gates run. `--selftest` synthesizes the signature in memory
and ALSO proves discrimination: the reversed order, a silent file, and a
single-tone file must all FAIL. A verdict that cannot fail proves nothing.

Exit 0 = PASS, 1 = FAIL (the reason on stdout), 2 = usage/unreadable file.
"""
import argparse
import math
import struct
import sys

WINDOW_S = 0.02          # 20 ms analysis windows
FLOOR_DBFS = -60.0       # absolute silence floor for the pre-tone region
ACTIVE_DBFS = -30.0      # a window this loud is "tone present" (-12 dBFS sine ~ -15 dBFS RMS)
DOMINANCE = 10.0         # the expected bin must carry 10x the power of every control bin
MIN_ACTIVE_WINDOWS = 15  # 0.3 s of each tone (the probe writes 0.5 s each)
MIN_TAIL_WINDOWS = 10    # >= 0.2 s of silence AFTER the last tone (the probe's tail + the idle stop)
MAX_GAP_FRACTION = 0.10  # inactive windows tolerated inside the tone span
MIN_CHORD_WINDOWS = 15   # windows that must carry BOTH tones at once (mixing proof)
CONTROL_BINS = (500.0, 1500.0, 3000.0, 4000.0)


def read_wav(path):
    """A lenient RIFF/WAVE reader: header sizes are ignored (QEMU patches them
    only on a clean exit); the data runs from the `data` chunk header to EOF."""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 44 or d[0:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    fmt = None
    data_off = None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        csz = struct.unpack("<I", d[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            fmt = d[pos + 8:pos + 8 + max(16, csz)]
            pos += 8 + max(16, csz) + (csz & 1)
        elif cid == b"data":
            data_off = pos + 8
            break
        else:
            if csz == 0:
                raise ValueError("unexpected chunk %r with size 0" % cid)
            pos += 8 + csz + (csz & 1)
    if fmt is None or data_off is None or len(fmt) < 16:
        raise ValueError("no fmt/data chunk")
    tag, ch, rate, _brate, _balign, bits = struct.unpack("<HHIIHH", fmt[:16])
    if tag != 1 or bits != 16:
        raise ValueError("expected PCM 16-bit, got tag %d bits %d" % (tag, bits))
    sw = 2
    raw = d[data_off:]
    count = len(raw) // 2
    samples = struct.unpack("<%dh" % count, raw[: count * 2])
    if ch > 1:
        mono = [sum(samples[i:i + ch]) / (ch * 32768.0) for i in range(0, count - ch + 1, ch)]
    else:
        mono = [s / 32768.0 for s in samples]
    return rate, mono


def goertzel(x, rate, freq):
    n = len(x)
    k = int(0.5 + n * freq / rate)
    w = 2.0 * math.pi * k / n
    c = 2.0 * math.cos(w)
    s0 = s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2 = s1
        s1 = s0
    return s1 * s1 + s2 * s2 - c * s1 * s2


def dbfs(rms):
    return -200.0 if rms <= 0 else 20.0 * math.log10(rms)


def analyze(rate, mono, expect):
    win = max(16, int(rate * WINDOW_S))
    nwin = len(mono) // win
    if nwin < 2 * MIN_ACTIVE_WINDOWS:
        return False, "capture too short: %d windows of %d ms" % (nwin, WINDOW_S * 1000)
    rms = []
    dom = []  # per window: which expected freq dominates (index) or None
    bins = list(expect) + [b for b in CONTROL_BINS if b not in expect]
    for i in range(nwin):
        x = mono[i * win:(i + 1) * win]
        r = math.sqrt(sum(v * v for v in x) / len(x))
        rms.append(r)
        if dbfs(r) < ACTIVE_DBFS:
            dom.append(None)
            continue
        p = [goertzel(x, rate, f) for f in bins]
        best = max(range(len(expect)), key=lambda j: p[j])
        others = [p[j] for j in range(len(bins)) if j != best]
        dom.append(best if all(p[best] > DOMINANCE * o for o in others) else -1)
    active = [i for i, d in enumerate(dom) if d is not None]
    if not active:
        return False, "no window above %g dBFS: nothing was played" % ACTIVE_DBFS
    first, last = active[0], active[-1]
    # The negative control: the capture must END silent -- the probe's 0.2 s
    # tail plus the driver's idle-stop silence. A driver that repeats its last
    # buffer, or feeds noise, when the FIFO runs dry fails here.
    tail = nwin - 1 - last
    if tail < MIN_TAIL_WINDOWS:
        return False, "only %d silent windows after the last tone (< %d): no silent tail" % (tail, MIN_TAIL_WINDOWS)
    loud_outside = [i for i in list(range(first)) + list(range(last + 1, nwin)) if dbfs(rms[i]) > FLOOR_DBFS]
    if len(loud_outside) > max(2, (nwin - (last - first + 1)) // 20):
        return False, "%d windows outside the tone span above %g dBFS (not silent)" % (len(loud_outside), FLOOR_DBFS)
    # The tones are one contiguous span: a gap inside it is an underrun (or a
    # different signal), not the probe's signature.
    span = last - first + 1
    gaps = sum(1 for i in range(first, last + 1) if dom[i] is None)
    if gaps > MAX_GAP_FRACTION * span:
        return False, "%d silent windows inside the %d-window tone span (an underrun?)" % (gaps, span)
    counts = [sum(1 for d in dom if d == j) for j in range(len(expect))]
    ambiguous = sum(1 for d in dom if d == -1)
    for j, f in enumerate(expect):
        if counts[j] < MIN_ACTIVE_WINDOWS:
            return False, "only %d windows dominated by %g Hz (need %d); ambiguous=%d" % (counts[j], f, MIN_ACTIVE_WINDOWS, ambiguous)
    # Order: the median index of each tone must be increasing.
    medians = []
    for j in range(len(expect)):
        idx = [i for i, d in enumerate(dom) if d == j]
        medians.append(idx[len(idx) // 2])
    for j in range(1, len(expect)):
        if medians[j] <= medians[j - 1]:
            return False, "order wrong: %g Hz (median window %d) does not follow %g Hz (%d)" % (
                expect[j], medians[j], expect[j - 1], medians[j - 1])
    summary = "PASS: %s; silent tail %d windows (prefix %d); ambiguous %d; rate %d; %d windows total" % (
        ", ".join("%g Hz x %d windows (median %d)" % (expect[j], counts[j], medians[j]) for j in range(len(expect))),
        tail, first, ambiguous, rate, nwin)
    return True, summary


def analyze_chord(rate, mono, expect):
    """Mixing witness: BOTH `expect` tones must be present in the SAME windows.
    Each expected bin must carry DOMINANCE x the power of every control bin, in
    the same 20 ms window, for >= MIN_CHORD_WINDOWS windows. A SEQUENTIAL
    capture -- each tone alone in its own windows, the N-1 signature -- has no
    window with both, so it FAILS here: that is the control proving this checks
    simultaneity (mixing), not mere presence."""
    win = max(16, int(rate * WINDOW_S))
    nwin = len(mono) // win
    if nwin < MIN_CHORD_WINDOWS + MIN_TAIL_WINDOWS:
        return False, "capture too short: %d windows of %d ms" % (nwin, WINDOW_S * 1000)
    ctrl = [b for b in CONTROL_BINS if b not in expect]
    rms = []
    active = []   # RMS above the active floor
    chord = []    # every expected bin dominates every control bin, same window
    for i in range(nwin):
        x = mono[i * win:(i + 1) * win]
        r = math.sqrt(sum(v * v for v in x) / len(x))
        rms.append(r)
        if dbfs(r) < ACTIVE_DBFS:
            active.append(False)
            chord.append(False)
            continue
        active.append(True)
        pe = [goertzel(x, rate, f) for f in expect]
        pc = [goertzel(x, rate, f) for f in ctrl]
        cmax = max(pc) if pc else 0.0
        chord.append(all(p > DOMINANCE * cmax for p in pe))
    act = [i for i, a in enumerate(active) if a]
    if not act:
        return False, "no window above %g dBFS: nothing was played" % ACTIVE_DBFS
    first, last = act[0], act[-1]
    tail = nwin - 1 - last
    if tail < MIN_TAIL_WINDOWS:
        return False, "only %d silent windows after the last tone (< %d): no silent tail" % (tail, MIN_TAIL_WINDOWS)
    loud_outside = [i for i in list(range(first)) + list(range(last + 1, nwin)) if dbfs(rms[i]) > FLOOR_DBFS]
    if len(loud_outside) > max(2, (nwin - (last - first + 1)) // 20):
        return False, "%d windows outside the active span above %g dBFS (not silent)" % (len(loud_outside), FLOOR_DBFS)
    span = last - first + 1
    gaps = sum(1 for i in range(first, last + 1) if not active[i])
    if gaps > MAX_GAP_FRACTION * span:
        return False, "%d silent windows inside the %d-window span (an underrun?)" % (gaps, span)
    nchord = sum(1 for c in chord if c)
    if nchord < MIN_CHORD_WINDOWS:
        label = "+".join("%g" % f for f in expect)
        return False, "only %d windows carry %s Hz SIMULTANEOUSLY (need %d): not a mix (sequential or single tone)" % (
            nchord, label, MIN_CHORD_WINDOWS)
    label = "+".join("%g" % f for f in expect)
    summary = "PASS(chord): %d windows carry %s Hz at once; span %d; silent tail %d; rate %d; %d windows total" % (
        nchord, label, span, tail, rate, nwin)
    return True, summary


def synth(rate, plan, amp=0.25, noise=0.0):
    """plan: list of (freq, seconds). freq is None (silence), a number (one
    tone), or a tuple/list of numbers (a chord -- the tones summed). Each
    frequency keeps its own continuous phase across segments so a spliced tone
    never jumps."""
    out = []
    phases = {}
    for f, secs in plan:
        n = int(rate * secs)
        if f is None:
            freqs = ()
        elif isinstance(f, (tuple, list)):
            freqs = tuple(f)
        else:
            freqs = (f,)
        for _ in range(n):
            v = 0.0
            for ff in freqs:
                ph = phases.get(ff, 0.0)
                v += amp * math.sin(ph)
                phases[ff] = ph + 2 * math.pi * ff / rate
            out.append(v)
    return out


def selftest():
    rate = 48000
    cases = [
        ("signature (QEMU shape)", [(1000, 0.5), (2000, 0.5), (None, 0.7)], True),
        ("signature with a prefix", [(None, 1.0), (1000, 0.5), (2000, 0.5), (None, 0.3)], True),
        ("reversed order", [(2000, 0.5), (1000, 0.5), (None, 0.7)], False),
        ("silent", [(None, 2.2)], False),
        ("single tone", [(1000, 1.0), (None, 0.7)], False),
        ("no silent tail", [(1000, 0.5), (2000, 0.5)], False),
        ("noise after the tones", [(1000, 0.5), (2000, 0.5), (None, 0.2), (700, 0.5)], False),
        ("gap inside the span", [(1000, 0.5), (None, 0.3), (2000, 0.5), (None, 0.7)], False),
        ("signature at 44.1 kHz", [(1000, 0.5), (2000, 0.5), (None, 0.7)], True),
    ]
    ok = True
    for name, plan, want in cases:
        r = 44100 if "44.1" in name else rate
        got, why = analyze(r, synth(r, plan), (1000.0, 2000.0))
        verdict = "ok" if got == want else "WRONG"
        if got != want:
            ok = False
        print("selftest %-24s expect %-5s got %-5s %s -- %s" % (name, want, got, verdict, why))
    print("selftest", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def selftest_chord():
    rate = 48000
    C = (1000, 2000)  # the chord the probe plays
    cases = [
        ("chord (mixed)", [(C, 1.2), (None, 0.7)], True),
        ("chord with a prefix", [(None, 0.5), (C, 1.2), (None, 0.4)], True),
        ("sequential (N-1 shape)", [(1000, 0.6), (2000, 0.6), (None, 0.7)], False),
        ("single tone", [(1000, 1.2), (None, 0.7)], False),
        ("silent", [(None, 2.2)], False),
        ("no silent tail", [(C, 1.2)], False),
        ("chord then noise", [(C, 1.0), (None, 0.2), (700, 0.6)], False),
        ("gap inside the chord", [(C, 0.5), (None, 0.4), (C, 0.6), (None, 0.7)], False),
        ("chord at 44.1 kHz", [(C, 1.2), (None, 0.7)], True),
    ]
    ok = True
    for name, plan, want in cases:
        r = 44100 if "44.1" in name else rate
        got, why = analyze_chord(r, synth(r, plan), (1000.0, 2000.0))
        verdict = "ok" if got == want else "WRONG"
        if got != want:
            ok = False
        print("selftest-chord %-22s expect %-5s got %-5s %s -- %s" % (name, want, got, verdict, why))
    print("selftest-chord", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="?", help="the QEMU wav capture")
    ap.add_argument("--expect", default="1000,2000", help="tones, Hz, comma-separated (default 1000,2000)")
    ap.add_argument("--chord", action="store_true",
                    help="require the tones SIMULTANEOUSLY (the mixing witness); default is sequential order")
    ap.add_argument("--selftest", action="store_true", help="prove the verdict discriminates on synthetic signals")
    a = ap.parse_args()
    if a.selftest:
        # Both discriminations must hold; the exit code is their OR of failure.
        rc_seq = selftest()
        rc_chord = selftest_chord()
        return 0 if rc_seq == 0 and rc_chord == 0 else 1
    if not a.wav:
        ap.print_usage()
        return 2
    expect = tuple(float(x) for x in a.expect.split(","))
    try:
        rate, mono = read_wav(a.wav)
    except Exception as e:  # noqa: BLE001
        print("FAIL: cannot read %s: %s" % (a.wav, e))
        return 2
    ok, why = (analyze_chord if a.chord else analyze)(rate, mono, expect)
    print(("PASS " if ok else "FAIL ") + why if not why.startswith(("PASS", "FAIL")) else why)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
