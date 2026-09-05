#!/usr/bin/env python3
"""PreToolUse(Edit|Write) session hook: warn when editing an audit:hard
dossier's code territory without that dossier in this session's read log.
ADVISORY ONLY -- always exits 0; never blocks (workflow section 6.3)."""
import fnmatch, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import lint
    payload = json.load(sys.stdin)
except Exception:
    sys.exit(0)
target = (payload.get("tool_input") or {}).get("file_path", "")
if not target or "/vault/" in target:
    sys.exit(0)
root = lint.repo_root()
reg, _ = lint.load_registry(root)
sid = os.environ.get("CLAUDE_SESSION_ID") or str(os.getppid())
try:
    readlog = open("/tmp/claude-vault-readlog-%s" % sid).read()
except Exception:
    readlog = ""
rel = os.path.relpath(target, root)
for n in reg.values():
    if n.front.get("type") != "sub" or n.front.get("audit") != "hard":
        continue
    code = n.front.get("code") or []
    for g in (code if isinstance(code, list) else [code]):
        if fnmatch.fnmatch(rel, g) or rel == g:
            if n.path not in readlog:
                sys.stderr.write(
                    "vault nag: %s is [%s] territory (audit: hard); its dossier "
                    "was not read this session -- read %s first.\n"
                    % (rel, n.id, n.rel))
            sys.exit(0)
sys.exit(0)
