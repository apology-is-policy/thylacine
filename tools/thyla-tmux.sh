#!/usr/bin/env bash
# Bring up the three-agent tmux layout.
#
#   thyla-tmux.sh            main + aux, one session, two panes  (Ghostty tab 1)
#   thyla-tmux.sh vault      the vault, its own session          (Ghostty tab 2)
#   thyla-tmux.sh status     what is up, without attaching
#
# WHY TMUX AT ALL: an agent inside a tmux pane can address ITSELF -- tmux sets
# $TMUX_PANE to a unique pane id in every process it starts -- which is what
# makes self-directed `send-keys` (and therefore self-compaction) possible. No
# naming scheme has to be kept in sync between this script and the agents; the
# address is handed to each one by tmux.
#
# LAYOUT, and why main+aux share a session: they are watched together, so one
# session with two panes shows both at once with no nesting and no doubled
# prefix key. The cost is that `kill-session` takes out both -- use
# `kill-pane` when you mean one.
#
# The vault is separate because it is transient, and because its conversation
# lives under the AUX project key while its cwd is the vault worktree (there is
# no ~/.claude/projects/-Users-northkillpd-projects-thylacine-vault). That
# asymmetry is why it needs its own launch line rather than a third pane here.
set -uo pipefail

MAIN_DIR="$HOME/projects/thylacine"
AUX_DIR="$HOME/projects/thylacine-aux"
VAULT_DIR="$HOME/projects/thylacine-vault"

MAIN_SESSION="thylacine"
VAULT_SESSION="knowledge"
WINDOW="agents"

# THE LAUNCH COMMAND IS PART OF EACH AGENT'S IDENTITY -- a bare `claude` is NOT
# a neutral default here, it silently starts a FRESH conversation and discards
# the arc. Measured from the running processes rather than assumed: main and
# aux both run `claude --continue`, the vault runs `claude -r knowledge` (its
# conversation lives under the AUX project key, so --continue would resume the
# wrong one). The first version of this script sent bare `claude` and produced
# exactly that: a fresh, historyless main.
MAIN_CMD="claude --continue"
AUX_CMD="claude --continue"
VAULT_CMD="claude -r $VAULT_SESSION"

die() { echo "thyla-tmux: $*" >&2; exit 2; }

command -v tmux >/dev/null || die "tmux is not installed"

# A claude already serving a tree is the thing we must never duplicate: two
# agents in one worktree collide on the yip identity, the build artifacts and
# the pool fixture. Match on the process CWD, not on a name -- a name match
# would also hit this script and any editor sitting in the same dir.
#
# ENUMERATE WITH ps, NOT pgrep, and the reason is measured rather than stylistic.
# BSD/macOS pgrep EXCLUDES THE CALLING PROCESS AND ALL ITS ANCESTORS unless -a
# is given. This script is normally run from a shell inside an agent's own
# pane, so that agent is an ancestor -- and pgrep would omit precisely the one
# whose duplication this guard exists to prevent. Measured here: with three
# claudes live, `pgrep claude` returned 2 (silently dropping the caller's own),
# `pgrep -a claude` returned 3. `-a` is NOT the portable fix either: on Linux
# it means "print the full command line". ps has neither behaviour.
claude_pids() { ps -eo pid=,comm= | awk '{ n = $2; sub(/.*\//, "", n); if (n == "claude") print $1 }'; }

claude_in() {
    local dir="$1" pid
    for pid in $(claude_pids); do
        [ "$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')" = "$dir" ] && return 0
    done
    return 1
}

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
    else
        tmux send-keys -t "$target" "$cmd" C-m
        echo "  $label: started -> $cmd"
    fi
}

start_pair() {
    if tmux has-session -t "$MAIN_SESSION" 2>/dev/null; then
        echo "thyla-tmux: session '$MAIN_SESSION' already exists -- attaching, not recreating."
    else
        for d in "$MAIN_DIR" "$AUX_DIR"; do
            [ -d "$d" ] || die "missing worktree: $d"
        done
        # Launch a SHELL and type the command into it, rather than making claude
        # the pane's root process: if claude exits (crash, /exit, a bad resume)
        # the pane then survives holding the error instead of vanishing with it.
        tmux new-session -d -s "$MAIN_SESSION" -n "$WINDOW" -c "$MAIN_DIR"
        tmux split-window -h -t "$MAIN_SESSION:$WINDOW" -c "$AUX_DIR"

        tmux select-pane -t "$MAIN_SESSION:$WINDOW.0" -T main
        tmux select-pane -t "$MAIN_SESSION:$WINDOW.1" -T aux
        tmux set-option -p -t "$MAIN_SESSION:$WINDOW.0" @thyla-role main
        tmux set-option -p -t "$MAIN_SESSION:$WINDOW.1" @thyla-role aux
        tmux set-option -t "$MAIN_SESSION" pane-border-status top
        tmux set-option -t "$MAIN_SESSION" pane-border-format ' #{pane_index} #{pane_title} #{pane_current_path} '

        launch_pane "$MAIN_SESSION:$WINDOW.0" "$MAIN_DIR" "$MAIN_CMD" main
        launch_pane "$MAIN_SESSION:$WINDOW.1" "$AUX_DIR"  "$AUX_CMD"  aux
        tmux select-pane -t "$MAIN_SESSION:$WINDOW.0"
        echo "thyla-tmux: '$MAIN_SESSION' up -- pane 0 main ($MAIN_DIR), pane 1 aux ($AUX_DIR)"
    fi
    attach "$MAIN_SESSION"
}

start_vault() {
    [ -d "$VAULT_DIR" ] || die "missing worktree: $VAULT_DIR"
    if tmux has-session -t "$VAULT_SESSION" 2>/dev/null; then
        echo "thyla-tmux: session '$VAULT_SESSION' already exists -- attaching."
        attach "$VAULT_SESSION"; return
    fi
    claude_in "$VAULT_DIR" && die "a claude is already serving $VAULT_DIR (outside tmux?) -- refusing a second one"

    tmux new-session -d -s "$VAULT_SESSION" -n vault -c "$VAULT_DIR"
    tmux set-option -p -t "$VAULT_SESSION:vault.0" @thyla-role vault
    tmux send-keys -t "$VAULT_SESSION:vault.0" "$VAULT_CMD" C-m

    # THE CHECK IS BEHAVIOURAL, NOT PREDICTIVE, and the distinction is
    # deliberate. Nothing on disk resolves the name "knowledge" -- there is no
    # vault project dir to inspect -- so a "does this session exist?" precheck
    # would be a check that cannot fail, which is worse than none. Instead:
    # start it, then look. A failed `-r` exits fast, so a pane that still holds
    # a live claude after the settle is the evidence.
    sleep 6
    if claude_in "$VAULT_DIR"; then
        echo "thyla-tmux: '$VAULT_SESSION' up -- claude resumed in $VAULT_DIR"
        attach "$VAULT_SESSION"
    else
        echo "thyla-tmux: vault did NOT come up." >&2
        echo "  'claude -r $VAULT_SESSION' left no live claude in $VAULT_DIR after 6s." >&2
        echo "  Most likely the session id is not resumable (gone, or attached elsewhere)." >&2
        echo "  REFUSING to fall back to a fresh session: it would carry no history and" >&2
        echo "  register a different yip line, while looking identical to a working one." >&2
        echo "  The pane is left up with the error -- attach and read it:" >&2
        echo "      tmux attach -t $VAULT_SESSION" >&2
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
    tmux list-panes -a -F '  #{session_name}:#{window_name}.#{pane_index}  id=#{pane_id}  role=#{@thyla-role}  #{pane_current_path}' 2>/dev/null || true
    echo "== claude processes, by cwd =="
    local pid cwd found=0
    for pid in $(claude_pids); do
        cwd=$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')
        printf '  pid=%-7s %s\n' "$pid" "${cwd:-<unknown>}"; found=1
    done
    [ "$found" = 1 ] || echo "  (none)"
}

case "${1:-pair}" in
    pair|main|"")  start_pair ;;
    vault)         start_vault ;;
    status)        status ;;
    *)             die "unknown target '$1' (want: pair | vault | status)" ;;
esac
