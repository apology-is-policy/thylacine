#!/bin/sh
# Drive pi-smp.sh's judge from REAL boot logs (the 17:09Z gate's), green and
# then one variable away in each red direction. Run from the checkout root:
#   sh work/oct19-pi/judge-selftest.sh
set -u
R=/Users/northkillpd/projects/thylacine-corona
G=$R/work/oct19-1709z/gate-20261019T074139Z
S=${TMPDIR:-/tmp}/pi-judge-fx.$$
fixture() {
  rm -rf "$S"; mkdir -p "$S/build/multiboot-logs"
  for i in 1 2; do
    cp "$G/control-boot.log" "$S/build/multiboot-logs/pi-default-smp4-$i.log"
    cp "$G/ubsan-boot.log" "$S/build/multiboot-logs/pi-ubsan-smp4-$i.log"
  done
  for l in pi-default-smp4 pi-ubsan-smp4; do
    echo 0 > "$S/$l.rc"
    echo "== $l: 2 PASS / 0 CORRUPTION / 0 external-kill / 0 inject-miss / 0 timing / 0 other  (N=2) ==" > "$S/$l.out"
  done
}
verdict() { PI_N=2 sh work/oct19-pi/pi-smp.sh judge "$S" 1899 > "$S.last" 2>&1; echo $?; }
expect() { # expect <want rc> <case> [the message that must have fired]
  got=$(verdict)
  why=ok
  [ "$got" = "$1" ] || why="rc $got, want $1"
  if [ -n "${3:-}" ] && ! /usr/bin/grep -qF "$3" "$S.last"; then why="no '$3' in the output"; fi
  if [ "$why" = ok ]; then echo "ok   $2 (rc $got${3:+, fired: $3})"; else echo "BAD  $2: $why"; sed 's/^/     /' "$S.last"; fi
}
fixture; expect 0 "green: four real boot logs"
fixture; echo 'EXTINCTION: selftest' >> "$S/build/multiboot-logs/pi-ubsan-smp4-2.log"; expect 1 "an EXTINCTION line in one boot" "pi-ubsan-smp4 boot 2: RED -- EXTINCTION: selftest"
fixture; sed -i '' 's/tests: 1899\/1899/tests: 1898\/1899/' "$S/build/multiboot-logs/pi-default-smp4-1.log"; expect 1 "one boot's tally 1898/1899" "pi-default-smp4 boot 1: RED -- tally '1898 1899' != 1899/1899"
fixture; rm "$S/build/multiboot-logs/pi-default-smp4-2.log"; expect 1 "a missing boot log" "pi-default-smp4 boot 2: NO LOG"
fixture; echo 1 > "$S/pi-ubsan-smp4.rc"; expect 1 "a config's rc 1" "pi-ubsan-smp4: smp-multiboot rc 1"
fixture; /usr/bin/grep -v 'D7 overlapping login probe PASS' "$G/control-boot.log" > "$S/build/multiboot-logs/pi-default-smp4-1.log"; expect 1 "D7 absent from one boot" "pi-default-smp4 boot 1: RED -- D7 line not exactly once"
fixture; : > "$S/pi-default-smp4.out"; expect 1 "no summary line" "pi-default-smp4: smp-multiboot rc 0; NO SUMMARY LINE"
fixture; cat "$G/control-boot.log" >> "$S/build/multiboot-logs/pi-default-smp4-1.log"; expect 1 "D7 twice (a doubled log)" "pi-default-smp4 boot 1: RED -- D7 line not exactly once"
fixture; expect 0 "green again after the red cases"
rm -rf "$S" "$S.last"
