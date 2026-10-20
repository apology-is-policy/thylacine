#!/usr/bin/env python3
"""Gate: no two defines of a flag word share a bit.

A flag's _Static_assert names the flags its author knew, so two branches can
each take the same free bit and both compile -- it happened in proc_flags, at
bit 22. This check derives each word's set from its header instead. WORDS names
each word: the header, the pattern a member's name matches, and the word's
width. Every member is evaluated (the header's other macros resolved), and a
member OWNS the bits its own literals contribute: a reference to another member
contributes nothing, so a mask built from members (SPAWN_PERM_ALL) overlaps them
freely, while the mode bits SYS_WALK_CREATE_PERM_VALID adds by literal are its
own. A member may use another member only as an operand of |, since only then
does dropping the reference leave its own bits; any other use (a shift, an &,
a ~) cannot be classified and fails. So does a member that does not evaluate,
one outside its word, and a word with no member at all: an unread flag is not a
checked one.

A passing check is then proved able to fail: each word's header is mutated in
memory -- a new member taking an owned bit, a member built by shifting another,
a member naming an undefined macro, a member outside the word, the word's
members all renamed away -- and a mutation the check does not report, by the
rule it targets, fails the check. A checker that cannot verify itself stops the
build.

usage: check-flag-words.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROC_H = 'kernel/include/thylacine/proc.h'
SYSCALL_H = 'kernel/include/thylacine/syscall.h'
TERRITORY_H = 'kernel/include/thylacine/territory.h'

WORDS = [
    # (word, header, member-name pattern, width in bits)
    ('proc_flags',            PROC_H,      r'PROC_FLAG_\w+',       32),
    ('spawn perms',           SYSCALL_H,   r'SPAWN_PERM_\w+',      32),
    ('spawn identity flags',  SYSCALL_H,   r'SPAWN_IDENTITY_\w+',  32),
    ('spawn allowance flags', SYSCALL_H,   r'SPAWN_ALLOWANCE_\w+', 32),
    ('spawn phenotype flags', SYSCALL_H,   r'SPAWN_PHENO_\w+',     32),
    ('spawn debug flags',     SYSCALL_H,   r'SPAWN_DEBUG_\w+',     32),
    ('spawn ext flags',       SYSCALL_H,   r'SPAWN_EXT_\w+',       32),
    ('walk-create mode word', SYSCALL_H,   r'SYS_WALK_CREATE_\w+', 32),
    ('9P attach flags',       SYSCALL_H,   r'SYS_ATTACH_9P_\w+',   32),
    ('mount flags',           TERRITORY_H, r'M[A-Z]+(?:_[A-Z]+)*', 32),
]

NUMBER = re.compile(r'\b(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]{0,2}\b')
CAST = re.compile(r'\(\s*(u8|u16|u32|u64|uint32_t|uint64_t|unsigned)\s*\)')
IDENT = re.compile(r'[A-Za-z_]\w*')


def load_defines(text):
    text = re.sub(r'\\\n', ' ', text)
    defs = {}
    for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+)$',
                         text, re.M):
        body = re.sub(r'//.*$', '', m.group(2))
        body = re.sub(r'/\*.*?\*/', '', body).strip()
        if body:
            defs[m.group(1)] = body
    return defs


def only_ored(name, body, used):
    """Whether each name in `used` appears in body only as an operand of |."""
    flat = re.sub(r'[()]', ' ', CAST.sub(' ', body))
    for m in IDENT.finditer(flat):
        if m.group(0) not in used:
            continue
        left = flat[:m.start()].rstrip()
        right = flat[m.end():].lstrip()
        if (left and not left.endswith('|')) or (right and not right.startswith('|')):
            return False
    return True


def evaluate(name, defs, zero, memo, stack=()):
    """The value of macro `name`, with every name in `zero` read as 0."""
    if name in zero:
        return 0
    if name in memo:
        return memo[name]
    if name in stack:
        raise ValueError('%s: recursive definition' % name)
    body = defs[name]
    used = set(IDENT.findall(NUMBER.sub(' ', CAST.sub(' ', body)))) & zero
    if used and not only_ored(name, body, used):
        raise ValueError('%s uses %s other than as an operand of |, so its own '
                         'bits cannot be told apart' % (name, ', '.join(sorted(used))))
    expr = NUMBER.sub(lambda m: str(int(m.group(1), 0)), CAST.sub('', body))
    for tok in sorted(set(IDENT.findall(expr)), key=len, reverse=True):
        if tok not in defs:
            raise ValueError('%s references %s, which its header does not define'
                             % (name, tok))
        val = evaluate(tok, defs, zero, memo, stack + (name,))
        expr = re.sub(r'\b%s\b' % tok, '(%d)' % val, expr)
    if not re.fullmatch(r'[\s\d()|&^~<>+\-*]+', expr):
        raise ValueError('%s: cannot evaluate %r' % (name, body))
    try:
        val = eval(expr, {'__builtins__': {}}, {})
    except SyntaxError:
        raise ValueError('%s: cannot evaluate %r' % (name, body))
    memo[name] = val
    return val


def bit_list(v):
    return [i for i in range(v.bit_length()) if v >> i & 1]


def ranges(bits):
    out, run = [], []
    for b in bits:
        if run and b == run[-1] + 1:
            run.append(b)
            continue
        if run:
            out.append(str(run[0]) if len(run) == 1 else '%d-%d' % (run[0], run[-1]))
        run = [b]
    if run:
        out.append(str(run[0]) if len(run) == 1 else '%d-%d' % (run[0], run[-1]))
    return ','.join(out) or 'none'


def check_word(word, defs, pattern, width):
    """(summary line, [errors]) for one word, each error prefixed by the word."""
    members = sorted(n for n in defs if re.fullmatch(pattern, n))
    if not members:
        return None, ['%s: no define matches %s' % (word, pattern)]
    errs, owned_by, masks = [], {}, 0
    for n in members:
        try:
            full = evaluate(n, defs, frozenset(), {})
            own = evaluate(n, defs, frozenset(members) - {n}, {})
        except ValueError as e:
            errs.append('%s: %s' % (word, e))
            continue
        if full <= 0 or full >= 1 << width:
            errs.append('%s: %s = %#x is outside the %d-bit word'
                        % (word, n, full, width))
            continue
        if own == 0:
            masks += 1
        for b in bit_list(own):
            if b in owned_by:
                errs.append('%s: bit %d is taken by both %s and %s'
                            % (word, b, owned_by[b], n))
            else:
                owned_by[b] = n
    free = [b for b in range(width) if b not in owned_by]
    line = ('%s: %d defines (%d built from members), %d bits owned, free %s'
            % (word, len(members), masks, len(owned_by), ranges(free)))
    return line, errs


def check(texts):
    defs = {h: load_defines(t) for h, t in texts.items()}
    lines, errs = [], []
    for word, header, pattern, width in WORDS:
        line, e = check_word(word, defs[header], pattern, width)
        if line:
            lines.append(line)
        errs += e
    return lines, errs


def reported(texts, needle):
    """Whether the check reports a fault naming needle -- a mutation that another
    rule happens to catch proves nothing about the rule it was aimed at."""
    return any(needle in e for e in check(texts)[1])


def self_test(texts):
    missed = []
    for word, header, pattern, width in WORDS:
        defs = load_defines(texts[header])
        members = sorted(n for n in defs if re.fullmatch(pattern, n))
        prefix = re.match(r'[A-Z0-9_]*', pattern).group(0)
        probe = prefix + 'SELFTEST'
        if not re.fullmatch(pattern, probe) or not members:
            missed.append('%s (no probe name or no member to mutate)' % word)
            continue
        owned = sorted(b for n in members
                       for b in bit_list(evaluate(n, defs, frozenset(members) - {n}, {})))
        first = next(n for n in members
                     if evaluate(n, defs, frozenset(members) - {n}, {}))
        cases = [
            ('a new member on bit %d' % owned[0],
             '#define %s 0x%xu' % (probe, 1 << owned[0]),
             '%s: bit %d is taken by both' % (word, owned[0])),
            ('a member shifted from another',
             '#define %s (%s << 1)' % (probe, first),
             '%s: %s uses %s other than' % (word, probe, first)),
            ('a member naming an undefined macro',
             '#define %s (1u << SELFTEST_UNDEFINED)' % probe,
             '%s: %s references SELFTEST_UNDEFINED' % (word, probe)),
            ('a member outside the word',
             '#define %s 0x%xu' % (probe, 1 << width),
             '%s: %s = %#x is outside' % (word, probe, 1 << width)),
        ]
        for what, line, needle in cases:
            mutated = dict(texts)
            mutated[header] = texts[header] + '\n' + line + '\n'
            if not reported(mutated, needle):
                missed.append('%s: %s' % (word, what))
        gone = dict(texts)
        gone[header] = re.sub(r'(#[ \t]*define[ \t]+)(%s)\b' % pattern,
                              r'\1SELFTEST_GONE_\2', texts[header])
        if not reported(gone, '%s: no define matches' % word):
            missed.append('%s: every member renamed away' % word)
    return missed


def main():
    texts = {h: (ROOT / h).read_text()
             for h in sorted({w[1] for w in WORDS})}
    # The real check first: the self-test's mutations are measured against a
    # passing baseline, and on a failing one a mutation can read as caught
    # because of the fault already there.
    lines, errs = check(texts)
    for line in lines:
        print('check-flag-words: ' + line)
    if errs:
        for e in errs:
            print('check-flag-words: FAIL: ' + e, file=sys.stderr)
        return 1
    missed = self_test(texts)
    if missed:
        print('check-flag-words: the check is BLIND to: ' + '; '.join(missed),
              file=sys.stderr)
        return 1
    print('check-flag-words: %d words ok; the self-test caught all %d mutations'
          % (len(WORDS), 5 * len(WORDS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
