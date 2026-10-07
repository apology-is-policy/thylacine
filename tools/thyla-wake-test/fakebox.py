# usage: fakebox.py <out-file> <empty|draft|dialog|placeholder> [exit_after_s]
# Draws a Claude-Code-shaped input box in a tmux pane and appends every line typed into it to <out-file>.
#   empty  : an empty box from the start
#   placeholder : an empty box showing a DIM suggestion, the way Claude Code draws one (2026-10-07)
#   draft  : a box holding an operator's half-typed line for 12 s, then an empty box
#   dialog : a permission dialog (a `❯ 1. Yes` cursor, no box edge above it); never an empty box
import sys, time, os, select

out, mode = sys.argv[1], sys.argv[2]
exit_after = float(sys.argv[3]) if len(sys.argv) > 3 else 0
RULE = '─' * 40
t0 = time.time()

def box(text=''):
    if mode == 'placeholder' and not text:
        text = '\033[2mkeep going\033[0m'
    sys.stdout.write('\033[2J\033[H' + 'fakebox %s\n\n%s\n❯ %s\n%s\n  ? for shortcuts\n' % (mode, RULE, text, RULE))
    sys.stdout.flush()

if mode == 'dialog':
    sys.stdout.write('\033[2J\033[HDo you want to proceed?\n❯ 1. Yes\n  2. No\n'); sys.stdout.flush()
elif mode == 'draft':
    box('hello draft'); time.sleep(12); box()
else:
    box()
while True:
    if exit_after and time.time() - t0 > exit_after: sys.exit(0)
    r, _, _ = select.select([sys.stdin], [], [], 0.5)
    if r:
        line = sys.stdin.readline()
        if not line: sys.exit(0)
        with open(out, 'a') as f: f.write(line)
        if mode != 'dialog': box()
