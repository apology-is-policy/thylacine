#!/usr/bin/env python3
"""The vault linter -- the schema's teeth (vault/meta/schema.md section 8).

Modes:
  lint.py --all                validate every note (CI / manual)
  lint.py --staged             pre-commit: full validation + Record-plane
                               immutability + staged-chg checks
  lint.py --render             rewrite generated view bodies, then validate
  lint.py --closed <sub-id>    print the do-not-re-report preamble for a surface

Stdlib only. Parses the schema's deliberately small YAML subset: scalar
values, inline [a, b] lists, block "- item" lists, one flow-map kept raw.
Notes live under vault/ excluding vault/meta/ (prose + machinery, not notes).
"""

import os
import re
import subprocess
import sys

TYPES = ("moc", "sub", "inv", "spec", "abi", "lock", "lin", "haz", "gls",
         "gate", "seam", "msr", "arc", "chg", "adt", "fnd", "dec", "wkf",
         "view")
NO_PREFIX_IDS = {"home", "dashboard"}
ID_RE = re.compile(r"^(%s)-[a-z0-9][a-z0-9.-]*$" % "|".join(TYPES))

REQUIRED = {
    "moc": ["parent"],
    "sub": ["parent", "code", "audit", "guarded-by", "validated-by"],
    "inv": ["number", "guards", "validated-by", "strength"],
    "spec": ["models", "pins", "cfgs", "gate"],
    "abi": ["kind", "stability", "pinned-by", "mirrors"],
    "lock": ["kind", "guards"],
    "lin": ["surfaces", "members"],
    "haz": ["applies-to"],
    "gls": [],
    "gate": ["proves", "blind-to", "invocation"],
    "seam": ["status", "surface", "opened-by"],
    "msr": ["metric", "unit"],
    "arc": ["status", "chunks"],
    "chg": ["date", "arc", "commits", "touched", "depth"],
    "adt": ["date", "scope", "reviewer", "model-start", "model-end",
            "verdict", "counts", "findings"],
    "fnd": ["round", "severity", "status", "surface", "threatens"],
    "dec": ["date", "status", "decided-by", "affects"],
    "wkf": [],
    "view": ["query"],
}

ENUMS = {
    ("sub", "audit"): {"hard", "light", "none"},
    ("inv", "strength"): {"spec", "test", "prose"},
    ("abi", "kind"): {"syscall", "wire", "struct", "registry", "contract"},
    ("abi", "stability"): {"frozen", "append-only", "internal"},
    ("seam", "status"): {"open", "closed"},
    ("arc", "status"): {"active", "complete", "abandoned"},
    ("chg", "depth"): {"rich", "skeletal"},
    ("adt", "reviewer"): {"fable", "opus", "self"},
    ("adt", "verdict"): {"clean", "dirty"},
    ("fnd", "severity"): {"P0", "P1", "P2", "P3"},
    ("fnd", "status"): {"fixed", "deferred", "documented", "withdrawn"},
    ("dec", "status"): {"standing", "superseded"},
    ("dec", "decided-by"): {"user-vote", "autonomous", "research-collapsed"},
}

# Fields whose id-shaped values must resolve in the registry.
STRICT_EDGE_FIELDS = {
    "parent", "guarded-by", "guards", "validated-by", "models", "pins",
    "hazards", "locks", "abis", "surfaces", "members", "applies-to",
    "touched", "established", "closed", "opened", "scope", "findings",
    "threatens", "surface", "arc", "chunks", "follow-ons", "round",
    "prior-round", "supersedes", "superseded-by", "fixed-by", "closed-by",
    "opened-by", "seam", "hazard", "instances", "orders-before",
    "refers-to", "affects", "round-of",
}
EDGE_LITERALS = {"prose", "global"}

# Record-plane closure fields (schema section 5.3).
CLOSURE = {
    "fnd": {"status", "fixed-by", "regression", "seam"},
    "dec": {"superseded-by", "status"},
    "chg": {"commits"},
    "adt": set(),
}

SUB_SECTIONS = ["Purpose", "Contract", "Mechanism", "Data structures",
                "Concurrency", "Invariants enforced", "Error paths",
                "Performance", "Prosecution", "Seams", "Caveats",
                "Provenance"]

GEN_BEGIN = "<!-- generated:begin -->"
GEN_END = "<!-- generated:end -->"

FILE_LINE_RE = re.compile(r"\b[\w./-]+\.(?:c|h|rs|go|py|S|s|tla|md):\d+\b")
WIKILINK_RE = re.compile(r"\[\[([^\]#|]+)")


class Note:
    __slots__ = ("id", "path", "rel", "front", "body", "raw")

    def __init__(self, id_, path, rel, front, body, raw):
        self.id, self.path, self.rel = id_, path, rel
        self.front, self.body, self.raw = front, body, raw


def repo_root():
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True)
    if out.returncode == 0:
        return out.stdout.strip()
    return os.getcwd()


def strip_quotes(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        return v[1:-1]
    return v


def strip_comment(v):
    if v.startswith(("\"", "'", "{", "[")):
        return v
    return re.sub(r"\s+#\s.*$", "", v)


def parse_front(text):
    """Return (front_dict_or_None, body)."""
    if not text.startswith("---\n"):
        return None, text
    end = text.find("\n---", 4)
    while end != -1 and not (text[end:end + 5] in ("\n---\n",) or
                             text[end:].rstrip() == "\n---"):
        end = text.find("\n---", end + 1)
    if end == -1:
        return None, text
    block = text[4:end]
    body = text[end + 5:] if text[end:end + 5] == "\n---\n" else ""
    front = {}
    key = None
    for raw in block.splitlines():
        if not raw.strip() or raw.strip().startswith("#"):
            continue
        if raw[:1] in (" ", "\t") and raw.lstrip().startswith("- "):
            if key is None:
                continue
            if not isinstance(front.get(key), list):
                front[key] = []
            front[key].append(strip_quotes(strip_comment(raw.lstrip()[2:])))
            continue
        m = re.match(r"^([A-Za-z0-9_-]+):\s*(.*)$", raw)
        if not m:
            continue
        key, val = m.group(1), strip_comment(m.group(2)).strip()
        if val == "" or val == "[]":
            front[key] = [] if val == "[]" else []
            # bare "key:" opens a block list; "[]" is the empty list
            if val == "":
                front[key] = []
            continue
        if val.startswith("[") and val.endswith("]"):
            inner = val[1:-1].strip()
            front[key] = ([strip_quotes(x) for x in
                           re.split(r",\s*", inner)] if inner else [])
        elif val.startswith("{"):
            front[key] = val  # flow map kept raw; presence-checked only
        else:
            front[key] = strip_quotes(val)
    return front, body


def load_registry(root):
    vault = os.path.join(root, "vault")
    notes, errors = {}, []
    for dirpath, dirnames, filenames in os.walk(vault):
        rel_dir = os.path.relpath(dirpath, vault)
        if rel_dir == "meta" or rel_dir.startswith("meta" + os.sep):
            dirnames[:] = []
            continue
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for fn in sorted(filenames):
            if not fn.endswith(".md"):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root)
            raw = open(path, encoding="utf-8").read()
            front, body = parse_front(raw)
            stem = fn[:-3]
            if front is None:
                errors.append(f"{rel}: missing or unterminated frontmatter")
                continue
            n = Note(stem, path, rel, front, body, raw)
            if stem in notes:
                errors.append(f"{rel}: duplicate id '{stem}'")
            notes[stem] = n
    return notes, errors


def alias_map(reg):
    amap = {}
    for n in reg.values():
        for a in (n.front.get("aliases") or []):
            amap.setdefault(a, n.id)
    return amap


def is_record(rel):
    return rel.replace(os.sep, "/").startswith("vault/record/")


def validate(reg, pre_errors):
    fails = list(pre_errors)
    warns = []
    ids = set(reg)
    amap = alias_map(reg)
    for n in sorted(reg.values(), key=lambda x: x.rel):
        f = n.front
        t = f.get("type")
        if f.get("id") != n.id:
            fails.append(f"{n.rel}: frontmatter id '{f.get('id')}' != filename '{n.id}'")
        if t not in TYPES:
            fails.append(f"{n.rel}: unknown type '{t}'")
            continue
        if n.id not in NO_PREFIX_IDS:
            if not n.id.startswith(t + "-"):
                fails.append(f"{n.rel}: id prefix does not match type '{t}'")
            if not ID_RE.match(n.id):
                fails.append(f"{n.rel}: id '{n.id}' not kebab-case type-prefixed")
        for req in REQUIRED.get(t, []):
            if req == "parent" and n.id == "home":
                continue
            if req not in f:
                fails.append(f"{n.rel}: required field '{req}' missing")
        for (et, field), allowed in ENUMS.items():
            if et == t and field in f and isinstance(f[field], str) \
                    and f[field] not in allowed:
                fails.append(f"{n.rel}: {field}='{f[field]}' not in {sorted(allowed)}")
        if is_record(n.rel) and "updated" in f:
            fails.append(f"{n.rel}: 'updated' is forbidden on the Record plane")
        # edge resolution
        for field, val in f.items():
            if field not in STRICT_EDGE_FIELDS:
                continue
            vals = val if isinstance(val, list) else [val]
            for v in vals:
                if not isinstance(v, str) or not v:
                    continue
                if v in ids:
                    continue
                if ID_RE.match(v):
                    if v not in ids:
                        fails.append(f"{n.rel}: {field} -> unknown id '{v}'")
                elif v in EDGE_LITERALS or "/" in v or " " in v or "." in v:
                    pass
                else:
                    warns.append(f"{n.rel}: {field} value '{v}' is neither a "
                                 f"known id shape nor a path/literal")
        # wikilinks
        for target in WIKILINK_RE.findall(n.body):
            tgt = target.strip()
            if not tgt or "/" in tgt:
                continue
            if tgt not in ids and tgt not in amap:
                fails.append(f"{n.rel}: dangling wikilink [[{tgt}]]")
        # type-specific
        if t == "sub":
            waived = set(m.group(1).strip() for m in
                         re.finditer(r"^>\s*waived:\s*(.+?)\s+--", n.body, re.M))
            got = [m.group(1).strip() for m in
                   re.finditer(r"^##\s+(.+?)\s*$", n.body, re.M)]
            need = [s for s in SUB_SECTIONS if s not in waived]
            missing = [s for s in need if s not in got]
            if missing:
                fails.append(f"{n.rel}: dossier sections missing (no waiver): {missing}")
            else:
                order = [s for s in got if s in need]
                if order != [s for s in need if s in order]:
                    warns.append(f"{n.rel}: dossier sections out of schema order")
        if t == "fnd" and f.get("status") == "deferred":
            linked = (isinstance(f.get("seam"), str) and f["seam"].startswith("seam-")) \
                or (isinstance(f.get("regression"), str) and f["regression"].startswith("seam-")) \
                or ("seam-" in n.body)
            if not linked:
                fails.append(f"{n.rel}: status=deferred without a seam-* link "
                             f"(silent drops cannot land)")
        if t == "chg":
            touched = f.get("touched") or []
            mirrors_needed = 0
            for tid in touched:
                tn = reg.get(tid)
                if tn and tn.front.get("type") == "abi" and (tn.front.get("mirrors") or []):
                    mirrors_needed = max(mirrors_needed,
                                         len(tn.front.get("mirrors")))
            checked = f.get("mirrors-checked") or []
            if mirrors_needed and len(checked) < mirrors_needed:
                fails.append(f"{n.rel}: touched abi has {mirrors_needed} mirrors; "
                             f"mirrors-checked covers {len(checked)}")
        if not is_record(n.rel) and t != "view":
            for m in FILE_LINE_RE.finditer(n.body):
                warns.append(f"{n.rel}: file:line citation '{m.group(0)}' on the "
                             f"Present plane (R4: cite symbols/tests)")
    return fails, warns


# ---------------------------------------------------------------- renderers

def _line(n, key, default=""):
    v = n.front.get(key, default)
    return ", ".join(v) if isinstance(v, list) else str(v)


def render_dashboard(reg):
    arcs = [n for n in reg.values() if n.front.get("type") == "arc"]
    seams = [n for n in reg.values() if n.front.get("type") == "seam"
             and n.front.get("status") == "open"]
    chgs = sorted([n for n in reg.values() if n.front.get("type") == "chg"],
                  key=lambda n: str(n.front.get("date", "")), reverse=True)[:8]
    out = ["## Arcs", "", "| arc | status | chunks |", "|---|---|---|"]
    for a in sorted(arcs, key=lambda n: n.id):
        out.append(f"| [[{a.id}]] | {a.front.get('status')} | "
                   f"{len(a.front.get('chunks') or [])} |")
    if not arcs:
        out.append("| (none yet) | | |")
    out += ["", f"## Open seams: {len(seams)}", ""]
    out += [f"- [[{s.id}]] ({_line(s, 'surface')})" for s in
            sorted(seams, key=lambda n: n.id)] or ["- (none)"]
    out += ["", "## Recent changes", ""]
    out += [f"- {c.front.get('date')} [[{c.id}]] — {c.front.get('title', '')}"
            for c in chgs] or ["- (none yet)"]
    return "\n".join(out)


def render_invariants(reg):
    invs = sorted([n for n in reg.values() if n.front.get("type") == "inv"],
                  key=lambda n: n.id)
    out = ["| # | invariant | strength | guards | validated by |",
           "|---|---|---|---|---|"]
    for i in invs:
        out.append(f"| {i.front.get('number')} | [[{i.id}]] | "
                   f"{i.front.get('strength')} | {_line(i, 'guards')} | "
                   f"{_line(i, 'validated-by')} |")
    if not invs:
        out.append("| (none yet) | | | | |")
    return "\n".join(out)


def render_seams(reg):
    seams = sorted([n for n in reg.values() if n.front.get("type") == "seam"],
                   key=lambda n: (n.front.get("status", ""), n.id))
    out = ["| seam | status | surface | opened by | tracker |",
           "|---|---|---|---|---|"]
    for s in seams:
        out.append(f"| [[{s.id}]] | {s.front.get('status')} | "
                   f"{_line(s, 'surface')} | {_line(s, 'opened-by')} | "
                   f"{s.front.get('tracker', '')} |")
    if not seams:
        out.append("| (none yet) | | | | |")
    return "\n".join(out)


def render_audit_triggers(reg):
    subs = sorted([n for n in reg.values() if n.front.get("type") == "sub"
                   and n.front.get("audit") == "hard"], key=lambda n: n.id)
    out = ["| surface | code | invariants | prosecution |",
           "|---|---|---|---|"]
    for s in subs:
        pros = ""
        m = re.search(r"^##\s+Prosecution\s*$\n+(.+?)$", s.body, re.M)
        if m:
            pros = m.group(1).strip()[:160]
        out.append(f"| [[{s.id}]] | {_line(s, 'code')} | "
                   f"{_line(s, 'guarded-by')} | {pros} |")
    if not subs:
        out.append("| (none yet) | | | |")
    return "\n".join(out)


def render_roadmap(reg):
    arcs = sorted([n for n in reg.values() if n.front.get("type") == "arc"],
                  key=lambda n: (n.front.get("status", ""), n.id))
    out = ["| arc | status | chunks landed | follow-ons |",
           "|---|---|---|---|"]
    for a in arcs:
        out.append(f"| [[{a.id}]] | {a.front.get('status')} | "
                   f"{len(a.front.get('chunks') or [])} | "
                   f"{_line(a, 'follow-ons')} |")
    if not arcs:
        out.append("| (none yet) | | | |")
    return "\n".join(out)


RENDERERS = {
    "dashboard": render_dashboard,
    "invariants": render_invariants,
    "seams": render_seams,
    "audit-triggers": render_audit_triggers,
    "roadmap": render_roadmap,
}


def view_notes(reg):
    return [n for n in reg.values() if n.front.get("type") == "view"]


def rendered_body(note, reg):
    q = note.front.get("query")
    fn = RENDERERS.get(q)
    if fn is None:
        return None, f"{note.rel}: no renderer for query '{q}'"
    if GEN_BEGIN not in note.raw or GEN_END not in note.raw:
        return None, f"{note.rel}: missing {GEN_BEGIN}/{GEN_END} markers"
    pre, rest = note.raw.split(GEN_BEGIN, 1)
    _, post = rest.split(GEN_END, 1)
    new = pre + GEN_BEGIN + "\n" + fn(reg) + "\n" + GEN_END + post
    return new, None


def check_views(reg):
    fails = []
    for v in view_notes(reg):
        new, err = rendered_body(v, reg)
        if err:
            fails.append(err)
        elif new != v.raw:
            fails.append(f"{v.rel}: stale generated body (run lint.py --render)")
    return fails


def render_views(root, reg):
    changed = []
    for v in view_notes(reg):
        new, err = rendered_body(v, reg)
        if err:
            print("render: " + err)
            continue
        if new != v.raw:
            open(v.path, "w", encoding="utf-8").write(new)
            changed.append(v.rel)
    return changed


# ------------------------------------------------------------------ staged

def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True).stdout


def staged_checks(root, reg):
    fails, warns = [], []
    status = git("diff", "--cached", "--name-status")
    entries = []
    for line in status.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2:
            entries.append((parts[0][:1], parts[-1]))
    staged_paths = {p for _, p in entries}
    added_chg_raw = ""
    for st, p in entries:
        if st == "A" and p.startswith("vault/record/changes/"):
            added_chg_raw += git("show", ":" + p)
    for st, p in entries:
        if st != "M" or not p.startswith("vault/record/"):
            continue
        old_raw = git("show", "HEAD:" + p)
        new_raw = git("show", ":" + p)
        of, ob = parse_front(old_raw)
        nf, nb = parse_front(new_raw)
        if of is None or nf is None:
            continue
        t = nf.get("type")
        if t == "arc" and of.get("status") == "active":
            continue  # arcs are mutable until frozen
        if (ob or "").strip() != (nb or "").strip():
            fails.append(f"{p}: Record-plane body changed (R3: append-only; "
                         f"correct via a superseding note)")
            continue
        keys = set(of) | set(nf)
        changed = {k for k in keys if of.get(k) != nf.get(k)}
        allowed = CLOSURE.get(t, set())
        illegal = changed - allowed
        if illegal:
            fails.append(f"{p}: non-closure Record fields changed: {sorted(illegal)}")
            continue
        if changed:
            note_id = os.path.basename(p)[:-3]
            self_fixup = (t == "chg" and changed == {"commits"})
            if not self_fixup and note_id not in added_chg_raw:
                fails.append(f"{p}: closure fields {sorted(changed)} changed with "
                             f"no staged chg-* note linking '{note_id}'")
    # audit:hard dossier-diff warning for staged chg notes
    for st, p in entries:
        if st != "A" or not p.startswith("vault/record/changes/"):
            continue
        f, _ = parse_front(git("show", ":" + p))
        if not f:
            continue
        if "no-dossier-change" in f:
            continue
        for tid in (f.get("touched") or []):
            tn = reg.get(tid)
            if tn and tn.front.get("type") == "sub" \
                    and tn.front.get("audit") == "hard" \
                    and tn.rel not in staged_paths:
                warns.append(f"{p}: touches audit:hard [[{tid}]] but "
                             f"{tn.rel} is not in this commit (add a "
                             f"'no-dossier-change: <why>' field if deliberate)")
    return fails, warns


def print_closed(reg, sub_id):
    fnds = [n for n in reg.values() if n.front.get("type") == "fnd"
            and sub_id in (n.front.get("surface") or [])
            and n.front.get("status") in ("fixed", "documented", "withdrawn")]
    print(f"# Do-not-re-report preamble for {sub_id} "
          f"({len(fnds)} closed findings)\n")
    for n in sorted(fnds, key=lambda x: x.id):
        f = n.front
        line1 = ""
        m = re.search(r"^##\s+Disposition\s*$\n+(.+?)$", n.body, re.M)
        if m:
            line1 = " — " + m.group(1).strip()
        print(f"- {n.id} [{f.get('severity')}] {f.get('title', '')} "
              f"({f.get('status')}){line1}")


def main(argv):
    root = repo_root()
    os.chdir(root)
    mode = argv[1] if len(argv) > 1 else "--all"
    reg, pre_errors = load_registry(root)
    if mode == "--closed":
        if len(argv) < 3:
            print("usage: lint.py --closed <sub-id>")
            return 2
        print_closed(reg, argv[2])
        return 0
    if mode == "--render":
        changed = render_views(root, reg)
        for c in changed:
            print(f"rendered: {c}")
        reg, pre_errors = load_registry(root)
    fails, warns = validate(reg, pre_errors)
    fails += check_views(reg)
    if mode == "--staged":
        sf, sw = staged_checks(root, reg)
        fails += sf
        warns += sw
    for w in warns:
        print(f"WARN {w}")
    for f in fails:
        print(f"FAIL {f}")
    n_notes = len(reg)
    print(f"vault-lint: {n_notes} notes, {len(fails)} fail(s), "
          f"{len(warns)} warn(s) [{mode}]")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
