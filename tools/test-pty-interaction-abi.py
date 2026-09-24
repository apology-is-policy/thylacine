#!/usr/bin/env python3
"""Compile kernel C, libt C and Rust mirrors against one frozen byte record.

Run on a little-endian AArch64 host (the C header contains native register
names). No guest syscall executes: C objects contribute an ELF data section,
and Rust links only the standalone, syscall-free record module.
"""
from pathlib import Path
import os
import platform
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
if platform.machine().lower() not in ('aarch64', 'arm64') or sys.byteorder != 'little':
    raise SystemExit('requires a little-endian AArch64 host')

# The fixed oracle is independent of the language declarations and their offsets.
EXPECTED = bytes.fromhex('''
1000000000000000 1100000000000000 1200000000000000 1300000000000000
1400000000000000 1500000000000000 0100000000000000 5000000000000000
1800000000000000 0100000000000000 0200000000000000 ffffffffffffff7f
01000000 03000000 0807060504030201 1817161514131211
2827262524232221 3837363534333231 4847464544434241
54535251 64636261 7877767574737271 8887868584838281 94939291 00000000
01000000 18000000 a8a7a6a5a4a3a2a1 b8b7b6b5b4b3b2b1
''')
NAMES = '''BIND UNBIND WATCH STATE ACK CHECK VERSION STATE_BYTES CHECK_BYTES
LIVE ACKNOWLEDGED ID_MAX'''.split()
STATE = '''version=1 flags=3 binding_id=0x0102030405060708
pts_id=0x1112131415161718 foreground_epoch=0x2122232425262728
acknowledged_epoch=0x3132333435363738 revision=0x4142434445464748
controlling_sid=0x51525354 foreground_pgid=0x61626364
subject_stripes=0x7172737475767778 binder_stripes=0x8182838485868788
binder_pid=0x91929394 reserved=0'''.split()
CHECK = '''version=1 size=24 expected_epoch=0xa1a2a3a4a5a6a7a8
subject_stripes=0xb1b2b3b4b5b6b7b8'''.split()


def c_fields(fields):
    return ', '.join('.' + item.replace('=', ' = ') + 'ULL' for item in fields)


def rust_fields(fields):
    return ', '.join(item.replace('=', ': ') for item in fields)


def elf_section(path, name):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise RuntimeError('C fixture must be ELF64 little-endian (use Pi)')
    if struct.unpack_from('<H', data, 18)[0] != 183:
        raise RuntimeError('C fixture is not AArch64')
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count, string_index = struct.unpack_from('<HHH', data, 58)
    headers = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride)
               for i in range(count)]
    string_header = headers[string_index]
    strings = data[string_header[4]:string_header[4] + string_header[5]]
    for index, header in enumerate(headers):
        label = strings[header[0]:].split(b'\0', 1)[0].decode()
        if label == name:
            if any(h[1] in (4, 9) and h[7] == index and h[5] for h in headers):
                raise RuntimeError('fixture must not contain relocations')
            return data[header[4]:header[4] + header[5]]
    raise RuntimeError('fixture section missing')


with tempfile.TemporaryDirectory(prefix='pty-abi-') as directory:
    tmp = Path(directory)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    for label, header, prefix, include in [
        ('kernel', 'thylacine/syscall.h', '', 'kernel/include'),
        ('libt', 'thyla/syscall.h', 'T_', 'usr/lib/libt/include'),
    ]:
        constants = ', '.join(prefix + 'PTY_INTERACTION_' + name for name in NAMES)
        source = f'''#include <{header}>
_Static_assert({prefix}SYS_PTY_REGISTER == 93, "syscall number");
struct fixture {{
    unsigned long long operations[12];
    struct t_pty_interaction_state state;
    struct t_pty_interaction_check check;
}};
_Static_assert(sizeof(struct fixture) == 200, "fixture has no padding");
__attribute__((used, section(".pty_abi_fixture")))
const struct fixture fixture = {{
    .operations = {{ {constants} }},
    .state = {{ {c_fields(STATE)} }},
    .check = {{ {c_fields(CHECK)} }}
}};
'''
        src, obj = tmp / (label + '.c'), tmp / (label + '.o')
        src.write_text(source)
        subprocess.run(cc + ['-std=c11', '-ffreestanding', '-Wall', '-Wextra',
                             '-Werror', '-I', str(ROOT / include), '-c', str(src),
                             '-o', str(obj)], check=True)
        actual = elf_section(obj, '.pty_abi_fixture')
        if actual != EXPECTED:
            raise SystemExit(f'{label}: byte fixture differs: {actual.hex()}')
        print(f'{label}: constants, all offsets/alignment and 200-byte fixture PASS')

    # Only the mirrored definitions are linked; the native runtime is not run
    # on a Linux host. Raw bytes are safe here: every field is initialized and
    # the production declarations pin all offsets, sizes and no-padding layout.
    module = ROOT / 'usr/lib/libthyla-rs/src/pty_interaction.rs'
    constants = ', '.join('T_PTY_INTERACTION_' + name for name in NAMES)
    src, exe = tmp / 'fixture.rs', tmp / 'fixture'
    src.write_text(f'''
#[path = "{module}"] mod abi;
use abi::*;
use std::io::Write;
fn main() {{
    let operations: [u64; 12] = [{constants}];
    let state = TPtyInteractionState {{ {rust_fields(STATE)} }};
    let check = TPtyInteractionCheck {{ {rust_fields(CHECK)} }};
    let mut out = std::io::stdout().lock();
    for op in operations {{ out.write_all(&op.to_le_bytes()).unwrap(); }}
    unsafe {{
        out.write_all(std::slice::from_raw_parts((&state as *const TPtyInteractionState).cast::<u8>(), 80)).unwrap();
        out.write_all(std::slice::from_raw_parts((&check as *const TPtyInteractionCheck).cast::<u8>(), 24)).unwrap();
    }}
}}
''')
    subprocess.run(shlex.split(os.environ.get('RUSTC', 'rustc')) +
                   ['--edition=2021', '-Dwarnings', str(src), '-o', str(exe)], check=True)
    actual = subprocess.check_output([str(exe)])
    if actual != EXPECTED:
        raise SystemExit(f'Rust: byte fixture differs: {actual.hex()}')
    print('Rust: constants, all offsets/alignment and 200-byte fixture PASS')
print('Reservation only: no kernel authority, poll/lifetime or guest behavior tested.')
