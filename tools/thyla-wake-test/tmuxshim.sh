#!/bin/bash
# A stand-in tmux for the parser controls: pane %7 is alive and runs 2.1.291; capture-pane replays
# $SHIM_CAPTURE byte for byte (escapes included), so a measured screen can be judged exactly.
case " $* " in
    *" display-message "*'#{pane_id}'*) echo "%7" ;;
    *" display-message "*) echo "2.1.291" ;;
    *" capture-pane "*) cat "$SHIM_CAPTURE" ;;
esac
