#!/usr/bin/env python3
"""PostToolUse(Read) session hook: log which files this session has Read.
Tier-3 advisory machinery (vault/meta/workflow.md section 6.3). Safe unwired."""
import json, os, sys
try:
    payload = json.load(sys.stdin)
except Exception:
    sys.exit(0)
p = (payload.get("tool_input") or {}).get("file_path", "")
if p:
    sid = os.environ.get("CLAUDE_SESSION_ID") or str(os.getppid())
    with open("/tmp/claude-vault-readlog-%s" % sid, "a") as f:
        f.write(p + "\n")
sys.exit(0)
