#!/usr/bin/env bash
# Wake this agent when a shared machine it waits for becomes its own.
#
#   thyla-wake.sh hold <res> <reason> [--for 2h] [--wait 4h] [--say <text>]
#                         queue for <res>; once the lease is HELD, type a wake line into this pane
#   thyla-wake.sh watch <res> [--say <text>]
#                         claim nothing; wake when <res> is FREE or held by this agent
#   thyla-wake.sh status  armed watchers, and the tail of the log
#   thyla-wake.sh cancel  stop every armed watcher (a HELD lease is NOT released,
#                         a queued request is NOT cancelled -- those are yip's verbs)
#   thyla-wake.sh probe [pane]   what a watcher would see: program, identity, input box
#
# Nothing here is specific to one repository: any agent in a tmux pane on a yip
# line can arm it. The input-box test reads Claude Code's prompt (THYLA_WAKE_GLYPH
# below the THYLA_WAKE_RULE edge); another client sets its own pair, and a client
# whose box is never recognised is never typed into.
#
# WHY TMUX: an agent whose turn has ended runs again only when something submits
# input. A harness background task can re-invoke it, but the harness also stops
# its own tasks, and compaction tears the session down under them. A line typed
# into the agent's pane is input like any other: it starts a turn when the agent
# is idle, and arrives as a mid-turn message when it is not. tmux hands every
# process its own pane in $TMUX_PANE, so the address needs no naming scheme.
#
# WHY `hold` CLAIMS INSTEAD OF ONLY WATCHING: yip gives an available queue head a
# two-minute offer window. A wake that lands during a ten-minute tool call is
# read after the window has closed, and the offer expires. So `hold` runs the
# blocking `yip hold` itself -- detached, so it outlives the turn and the
# compaction -- and the wake reports a lease already taken. FIFO places survive a
# re-issued wait, so arming this beside an existing waiter keeps the place.
#
# THE PANE IS TYPED INTO ONLY WHEN IT IS SAFE TO BE:
#   * it still runs the program it ran when armed. If the agent exited, the pane
#     is a shell, and a shell would EXECUTE the line;
#   * the input box is visible and empty. A permission dialog draws `❯ 1. Yes`
#     where the box was, and keystrokes there answer it; an operator's half-typed
#     line would be submitted with the wake glued to it. Either way this waits,
#     flashing the tmux status line instead, and types once the box is empty.
# If the agent is gone, nobody can be told about the lease, so a lease taken by
# `hold` is RELEASED rather than left to block every peer for its whole TTL.
set -uo pipefail

YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
DIR="${THYLA_WAKE_DIR:-$HOME/.claude/thyla-wake}"
POLL="${THYLA_WAKE_POLL:-20}"                    # watch: seconds between resource reads
BOUND="${THYLA_WAKE_BOUND:-86400}"               # watch: give up after this
DELIVER_BOUND="${THYLA_WAKE_DELIVER_BOUND:-3600}" # stop trying to type after this
TAILN="${THYLA_WAKE_TAIL:-15}"
GLYPH="${THYLA_WAKE_GLYPH:-$'\xe2\x9d\xaf'}"     # ❯, the input box's prompt
RULE="${THYLA_WAKE_RULE:-$'\xe2\x94\x80'}"       # ─, the box's top edge
NBSP=$'\xc2\xa0'

die() { echo "thyla-wake: $*" >&2; exit 2; }
stamp() { date -u +%H:%MZ; }

log() {  # $1 event, $2 detail
    mkdir -p "$DIR" 2>/dev/null
    printf '%s\t%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$$" "${RES:-?}" "$1" "${2:-}" \
        >> "$DIR/log.tsv" 2>/dev/null
}

usage() { sed -n '3,11p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

me() { "$YIP" presence 2>/dev/null | sed -n 's/^\([A-Za-z0-9_.-]*\) (you):.*/\1/p' | head -1; }

# The resource's own line of `yip resources`, e.g. "mac   HELD by main for 37m, 1.4h left".
res_line() { "$YIP" resources 2>/dev/null | grep -E "^$1 " | head -1; }

# yip names the asking agent's own lease "HELD by you", a peer's by name.
held_by_me() { printf '%s' "$1" | grep -qE "HELD by (you|$ME)( |,|$)"; }

# A pane id that no longer exists is NOT an error to tmux: display-message
# answers it with exit 0 and empty fields. So the pane is alive only when it
# hands back its own id.
pane_alive() { [ "$(tmux display-message -p -t "$PANE" '#{pane_id}' 2>/dev/null)" = "$PANE" ]; }

# empty | typed | none -- the state of the agent's input box. The box is the LAST
# line opening with the prompt glyph whose line above opens with the rule; the
# rule test keeps a dialog's `❯ 1. Yes` cursor (no rule above it) from passing.
# Trailing blank rows are dropped first: on a pane taller than its content they
# would fill the whole tail window and push the box out of it.
input_box() {
    pane_alive || { echo none; return; }
    LC_ALL=C tmux capture-pane -p -t "$PANE" 2>/dev/null \
        | LC_ALL=C sed "s/$NBSP/ /g" \
        | LC_ALL=C awk -v g="$GLYPH" -v r="$RULE" -v n="$TAILN" '
            { line[NR] = $0 }
            END {
                last = NR; while (last > 0 && line[last] ~ /^[ \t]*$/) last--
                first = last - n + 1; if (first < 2) first = 2
                for (i = first; i <= last; i++)
                    if (index(line[i], g) == 1 && index(line[i-1], r) == 1) {
                        rest = substr(line[i], length(g) + 1); gsub(/[ \t]/, "", rest)
                        st = (rest == "") ? "empty" : "typed"
                    }
                print (st == "" ? "none" : st)
            }'
}

# Type "$1" into the pane once it is safe; 0 = typed, 1 = the agent is gone, 2 = never safe.
deliver() {
    local msg="[thyla-wake] $1${SAY:+ -- $SAY}" t0 cur box last_flash=0 now
    t0=$(date +%s)
    while :; do
        pane_alive || { log agent-gone "pane $PANE no longer exists"; return 1; }
        cur=$(tmux display-message -p -t "$PANE" '#{pane_current_command}' 2>/dev/null)
        [ "$cur" = "$PROG" ] || { log agent-gone "pane $PANE now runs '$cur', not '$PROG'"; return 1; }
        box=$(input_box)
        if [ "$box" = empty ]; then
            tmux send-keys -t "$PANE" -l -- "$msg"
            sleep 0.3
            tmux send-keys -t "$PANE" C-m
            sleep 2
            log delivered "box after: $(input_box); $msg"
            return 0
        fi
        now=$(date +%s)
        if [ $((now - last_flash)) -ge 60 ]; then
            tmux display-message -t "$PANE" -d 15000 "thyla-wake: $1 (waiting for an empty input box: $box)" 2>/dev/null
            log waiting "input box: $box"
            last_flash=$now
        fi
        [ $((now - t0)) -ge "$DELIVER_BOUND" ] && { log undelivered "box never empty in ${DELIVER_BOUND}s: $msg"; return 2; }
        sleep 3
    done
}

run_hold() {
    local hp hrc line out="$DIR/$$.hold"
    trap '[ -n "${hp:-}" ] && kill "$hp" 2>/dev/null; log cancelled "TERM while waiting"; rm -f "$DIR/$$.meta"; exit 143' TERM INT
    log armed "hold pane=$PANE prog=$PROG me=$ME for=$FOR wait=$WAIT"
    "$YIP" hold "$RES" "$REASON" --for "$FOR" --wait "$WAIT" > "$out" 2>&1 &
    hp=$!
    wait "$hp"; hrc=$?; hp=""
    line=$(res_line "$RES")
    log hold-returned "rc=$hrc; $(head -1 "$out" 2>/dev/null); $line"
    if held_by_me "$line"; then
        deliver "$RES is yours: HELD by $ME since $(stamp) (yip hold rc=$hrc). Set lease_update phase+pids for what you start; release the moment the cores free." \
            || { [ $? -eq 1 ] && "$YIP" release "$RES" >/dev/null 2>&1 && log released "nobody left to tell about the lease"; }
    else
        deliver "$RES hold ended WITHOUT the lease at $(stamp) (yip hold rc=$hrc: $(head -1 "$out" 2>/dev/null | cut -c1-160)). Now: ${line:-no $RES line}. Re-arm if still wanted."
    fi
    rm -f "$DIR/$$.meta"
}

run_watch() {
    local t0 line
    trap 'log cancelled "TERM while watching"; rm -f "$DIR/$$.meta"; exit 143' TERM INT
    log armed "watch pane=$PANE prog=$PROG me=$ME"
    t0=$(date +%s)
    while :; do
        line=$(res_line "$RES")
        if printf '%s' "$line" | grep -qE "^$RES +FREE" || held_by_me "$line"; then
            deliver "$RES changed at $(stamp): $line"
            break
        fi
        [ $(( $(date +%s) - t0 )) -ge "$BOUND" ] && { log watch-timeout "no change in ${BOUND}s"; break; }
        sleep "$POLL"
    done
    rm -f "$DIR/$$.meta"
}

status() {
    local m pid found=0
    for m in "$DIR"/*.meta; do
        [ -e "$m" ] || continue
        pid=$(basename "$m" .meta)
        if kill -0 "$pid" 2>/dev/null; then echo "  armed pid=$pid  $(cat "$m")"; found=1
        else echo "  stale pid=$pid (dead)  $(cat "$m")"; rm -f "$m"; fi
    done
    [ "$found" = 1 ] || echo "  nothing armed"
    echo "== log (last 8) =="
    tail -n 8 "$DIR/log.tsv" 2>/dev/null || echo "  (empty)"
}

cancel() {
    local m pid n=0
    for m in "$DIR"/*.meta; do
        [ -e "$m" ] || continue
        pid=$(basename "$m" .meta)
        kill -0 "$pid" 2>/dev/null && kill "$pid" && { echo "  cancelled pid=$pid"; n=$((n + 1)); }
        rm -f "$m"
    done
    echo "thyla-wake: $n watcher(s) stopped"
}

# Arm: validate here, where the caller sees the error, then re-exec detached.
arm() {
    local mode="$1"; shift
    [ -n "${TMUX_PANE:-}" ] || die "not inside tmux -- there is no pane to type into"
    command -v tmux >/dev/null || die "tmux is not installed"
    [ -x "$YIP" ] || die "no yip at $YIP"
    RES="${1:-}"; [ -n "$RES" ] || usage 2; shift
    REASON=""; FOR="2h"; WAIT="4h"; SAY=""
    if [ "$mode" = hold ]; then REASON="${1:-}"; [ -n "$REASON" ] || die "hold needs a reason"; shift; fi
    while [ $# -gt 0 ]; do
        case "$1" in
            --for)  FOR="${2:?--for needs a value}"; shift 2 ;;
            --wait) WAIT="${2:?--wait needs a value}"; shift 2 ;;
            --say)  SAY="${2:?--say needs a value}"; shift 2 ;;
            *) die "unknown option '$1'" ;;
        esac
    done
    PANE="$TMUX_PANE"
    pane_alive || die "pane $PANE does not exist"
    PROG=$(tmux display-message -p -t "$PANE" '#{pane_current_command}' 2>/dev/null)
    case "$PROG" in
        bash|zsh|sh|dash|fish|tcsh|csh|ksh|"") die "pane $PANE runs '$PROG', a shell -- a wake line would be executed, not read" ;;
    esac
    ME=$(me); [ -n "$ME" ] || die "yip presence names no '(you)' line"
    [ -n "$(res_line "$RES")" ] || die "yip resources has no '$RES' line"
    mkdir -p "$DIR" || die "cannot create $DIR"
    export PANE PROG ME RES REASON FOR WAIT SAY
    nohup "$0" "_run_$mode" > /dev/null 2>&1 < /dev/null &
    local pid=$!
    printf 'mode=%s res=%s pane=%s prog=%s me=%s armed=%s%s\n' "$mode" "$RES" "$PANE" "$PROG" "$ME" "$(stamp)" \
        "${SAY:+ say=$SAY}" > "$DIR/$pid.meta"
    sleep 1
    kill -0 "$pid" 2>/dev/null || die "the watcher died at once (pid $pid) -- read $DIR/log.tsv"
    echo "thyla-wake: armed $mode on $RES for pane $PANE ($PROG) as $ME -- pid $pid"
}

probe() {
    PANE="${1:-${TMUX_PANE:-}}"; [ -n "$PANE" ] || die "no pane given and not inside tmux"
    echo "pane    : $PANE"
    if pane_alive; then echo "program : $(tmux display-message -p -t "$PANE" '#{pane_current_command}')"
    else echo "program : <no such pane>"; fi
    echo "me      : $(me)"
    echo "box     : $(input_box)"
    "$YIP" resources 2>/dev/null | grep -E '^[a-z]+ ' | sed 's/^/yip     : /'
}

case "${1:-help}" in
    -h|--help|help) usage 0 ;;
    hold|watch)     m="$1"; shift; arm "$m" "$@" ;;
    status)         status ;;
    cancel)         cancel ;;
    probe)          shift; probe "$@" ;;
    _run_hold)      run_hold ;;
    _run_watch)     run_watch ;;
    *)              die "unknown command '$1' (want: hold | watch | status | cancel | help)" ;;
esac
