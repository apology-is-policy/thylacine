#!/usr/bin/env bash
# Bring up the three-agent tmux layout.
#
#   thyla-tmux.sh [pair] [MAIN_CONV] [AUX_CONV]   main + aux, one session, two panes
#   thyla-tmux.sh vault  [VAULT_CONV]             the vault, its own session
#   thyla-tmux.sh status                          what is up, without attaching
#
# Defaults: MAIN_CONV=thylacine  AUX_CONV=aux_gfx  VAULT_CONV=knowledge
#
# TWO DIFFERENT THINGS ARE CALLED A "SESSION" HERE AND THEY ARE NOT THE SAME.
# Keeping them apart is the whole reason the names below are spelled out:
#   * a TMUX session  -- the window layout. Fixed: `thylacine` (the pair) and
#     `vault`. It describes where panes live and nothing else.
#   * a CONVERSATION  -- what `claude --resume` takes. THESE are the arguments.
# They coincide for main only (both "thylacine"), which is exactly the kind of
# accident that reads as a rule until the day it doesn't.
#
# WHY --resume <name> AND NOT --continue: `--continue` resumes "the most recent
# conversation in the current directory", so it silently picks whatever was
# touched last in that tree -- which is not necessarily the agent you mean. A
# named resume says which one.
#
# CONVERSATIONS ARE NAMED WITH `/name`, AND THE NAME IS AN IDENTIFIER. This
# session is named "thylacine", so `--resume thylacine` opens that exact
# conversation -- it does not search. That is what makes these names safe to
# pass as arguments and safe to default: they are stable handles the operator
# assigns, not fuzzy matches.
#
# `claude --help` describes `-r, --resume [value]` as "Resume a conversation by
# session ID, or open interactive picker with optional search term", and the
# picker is the FALLBACK -- what you get when the value resolves to no named
# conversation. So the only way to land in a picker is a typo or a name that
# has not been assigned yet. Worth knowing because the liveness check below
# cannot tell a resumed agent from a pane sitting in a picker; on the normal
# path that distinction does not arise, and on a typo the pane shows you.
#
# WHY TMUX AT ALL: an agent inside a pane can address ITSELF -- tmux sets
# $TMUX_PANE to a unique pane id in every process it starts -- so self-directed
# send-keys needs no naming scheme kept in sync between this script and the
# agents. The address is handed to each one by tmux.
#
# LAYOUT: main+aux share a session because they are watched together -- two
# panes, no nesting, no doubled prefix key. The cost is that `kill-session`
# takes out both; use `kill-pane` when you mean one. The vault is separate
# because it is transient, and because its conversation lives under the AUX
# project key while its cwd is the vault worktree (there is no
# ~/.claude/projects/-Users-northkillpd-projects-thylacine-vault).
set -uo pipefail

MAIN_DIR="$HOME/projects/thylacine"
AUX_DIR="$HOME/projects/thylacine-aux"
VAULT_DIR="$HOME/projects/thylacine-vault"

TMUX_PAIR="thylacine"          # tmux session holding main+aux
TMUX_VAULT="vault"             # tmux session holding the vault
WINDOW="agents"

DEF_MAIN_CONV="thylacine"      # conversation names -> claude --resume
DEF_AUX_CONV="aux_gfx"
DEF_VAULT_CONV="knowledge"

die() { echo "thyla-tmux: $*" >&2; exit 2; }

usage() {
    sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

command -v tmux >/dev/null || die "tmux is not installed"

# ENUMERATE CLAUDES WITH ps, NOT pgrep, and the reason is measured rather than
# stylistic. BSD/macOS pgrep EXCLUDES THE CALLING PROCESS AND ALL ITS ANCESTORS
# unless -a is given. This script is normally run from a shell inside an agent's
# own pane, so that agent is an ancestor -- pgrep would omit precisely the one
# whose duplication this guard exists to prevent, i.e. it fails OPEN in the only
# case that matters. Measured with three live claudes: `pgrep claude` returned
# 2, `pgrep -a claude` returned 3. `-a` is not the portable fix either: on Linux
# it means "print the full command line". ps has neither behaviour.
claude_pids() { ps -eo pid=,comm= | awk '{ n = $2; sub(/.*\//, "", n); if (n == "claude") print $1 }'; }

claude_pid_in() {
    local dir="$1" pid
    for pid in $(claude_pids); do
        [ "$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')" = "$dir" ] && { echo "$pid"; return 0; }
    done
    return 1
}

# Start a pane's agent, or explain IN THE PANE why it was not started.
#
# The explanation must land in the pane, not on the script's stdout: the attach
# that follows repaints the screen immediately, so anything echoed here is lost
# and the operator is left with a bare shell and no reason for it. That is
# precisely what the first real run produced -- the guard was working and
# looked like a bug.
#
# When it declines, the command is PRE-TYPED but not executed, so recovering is
# one Enter after quitting the other session rather than remembering the flags.
launch_pane() {
    local target="$1" dir="$2" cmd="$3" label="$4" pid
    if pid=$(claude_pid_in "$dir"); then
        tmux send-keys -t "$target" \
            "clear; echo; echo '  thyla-tmux: did NOT start $label here.'; echo '  A claude is already serving $dir (pid $pid).'; echo '  Quit that one, then press Enter to run the command already typed below.'; echo" C-m
        tmux send-keys -t "$target" "$cmd"      # deliberately no C-m
        echo "  $label: already running (pid $pid) -- pane left with the command pre-typed."
        return 1
    fi
    tmux send-keys -t "$target" "$cmd" C-m
    echo "  $label: started -> $cmd"
    return 0
}

# Did the thing we just launched survive? A named conversation resolves exactly,
# so on the normal path "alive" means "resumed". The one case this cannot see is
# a mistyped name leaving the pane in the picker -- which the pane itself shows.
settled() {
    local dir="$1" label="$2"
    if claude_pid_in "$dir" >/dev/null; then
        echo "  $label: alive"
        return 0
    fi
    echo "  $label: NOT alive after settle -- read the pane, the error is in it." >&2
    return 1
}

start_pair() {
    local main_conv="$1" aux_conv="$2" started=0
    if tmux has-session -t "$TMUX_PAIR" 2>/dev/null; then
        echo "thyla-tmux: tmux session '$TMUX_PAIR' already exists -- attaching, not recreating."
        attach "$TMUX_PAIR"; return
    fi
    for d in "$MAIN_DIR" "$AUX_DIR"; do [ -d "$d" ] || die "missing worktree: $d"; done

    # ADDRESS PANES BY ID, NEVER BY INDEX. `-P -F '#{pane_id}'` makes tmux hand
    # back the id (%0, %1) it just created; ids are absolute, never renumber,
    # and are immune to base-index settings.
    #
    # The first version used `.0` and `.1` and was silently, catastrophically
    # wrong on any host with `pane-base-index 1` -- which is a very common
    # setting and is this one. Measured on the live session: with panes at
    # indices 1 and 2, `.0` -> %0, `.1` -> %0, `.2` -> %1, `.3` -> %0. AN
    # OUT-OF-RANGE PANE INDEX DOES NOT ERROR; TMUX ALIASES IT TO THE FIRST
    # PANE. So both send-keys landed on main: it received its own resume
    # command AND aux's, the latter typed straight into the running agent's
    # input box, while aux's pane got nothing at all. Two distinct addresses
    # collapsed onto one target with no diagnostic.
    #
    # Launch a SHELL and type the command into it, rather than making claude the
    # pane's root process: if claude exits (crash, /exit, a bad resume) the pane
    # then survives holding the error instead of vanishing with it.
    local p_main p_aux
    p_main=$(tmux new-session -d -P -F '#{pane_id}' -s "$TMUX_PAIR" -n "$WINDOW" -c "$MAIN_DIR") \
        || die "could not create tmux session '$TMUX_PAIR'"
    p_aux=$(tmux split-window -h -P -F '#{pane_id}' -t "$p_main" -c "$AUX_DIR") \
        || die "could not split the aux pane"

    # The control that would have caught the index bug on its first run.
    [ -n "$p_main" ] && [ -n "$p_aux" ] && [ "$p_main" != "$p_aux" ] \
        || die "pane ids are not distinct (main='$p_main' aux='$p_aux') -- refusing to drive two agents into one pane"

    tmux select-pane -t "$p_main" -T "main:$main_conv"
    tmux select-pane -t "$p_aux"  -T "aux:$aux_conv"
    tmux set-option -p -t "$p_main" @thyla-role main
    tmux set-option -p -t "$p_aux"  @thyla-role aux
    tmux set-option -t "$TMUX_PAIR" pane-border-status top
    tmux set-option -t "$TMUX_PAIR" pane-border-format ' #{pane_index} #{pane_title} #{pane_current_path} '

    launch_pane "$p_main" "$MAIN_DIR" "claude --resume $main_conv" main && started=1
    launch_pane "$p_aux"  "$AUX_DIR"  "claude --resume $aux_conv"  aux  && started=1

    if [ "$started" = 1 ]; then
        sleep 6
        settled "$MAIN_DIR" main
        settled "$AUX_DIR"  aux
    fi
    tmux select-pane -t "$p_main"
    echo "thyla-tmux: '$TMUX_PAIR' up -- pane 0 main ($main_conv), pane 1 aux ($aux_conv)"
    attach "$TMUX_PAIR"
}

start_vault() {
    local conv="$1"
    [ -d "$VAULT_DIR" ] || die "missing worktree: $VAULT_DIR"
    if tmux has-session -t "$TMUX_VAULT" 2>/dev/null; then
        echo "thyla-tmux: tmux session '$TMUX_VAULT' already exists -- attaching."
        attach "$TMUX_VAULT"; return
    fi
    claude_pid_in "$VAULT_DIR" >/dev/null && die "a claude is already serving $VAULT_DIR (outside tmux?) -- refusing a second one"

    local p_vault
    p_vault=$(tmux new-session -d -P -F '#{pane_id}' -s "$TMUX_VAULT" -n vault -c "$VAULT_DIR") \
        || die "could not create tmux session '$TMUX_VAULT'"
    tmux select-pane -t "$p_vault" -T "vault:$conv"
    tmux set-option -p -t "$p_vault" @thyla-role vault
    tmux send-keys -t "$p_vault" "claude --resume $conv" C-m

    # THE CHECK IS BEHAVIOURAL, NOT PREDICTIVE, deliberately. Nothing on disk
    # resolves a conversation name -- there is no vault project dir to inspect
    # -- so a "does this conversation exist?" precheck would be a check that
    # CANNOT FAIL, which is worse than none. Start it, then look.
    sleep 6
    if settled "$VAULT_DIR" vault; then
        attach "$TMUX_VAULT"
    else
        echo "  'claude --resume $conv' left no live claude in $VAULT_DIR after 6s." >&2
        echo "  REFUSING to fall back to a fresh session: it would carry no history and" >&2
        echo "  register a different yip line, while looking identical to a working one." >&2
        echo "  The pane is left up with the error:  tmux attach -t $TMUX_VAULT" >&2
        exit 2
    fi
}

attach() {
    # Inside tmux already, `attach` would nest. Switch instead.
    if [ -n "${TMUX:-}" ]; then tmux switch-client -t "$1"; else tmux attach-session -t "$1"; fi
}

status() {
    echo "== tmux sessions =="
    tmux list-sessions 2>/dev/null || echo "  (no server running)"
    echo "== panes =="
    tmux list-panes -a -F '  #{session_name}:#{window_name}.#{pane_index}  id=#{pane_id}  role=#{@thyla-role}  #{pane_title}  #{pane_current_path}' 2>/dev/null || true
    echo "== claude processes, by cwd =="
    local pid cwd found=0
    for pid in $(claude_pids); do
        cwd=$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')
        printf '  pid=%-7s %-46s %s\n' "$pid" "${cwd:-<unknown>}" "$(ps -o args= -p "$pid" 2>/dev/null | head -1)"
        found=1
    done
    [ "$found" = 1 ] || echo "  (none)"
}

case "${1:-pair}" in
    -h|--help|help) usage 0 ;;
    pair|main|"")   start_pair "${2:-$DEF_MAIN_CONV}" "${3:-$DEF_AUX_CONV}" ;;
    vault)          start_vault "${2:-$DEF_VAULT_CONV}" ;;
    status)         status ;;
    *)              die "unknown target '$1' (want: pair | vault | status | help)" ;;
esac
