#!/usr/bin/env python3
"""Compare UA wire reservation constants and byte-array layouts (no build)."""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
rust = (root / 'usr/lib/corvus-authority/src/abi.rs').read_text()
c = (root / 'kernel/include/thylacine/authority_wire.h').read_text()
rs = {k: int(v, 0) for k, v in re.findall(r'pub const (\w+): \w+ = (0x[\da-f]+|\d+);', rust)}
cs = {k: int(v, 0) for k, v in re.findall(r'#define UA_(\w+) (0x[\da-f]+|\d+)u(?:ll)?\b', c)}
assert rs and rs == cs, (rs.keys() ^ cs.keys(), [(k, v, cs.get(k)) for k, v in rs.items() if cs.get(k) != v])
for name, total in [('MandateHeader', 96), ('EnvelopeHeader', 32)]:
    rb = re.search(r'pub struct ' + name + r' \{([^}]+)\}', rust).group(1)
    cb = re.search(r'struct Ua' + name + r' \{([^}]+)\}', c).group(1)
    rf = re.findall(r'pub (\w+): \[u8; (\d+)\]', rb)
    cf = re.findall(r'unsigned char (\w+)\[(\d+)\]', cb)
    assert rf == cf and sum(int(n) for _, n in rf) == total, name
    offset = 0
    for field, count in rf:
        assert f'offset_of!({name}, {field}) == {offset}' in rust, field
        assert f'__builtin_offsetof(struct Ua{name}, {field}) == {offset}' in c, field
        offset += int(count)
print(f'authority ABI: {len(rs)} constants and two complete byte layouts match')
