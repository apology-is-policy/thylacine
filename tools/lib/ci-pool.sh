# LS-CI pool selection and isolation. Sourced by test-interactive.sh and its
# fixture test. The caller supplies POOL, POOL_SNAP, KEYFILE and KEY_SNAP.
# Always populate the per-scenario destination before boot. A missing/stale
# pristine snapshot selects a copy of the current base, never an absent slot.
pool_restored=0
pool_restore() {
    local dest="${1:-$POOL}"
    local source="$POOL"
    local pristine=0
    if [[ "${LS_CI_POOL_RESTORE:-1}" == "0" && -f "$dest" ]]; then
        # Deliberate contamination reproduction: retain this scenario's pool
        # across attempts, but still seed it on the first attempt.
        return 0
    fi
    if [[ "${LS_CI_POOL_RESTORE:-1}" != "0" && -f "$POOL_SNAP" ]]; then
        if cmp -s "$KEYFILE" "$KEY_SNAP" 2>/dev/null; then
            source="$POOL_SNAP"
            pristine=1
        else
            [[ $pool_restored -ne 0 ]] || echo "    (pool snapshot rejected: key mismatch; copying current base into isolated slot)" >&2
        fi
    fi
    if [[ ! -f "$source" ]]; then
        echo "==> FATAL: pool source missing: $source; refusing to boot." >&2
        exit 1
    fi
    if [[ "$source" != "$dest" ]]; then
        if ! { cp -c "$source" "$dest" 2>/dev/null || cp "$source" "$dest"; }; then
            echo "==> FATAL: pool restore failed -- $dest may be partial/truncated; refusing to boot." >&2
            exit 1
        fi
    fi
    if [[ $pristine -eq 1 ]]; then pool_restored=1; else pool_restored=-1; fi
}
