#!/bin/bash
# A stand-in yip for thyla-wake.sh's controls: canned answers from files in $FAKEYIP_STATE, every
# hold, release and call recorded in $FAKEYIP_STATE/calls (a call's body in call.body). Never
# touches a real lease, and rings nobody.
S=${FAKEYIP_STATE:?}
case "$1" in
    presence)  echo "aux (you): LIVE, last contact 0s ago" ;;
    resources) cat "$S/resources" ;;
    hold)      echo "hold ${*:2}" >> "$S/calls"; sleep "$(cat "$S/hold.sleep" 2>/dev/null || echo 1)"
               cat "$S/hold.out"; exit "$(cat "$S/hold.rc")" ;;
    release)   echo "release ${*:2}" >> "$S/calls"; echo "released" ;;
    call)      echo "call ${*:2}" >> "$S/calls"; cat > "$S/call.body"; exit "$(cat "$S/call.rc" 2>/dev/null || echo 0)" ;;
    *)         echo "fakeyip: unknown $1" >&2; exit 2 ;;
esac
