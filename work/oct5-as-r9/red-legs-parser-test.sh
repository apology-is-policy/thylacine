#!/bin/sh
# Tests the red-legs script's verdict/attribution parsers against a REAL serial
# log, with two synthetic verdict lines appended so both outcomes are present.
set -u
RUN=$1

verdict() {
  line=$(grep -E "\[test\] $2 \.\.\. (PASS|FAIL)" "$RUN/$1-serial.log" | tail -1)
  case "${line:-}" in
    *PASS*) echo PASS ;;
    *FAIL*) echo FAIL ;;
    *)      echo ABSENT ;;
  esac
}
failing_set() {
  grep -oE '\[test\] [a-z0-9_.]+ \.\.\. FAIL' "$RUN/$1-serial.log" \
    | sed 's/\[test\] //; s/ \.\.\. FAIL//' | sort -u
}

bad=0
check() { # check <label> <got> <want>
  if [ "$2" = "$3" ]; then printf 'OK    %-22s %s\n' "$1" "$2"
  else printf 'WRONG %-22s got=%s want=%s\n' "$1" "$2" "$3"; bad=1; fi
}

check "interior"    "$(verdict green burrow.unmap_interior_start_refused)" FAIL
check "lifecycle"   "$(verdict green loom.private_owner_lifecycle)"        PASS
check "nonexistent" "$(verdict green no.such.test)"                        ABSENT
check "failing_set" "$(failing_set green | tr '\n' ' ' | sed 's/ $//')" \
      "burrow.unmap_interior_start_refused"

# The tally/skip/extinction predicates the green control uses.
tally=$(grep -E '^ *tests: [0-9]+/[0-9]+ PASS' "$RUN/green-serial.log" | tail -1)
ran=$(printf '%s\n' "$tally" | sed -E 's#.*tests: ([0-9]+)/([0-9]+) PASS.*#\1#')
tot=$(printf '%s\n' "$tally" | sed -E 's#.*tests: ([0-9]+)/([0-9]+) PASS.*#\2#')
check "tally ran"  "$ran" 1834
check "tally tot"  "$tot" 1834
check "skips"      "$(grep -c '\[skip\]' "$RUN/green-serial.log")" 0
check "extinctions" "$(grep -c 'EXTINCTION:' "$RUN/green-serial.log")" 0

exit $bad
