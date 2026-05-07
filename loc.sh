#!/bin/sh
# Cached LOC count for cc-statusline.
# Per-PWD cache in /tmp, refreshed every 10 minutes.

CACHE_TTL=600
CACHE_DIR="/tmp/cloc-statusline-cache2"
mkdir -p "$CACHE_DIR" 2>/dev/null

key=$(printf '%s' "$PWD" | shasum | awk '{print $1}')
cache="$CACHE_DIR/$key"

# Serve from cache if fresh
if [ -f "$cache" ]; then
    mtime=$(stat -f %m "$cache" 2>/dev/null)
    if [ -n "$mtime" ] && [ $(( $(date +%s) - mtime )) -lt "$CACHE_TTL" ]; then
        cat "$cache"
        exit 0
    fi
fi

# Refresh
result=$(cloc . \
    --include-lang="C,C/C++ Header,Rust,Assembly,TLA+,CMake" \
    --exclude-dir=build,cmake-build-debug,cmake-build-release,CMakeFiles,.cache \
    --not-match-d='(^|/)\.(git|idea|vscode)$' \
    --csv --quiet 2>/dev/null \
    | awk -F',' '$2 == "SUM" { print $5 }')

if [ -n "$result" ]; then
    printf '%s\n' "$result" > "$cache"
    printf '%s\n' "$result"
elif [ -f "$cache" ]; then
    # Fall back to stale cache on failure rather than blanking the statusline
    cat "$cache"
fi
