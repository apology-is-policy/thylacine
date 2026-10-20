#!/usr/bin/env python3
"""SYS_SPAWN_FULL_ARGV argument-block mirror check.

`struct sys_spawn_args` crosses the syscall boundary as raw bytes: the kernel
copies sizeof(struct sys_spawn_args) from the caller's pointer. So every
userspace copy of the struct must have the kernel's layout byte for byte, and
each copy's own size assert cannot say whether it does -- an assert compares
the copy with a NUMBER, not with the kernel. When the aux-2 merge grew the
kernel struct 96 -> 104, a mirror left at 96 still passed its own assert and
the kernel read 8 bytes past it (#100). The go fork's committed copy was left
at 96 by the same growth, and for a while its builds were right only because
they compiled an uncommitted working-tree fix.

This check takes the layout from the kernel header -- the struct's field list,
laid out with natural alignment, and cross-checked against the header's own
_Static_asserts so that the layout rule used here is the compiler's -- and
compares every mirror with it field by field:

  libt         usr/lib/libt/include/thyla/syscall.h    struct t_sys_spawn_args
  libthyla-rs  usr/lib/libthyla-rs/src/lib.rs          struct TSpawnArgs
  pouch        usr/lib/pouch/patches/0026-pouch-process.patch
                                                       struct pouch_spawn_args
  go fork      $GOFORK/src/syscall/exec_thylacine.go   type spawnArgs

The in-tree mirrors must match names, offsets and sizes: two same-size fields
swapped keep every offset and still hand the kernel the wrong value. The go
fork lives outside the repo and names its fields in Go style, so it is held to
offsets and sizes only, and is skipped (said so, never silently) when absent.
It is held to one more rule: every field at an offset where the kernel's field
is a user address (a name ending in _va) must be pointer-typed (*T or
unsafe.Pointer). A Go buffer can live on the goroutine stack, and the stack
copier adjusts pointer slots only, so an address kept as an integer across a
call that moves the stack names the freed old stack when the kernel reads it.
Which kernel fields are addresses is checked too: every 8-byte field of the
kernel's struct must be named *_va or be listed in NON_ADDRESS_U64, so a new
address field cannot slip past the rule by lacking the suffix.

The record's tails (SPAWN_EXT_*: a struct after the 104 bytes, announced by
ext_flags) are held to the same rules: each kernel tail struct's offsets
asserted, its 8-byte fields named *_va, and each in-tree mirror of it (EXT_TAILS)
matching names, offsets and sizes.

A green comparison is then proved able to fail: each source is mutated in
memory -- a mirror missing its last field, a mirror with two fields swapped,
the Rust mirror marked packed, a kernel field with no offset assert, each go
fork address field turned into an integer, a kernel address field without its
_va suffix -- and any mutation that goes unreported, by the rule it targets,
fails the check. A checker that cannot verify itself stops the build.
"""
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

KERNEL = ("kernel", "kernel/include/thylacine/syscall.h", "sys_spawn_args")
MIRRORS = [
    ("libt", "usr/lib/libt/include/thyla/syscall.h", "t_sys_spawn_args", "c"),
    ("libthyla-rs", "usr/lib/libthyla-rs/src/lib.rs", "TSpawnArgs", "rust"),
    ("pouch", "usr/lib/pouch/patches/0026-pouch-process.patch",
     "pouch_spawn_args", "patch"),
]
GO_REL = "src/syscall/exec_thylacine.go"
GO_TYPE = "spawnArgs"
# The kernel struct's 8-byte fields that are not user addresses; every other one
# must be named *_va.
NON_ADDRESS_U64 = {"cap_mask"}
# The record's tails (SPAWN_EXT_*, announced by ext_flags): each kernel tail
# struct and its in-tree mirrors, held to the same rules as the record. The go
# fork declares none until it sends one; a `type spawnExt<...> struct` it
# declares that no entry here lists is a failure, so its first tail cannot go
# unchecked.
GO_TAIL = re.compile(r"^\s*type\s+(spawnExt\w*)\s+struct\b", re.M)
EXT_TAILS = [
    ("sys_spawn_ext_cwd", [("libt", "t_sys_spawn_ext_cwd", "c"),
                           ("libthyla-rs", "TSpawnExtCwd", "rust")]),
]

# aarch64 LP64: every type here is naturally aligned (alignment == size).
C_SIZES = {
    "u8": 1, "u16": 2, "u32": 4, "u64": 8,
    "uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8,
    "unsigned char": 1, "unsigned short": 2, "unsigned int": 4,
    "unsigned long": 8, "unsigned long long": 8,
    "int": 4, "long": 8, "size_t": 8, "uintptr_t": 8,
}
RUST_SIZES = {"u8": 1, "u16": 2, "u32": 4, "u64": 8,
              "i8": 1, "i16": 2, "i32": 4, "i64": 8, "usize": 8, "isize": 8}
GO_SIZES = {"uint8": 1, "uint16": 2, "uint32": 4, "uint64": 8,
            "int8": 1, "int16": 2, "int32": 4, "int64": 8, "uintptr": 8}

C_FIELD = re.compile(
    r"^\s*(" + "|".join(sorted((re.escape(t).replace(r"\ ", r"\s+")
                                for t in C_SIZES), key=len, reverse=True))
    + r")\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;\s*$")
RUST_FIELD = re.compile(r"^\s*pub\s+(\w+)\s*:\s*(\w+)\s*,?\s*$")
GO_FIELD = re.compile(r"^\s*(\w+)\s+(\*?\w+(?:\.\w+)?)\s*$")


def go_pointer(t):
    return t.startswith("*") or t == "unsafe.Pointer"


def go_size(t):
    """A Go field type's size; a pointer is 8 bytes on arm64."""
    return 8 if go_pointer(t) else GO_SIZES.get(t)


class CheckError(Exception):
    pass


def patch_post_image(text):
    """The file a patch creates: its added and context lines, prefix removed."""
    out = []
    for line in text.splitlines():
        if line.startswith("+++") or line.startswith("---"):
            out.append("")
        elif line[:1] in ("+", " "):
            out.append(line[1:])
        else:
            out.append("")
    return "\n".join(out)


def strip_comments(lines):
    """Drop // and /* */ comments, keeping one output line per input line."""
    out, in_block = [], False
    for line in lines:
        s, i, buf = line, 0, []
        while i < len(s):
            if in_block:
                j = s.find("*/", i)
                if j < 0:
                    i = len(s)
                else:
                    in_block, i = False, j + 2
            elif s.startswith("/*", i):
                in_block, i = True, i + 2
            elif s.startswith("//", i):
                break
            else:
                buf.append(s[i])
                i += 1
        out.append("".join(buf))
    return out


def struct_body(text, opener, closer, label):
    """(first body line index, body lines) of the one struct `opener` names."""
    lines = text.splitlines()
    starts = [i for i, l in enumerate(lines) if opener.match(l)]
    if len(starts) != 1:
        raise CheckError(f"{label}: expected exactly one struct opener, "
                         f"found {len(starts)}")
    s = starts[0]
    for j in range(s + 1, len(lines)):
        if closer.match(lines[j]):
            return s + 1, lines[s + 1:j]
    raise CheckError(f"{label}: struct opened at line {s + 1} never closes")


def parse_fields(text, struct, lang, label):
    """[(name, size, line_index)] in declaration order; unknown lines fail."""
    if lang == "patch":
        text = patch_post_image(text)
    if lang in ("c", "patch", "kernel"):
        opener = re.compile(r"^\s*struct\s+" + re.escape(struct) + r"\s*\{")
        closer = re.compile(r"^\s*\}\s*;")
    elif lang == "rust":
        opener = re.compile(r"^\s*pub\s+struct\s+" + re.escape(struct) + r"\s*\{")
        closer = re.compile(r"^\s*\}")
    else:
        opener = re.compile(r"^\s*type\s+" + re.escape(struct) + r"\s+struct\s*\{")
        closer = re.compile(r"^\s*\}")
    first, body = struct_body(text, opener, closer, label)
    if lang == "rust":
        before = text.splitlines()[:first - 1]
        # The attribute block above the struct, back to a blank line or the
        # end of the previous item. An attribute may span lines, so the block
        # is read as one text, with its comments dropped.
        block = []
        for line in reversed(before):
            t = line.strip()
            if not t:
                break
            if t.startswith("//"):
                continue
            if t.endswith("}") or t.endswith(";"):
                break
            block.append(t)
        attrs = " ".join(reversed(block))
        # Exactly one plain #[repr(C)] and no other repr, however it is
        # spelled: packed or aligned lays the fields out differently from the
        # natural C layout this check computes for them, and a repr inside
        # cfg_attr is one the attribute's leading token does not show.
        reprs = re.findall(r"\brepr\s*\(", attrs)
        plain = re.findall(r"#\[\s*repr\(\s*C\s*\)\s*\]", attrs)
        if len(reprs) > len(plain) or len(plain) > 1:
            raise CheckError(f"{label}: {struct}'s attributes ({attrs}) carry a "
                             f"repr other than one plain #[repr(C)] -- this "
                             f"check computes the natural C layout only, and "
                             f"that repr changes it or makes it conditional")
        if not plain:
            raise CheckError(f"{label}: {struct} is not #[repr(C)] -- its "
                             f"layout is the compiler's choice, not the ABI's")
    fields = []
    for k, line in enumerate(strip_comments(body)):
        if not line.strip():
            continue
        if lang == "rust":
            m = RUST_FIELD.match(line)
            size = RUST_SIZES.get(m.group(2)) if m else None
            count = 1
        elif lang == "go":
            m = GO_FIELD.match(line)
            size = go_size(m.group(2)) if m else None
            count = 1
        else:
            m = C_FIELD.match(line)
            size = C_SIZES.get(" ".join(m.group(1).split())) if m else None
            count = int(m.group(3)) if m and m.group(3) else 1
        if not m or size is None:
            raise CheckError(f"{label}: cannot size line {first + k + 1}: "
                             f"{line.strip()!r}")
        name = m.group(2) if lang not in ("rust", "go") else m.group(1)
        fields.append((name, size, count, first + k))
    if not fields:
        raise CheckError(f"{label}: {struct} has no fields")
    return fields


def layout(fields):
    """[(name, offset, bytes, line_index)], total size -- natural alignment."""
    off, maxal, out = 0, 1, []
    for name, size, count, ln in fields:
        off = (off + size - 1) // size * size
        out.append((name, off, size * count, ln))
        off += size * count
        maxal = max(maxal, size)
    return out, (off + maxal - 1) // maxal * maxal


def kernel_layout(text, struct=None):
    label = "kernel"
    if struct is None:
        _, _, struct = KERNEL
    else:
        label = f"kernel {struct}"
    lay, size = layout(parse_fields(text, struct, "kernel", label))
    m = re.search(r"_Static_assert\(\s*sizeof\(\s*struct\s+" + struct
                  + r"\s*\)\s*==\s*(\d+)", text)
    if not m:
        raise CheckError(f"{label}: no sizeof assert for struct {struct}")
    asserted = {n: int(o) for n, o in re.findall(
        r"__builtin_offsetof\(\s*struct\s+" + struct
        + r"\s*,\s*(\w+)\s*\)\s*==\s*(\d+)", text)}
    errs = []
    if int(m.group(1)) != size:
        errs.append(f"{label}: laid out at {size} B but the header asserts "
                    f"{m.group(1)} -- the header disagrees with itself (it "
                    f"would not compile) or this check's layout rule is wrong")
    for name, off, _, _ in lay:
        if name not in asserted:
            errs.append(f"{label}: field {name} has no offsetof _Static_assert "
                        f"-- add one, so the layout every mirror is held to is "
                        f"the one the kernel's compiler verified")
        elif asserted[name] != off:
            errs.append(f"{label}: {name} laid out at {off} but asserted at "
                        f"{asserted[name]} -- the header disagrees with itself "
                        f"or this check's layout rule is wrong")
    for name in asserted:
        if name not in {f[0] for f in lay}:
            errs.append(f"{label}: an offsetof assert names {name}, which the "
                        f"struct does not declare")
    if errs:
        raise CheckError("\n".join(errs))
    return lay, size


def compare(label, mirror, kernel, names):
    (mf, msize), (kf, ksize) = mirror, kernel
    errs = []
    if msize != ksize:
        errs.append(f"{label}: {msize} B, the kernel's struct is {ksize} B")
    for i in range(max(len(mf), len(kf))):
        m = mf[i] if i < len(mf) else None
        k = kf[i] if i < len(kf) else None
        if m is None:
            errs.append(f"{label}: missing the kernel's {k[0]} "
                        f"(@{k[1]}, {k[2]} B)")
        elif k is None:
            errs.append(f"{label}: {m[0]} (@{m[1]}) has no kernel field")
        elif (m[1], m[2]) != (k[1], k[2]) or (names and m[0] != k[0]):
            errs.append(f"{label}: field {i} is {m[0]} @{m[1]} ({m[2]} B); "
                        f"the kernel's is {k[0]} @{k[1]} ({k[2]} B)")
    return errs


def go_types(go_text):
    """{offset: (field, type)} of the go fork's struct."""
    lines = go_text.splitlines()
    out = {}
    for name, off, _, ln in layout(parse_fields(go_text, GO_TYPE, "go", "go fork"))[0]:
        out[off] = (name, GO_FIELD.match(strip_comments([lines[ln]])[0]).group(2))
    return out


def go_va_errs(go_text, kernel):
    """Each kernel user-address field (*_va) must be pointer-typed in Go."""
    typed, errs = go_types(go_text), []
    for kname, off, _, _ in kernel[0]:
        if not kname.endswith("_va") or off not in typed:
            continue
        name, t = typed[off]
        if not go_pointer(t):
            errs.append(f"go fork: {name} @{off} carries the kernel's {kname}, a "
                        f"user address, as {t} -- make it pointer-typed (*T or "
                        f"unsafe.Pointer): the buffer can live on the goroutine "
                        f"stack, and the stack copier adjusts pointer slots only")
    return errs


def u64_class_errs(text, struct=None):
    """Every 8-byte kernel field is a user address (*_va) or listed as not one."""
    label, _, main_struct = KERNEL
    if struct is not None:
        label = f"kernel {struct}"
        wide = {name for name, size, count, _ in parse_fields(text, struct, "kernel", label)
                if size == 8 and count == 1}
        return [f"{label}: {name} is 8 bytes but not named *_va (a user address) "
                f"-- a tail carries addresses only, so name it *_va"
                for name in sorted(wide) if not name.endswith("_va")]
    struct = main_struct
    wide = {name for name, size, count, _ in parse_fields(text, struct, "kernel", label)
            if size == 8 and count == 1}
    errs = [f"{label}: {name} is 8 bytes but neither named *_va (a user address) "
            f"nor listed in NON_ADDRESS_U64 -- classify it, so that the go fork's "
            f"pointer rule covers it if it is an address"
            for name in sorted(wide)
            if not name.endswith("_va") and name not in NON_ADDRESS_U64]
    errs += [f"{label}: NON_ADDRESS_U64 lists {name}, which the struct does not "
             f"declare as an 8-byte field -- the list has gone stale"
             for name in sorted(NON_ADDRESS_U64 - wide)]
    return errs


def check(texts, go_text):
    """Every mismatch, as messages; raises CheckError on an unreadable source."""
    kernel = kernel_layout(texts["kernel"])
    errs = u64_class_errs(texts["kernel"])
    for label, _, struct, lang in MIRRORS:
        lay = layout(parse_fields(texts[label], struct, lang, label))
        errs += compare(label, lay, kernel, names=True)
    if go_text is not None:
        lay = layout(parse_fields(go_text, GO_TYPE, "go", "go fork"))
        errs += compare("go fork", lay, kernel, names=False)
        errs += go_va_errs(go_text, kernel)
    if go_text is not None:
        listed = {struct for _, mirrors in EXT_TAILS for _, struct, _ in mirrors}
        errs += [f"go fork: declares a record tail type {t} that EXT_TAILS does "
                 f"not hold to the kernel -- list it beside its kernel tail"
                 for t in GO_TAIL.findall(go_text) if t not in listed]
    for kstruct, mirrors in EXT_TAILS:
        ktail = kernel_layout(texts["kernel"], kstruct)
        errs += u64_class_errs(texts["kernel"], kstruct)
        for label, struct, lang in mirrors:
            lay = layout(parse_fields(texts[label], struct, lang, f"{label} {struct}"))
            errs += compare(f"{label} {struct}", lay, ktail, names=True)
    return kernel, errs


def fails(texts, go_text):
    try:
        return bool(check(texts, go_text)[1])
    except CheckError:
        return True


def reported(texts, go_text, needle):
    """Whether the check reports a fault naming needle -- a mutation that another
    rule happens to catch proves nothing about the rule it was aimed at."""
    try:
        return any(needle in e for e in check(texts, go_text)[1])
    except CheckError as e:
        return needle in str(e)


def edit_line(text, lang, index, fn):
    """Apply fn to line `index` of the struct's source (a patch keeps its +)."""
    lines = text.splitlines()
    if lang == "patch":
        lines[index] = lines[index][:1] + fn(lines[index][1:])
    else:
        lines[index] = fn(lines[index])
    return "\n".join(lines)


def self_test(texts, go_text):
    """Mutations this check must report; the names of those it missed."""
    missed = []
    targets = [(label, struct, lang, label) for label, _, struct, lang in MIRRORS]
    targets += [(label, struct, lang, f"{label} {struct}")
                for _, mirrors in EXT_TAILS for label, struct, lang in mirrors]
    for label, struct, lang, name in targets:
        fields = parse_fields(texts[label], struct, lang, name)
        last = fields[-1][3]
        dropped = dict(texts)
        dropped[label] = edit_line(texts[label], lang, last, lambda l: "")
        if not fails(dropped, go_text):
            missed.append(f"{name} without its last field")
        a, b = fields[-2], fields[-1]
        if a[1] == b[1]:
            swapped = edit_line(texts[label], lang, a[3],
                                lambda l: re.sub(r"\b%s\b" % a[0], b[0], l, count=1))
            swapped = edit_line(swapped, lang, b[3],
                                lambda l: re.sub(r"\b%s\b" % b[0], a[0], l, count=1))
            t = dict(texts)
            t[label] = swapped
            if not fails(t, go_text):
                missed.append(f"{name} with {a[0]} and {b[0]} swapped")
        if lang == "rust":
            packed = texts[label].replace("#[repr(C)]\npub struct " + struct,
                                          "#[repr(C, packed)]\npub struct " + struct, 1)
            t = dict(texts)
            t[label] = packed
            if packed == texts[label] or not fails(t, go_text):
                missed.append(f"{name} as #[repr(C, packed)]")
            plain_head = "#[repr(C)]\npub struct " + struct
            for shape, head in (
                    ("a cfg_attr repr(packed) above its #[repr(C)]",
                     "#[cfg_attr(all(), repr(packed))]\n" + plain_head),
                    ("a multi-line cfg_attr repr(packed) above its #[repr(C)]",
                     "#[cfg_attr(\n    all(),\n    repr(packed)\n)]\n" + plain_head)):
                t = dict(texts)
                t[label] = texts[label].replace(plain_head, head, 1)
                if t[label] == texts[label] or not fails(t, go_text):
                    missed.append(f"{name} with {shape}")
    if go_text is not None:
        fields = parse_fields(go_text, GO_TYPE, "go", "go fork")
        last = fields[-1][3]
        if not fails(texts, edit_line(go_text, "go", last, lambda l: "")):
            missed.append("go fork without its last field")
        widened = edit_line(go_text, "go", last,
                            lambda l: re.sub(r"\buint32\b", "uint64", l))
        if widened != go_text and not fails(texts, widened):
            missed.append("go fork with its last field widened")
        typed = go_types(go_text)
        vas = [(kname, off) for kname, off, _, _ in kernel_layout(texts["kernel"])[0]
               if kname.endswith("_va") and off in typed and go_pointer(typed[off][1])]
        ln = {off: f[3] for f, (_, off, _, _) in
              zip(fields, layout(fields)[0])}
        if not vas:
            missed.append("go fork address fields (none is pointer-typed to mutate)")
        stray = go_text + "\n\ntype spawnExtProbe struct {\n\tx uint32\n}\n"
        if not reported(texts, stray, "EXT_TAILS does not hold"):
            missed.append("a go fork tail type no EXT_TAILS entry lists")
        for kname, off in vas:
            integer = edit_line(go_text, "go", ln[off],
                                lambda l: re.sub(r"\*\w+|unsafe\.Pointer", "uint64", l, count=1))
            if integer == go_text or not reported(texts, integer, "a user address"):
                missed.append(f"go fork with {typed[off][0]} (the kernel's {kname}) "
                              f"as an integer")
    kfields = parse_fields(texts["kernel"], KERNEL[2], "kernel", "kernel")
    grown = edit_line(texts["kernel"], "kernel", kfields[-1][3],
                      lambda l: l + "\n    u32 unasserted_field;")
    t = dict(texts)
    t["kernel"] = grown
    if not fails(t, go_text):
        missed.append("a kernel field with no offsetof assert")
    for kstruct, _ in EXT_TAILS:
        tf = parse_fields(texts["kernel"], kstruct, "kernel", f"kernel {kstruct}")
        t = dict(texts)
        t["kernel"] = edit_line(texts["kernel"], "kernel", tf[-1][3],
                                lambda l: l + "\n    u32 unasserted_field;")
        if not fails(t, go_text):
            missed.append(f"a {kstruct} field with no offsetof assert")
        t = dict(texts)
        t["kernel"] = re.sub(r"\bcwd_va\b", "cwd_addr", texts["kernel"]) \
            if kstruct == "sys_spawn_ext_cwd" else texts["kernel"]
        if t["kernel"] == texts["kernel"] or not reported(t, go_text, "not named *_va"):
            missed.append(f"a {kstruct} address field without its _va suffix")
    first_va = next(n for n, _, _, _ in kernel_layout(texts["kernel"])[0]
                    if n.endswith("_va"))
    t = dict(texts)
    t["kernel"] = re.sub(r"\b%s\b" % first_va, first_va[:-3], texts["kernel"])
    if t["kernel"] == texts["kernel"] or not reported(t, go_text, "neither named *_va"):
        missed.append(f"a kernel address field ({first_va}) without its _va suffix")
    return missed


def main():
    texts = {KERNEL[0]: (ROOT / KERNEL[1]).read_text()}
    for label, rel, _, _ in MIRRORS:
        texts[label] = (ROOT / rel).read_text()
    gofork = pathlib.Path(os.environ.get(
        "GOFORK", str(pathlib.Path.home() / "projects" / "go-thylacine")))
    go_path = gofork / GO_REL
    go_text = go_path.read_text() if go_path.is_file() else None

    # The real comparison first: the self-test's mutations are measured against
    # a passing baseline, and on a failing one a mutation can undo the fault
    # (swapping an already-swapped pair back) and read as blindness.
    try:
        kernel, errs = check(texts, go_text)
        if errs:
            print("spawn-args mirror check: FAILED -- struct sys_spawn_args or "
                  "a mirror of it breaks a rule:", file=sys.stderr)
            for e in errs:
                print(f"  {e}", file=sys.stderr)
            return 1
        missed = self_test(texts, go_text)
    except CheckError as e:
        print(f"spawn-args mirror check: CANNOT CHECK\n{e}", file=sys.stderr)
        return 1
    if missed:
        print("spawn-args mirror check: the check is BLIND to: "
              + "; ".join(missed), file=sys.stderr)
        return 1
    checked = [m[0] for m in MIRRORS]
    tails = "; ".join(f"{k} ok ({', '.join(m[0] for m in ms)})" for k, ms in EXT_TAILS)
    goline = (f"go fork ok (offsets, sizes, pointer-typed addresses; {go_path})"
              if go_text is not None
              else f"go fork SKIPPED ({go_path} absent)")
    lay, size = kernel
    print(f"spawn-args mirror check: kernel {size} B / {len(lay)} fields, every "
          f"u64 classified; "
          f"{len(checked)} in-tree mirrors ok ({', '.join(checked)}); {goline}; "
          f"tails: {tails}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
