# usage: fixtures.py <dir> -- writes the input-box parser fixtures as <name>.<want>.txt (raw escapes).
# f1 is the box as measured in an idle Claude Code pane on 2026-10-07: the suggestion is drawn DIM.
import sys
d = sys.argv[1]
E = '\033'; G = '❯'; NB = ' '
R = E + '[38;5;167m' + '─' * 20 + E + '[39m'
ST = E + '[39m  ' + E + '[36mOpus 5.5' + E + '[38;5;246m ctx 305k'
fx = {
    'f1_measured.empty':   [R, E + '[39m' + G + NB + E + '[2mkeep going' + E + '[0m', R, ST],
    'f2_notdim.typed':     [R, E + '[39m' + G + NB + 'keep going', R, ST],
    'f3_carried.empty':    [E + '[2m' + '─' * 20, G + NB + 'keep going' + E + '[0m', R, ST],
    'f4_truecolor.typed':  [R, G + NB + E + '[38;2;2;2;200mhello' + E + '[0m', R, ST],
    'f5_index2.typed':     [R, G + NB + E + '[38;5;2mhello' + E + '[0m', R, ST],
    'f6_partdim.typed':    [R, G + NB + E + '[2mkeep' + E + '[22m going', R, ST],
    'f7_empty.empty':      [R, E + '[39m' + G + NB, R, ST],
    'f8_dialog.none':      ['Do you want to proceed?', G + ' 1. Yes', '  2. No'],
}
for k, v in fx.items():
    open('%s/%s.txt' % (d, k), 'w').write('\n'.join(v) + '\n')
