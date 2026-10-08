#!/bin/bash
# A stand-in yip for thyla-wake.sh's controls: canned answers from files in $FAKEYIP_STATE, every
# hold and release recorded in $FAKEYIP_STATE/calls. Never touches a real lease.
S=${FAKEYIP_STATE:?}
case "$1" in
    presence)  echo "aux (you): LIVE, last contact 0s ago" ;;
    resources) cat "$S/resources" ;;
    hold)      echo "hold ${*:2}" >> "$S/calls"; sleep "$(cat "$S/hold.sleep" 2>/dev/null || echo 1)"
               cat "$S/hold.out"; exit "$(cat "$S/hold.rc")" ;;
    release)   echo "release ${*:2}" >> "$S/calls"; echo "released" ;;
    *)         echo "fakeyip: unknown $1" >&2; exit 2 ;;
esac
