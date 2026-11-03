#!/usr/bin/env python3
"""Compile the kernel C, libt C and Rust account mirrors against one byte record."""
from pathlib import Path
import os
import re
import shlex
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
cc = shlex.split(os.environ.get('CC', '/opt/homebrew/opt/llvm/bin/clang --target=aarch64-none-elf'))
objcopy = os.environ.get('OBJCOPY', '/opt/homebrew/opt/llvm/bin/llvm-objcopy')

# The oracle is independent of every declaration: packed here by hand, field by field.
hdr = struct.pack('<IIIIQ', 1, 168, 0x0a, 0x0b, 0x0102030405060708)
classes = b''.join(struct.pack('<QQQ', 0x10 + i, 0x20 + i, 0x30 + i) for i in range(4))
query = hdr + struct.pack('<QIIQQQQ', 0x1112131415161718, 2, 3, 0x2122232425262728,
                          0x3132333435363738, 0x4142434445464748, 0x5152535455565758) + classes
outcome = struct.pack('<QIIQQQ', 0x6162636465666768, 1, 9, 0x7172737475767778,
                      0x8182838485868788, 0x9192939495969798)
assert len(query) == 168 and len(outcome) == 40
constants = struct.pack('<Q22i', 140, 1, -1, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 0, 1, 2, 1, 5, 9, 0, 0, 0)
EXPECTED = constants + query + outcome


def values(p):
    """The C spelling of the fixture. Designated, so a swap of two fields shows."""
    return f'''{{ {p}SYS_ACCOUNT, {{ (int){p}ACCOUNT_VERSION, {p}ACCOUNT_SELF,
  {p}ACCOUNT_QUERY, {p}ACCOUNT_CHILD, {p}ACCOUNT_SET_LIMITS, {p}VOUCHER_CREATE, {p}VOUCHER_CLOSE, {p}ALLOCATE_FUNDED,
  {p}CLASS_ALLOC, {p}CLASS_RETAIN, {p}CLASS_META, {p}CLASS_DEVICE, {p}ACCOUNT_CLASSES,
  {p}ACCOUNT_NORMAL, {p}ACCOUNT_WARNING, {p}ACCOUNT_CRITICAL,
  {p}REASON_ACCOUNT_LIMIT, {p}REASON_DEVICE_BUDGET, {p}REASON_STALE_GRANT, 0, 0, 0 }},
 {{ .h = {{ .version = 1, .size = 168, .flags = 0x0a, .pad = 0x0b, .incarnation = 0x0102030405060708ull }},
   .generation = 0x1112131415161718ull, .level = 2, .limiting = 3, .headroom = 0x2122232425262728ull,
   .reclaim_target = 0x3132333435363738ull, .retained_after_exit = 0x4142434445464748ull,
   .awaiting_device = 0x5152535455565758ull,
   .c = {{ {{.used = 0x10, .reserved = 0x20, .limit = 0x30}}, {{.used = 0x11, .reserved = 0x21, .limit = 0x31}},
          {{.used = 0x12, .reserved = 0x22, .limit = 0x32}}, {{.used = 0x13, .reserved = 0x23, .limit = 0x33}} }} }},
 {{ .request_id = 0x6162636465666768ull, .cls = 1, .reason = 9, .requested = 0x7172737475767778ull,
   .available = 0x8182838485868788ull, .limiting_account = 0x9192939495969798ull }} }}'''


with tempfile.TemporaryDirectory(prefix='account-abi-') as tmp:
    tmp = Path(tmp)
    for name, include, headers, sprefix, p in [
        ('kernel', 'kernel/include', '#include <thylacine/syscall.h>\n#include <thylacine/account.h>', '', ''),
        ('libt', 'usr/lib/libt/include', '#include <thyla/syscall.h>\n#include <thyla/account.h>', 't_', 'T_'),
    ]:
        (tmp / 'fixture.c').write_text(f'''{headers}
struct fixture {{ unsigned long long sysno; int constants[22];
  struct {sprefix}account_query query; struct {sprefix}account_outcome outcome; }};
__attribute__((used, section(".account_abi"))) const struct fixture fixture = {values(p)};
''')
        subprocess.run(cc + ['-std=c11', '-ffreestanding', '-Werror', '-I', str(root / include), '-I',
                             str(root / 'arch/arm64'), '-c', str(tmp / 'fixture.c'), '-o', str(tmp / 'fixture.o')],
                       check=True)
        subprocess.run([objcopy, '--dump-section', f'.account_abi={tmp}/bytes', str(tmp / 'fixture.o')], check=True)
        got = (tmp / 'bytes').read_bytes()
        assert got == EXPECTED, f'{name}: {got.hex()} != {EXPECTED.hex()}'
        print(f'{name}: number, constants and record bytes PASS', flush=True)

    lib = (root / 'usr/lib/libthyla-rs/src/lib.rs').read_text()
    sysno = re.search(r'pub const T_SYS_ACCOUNT: [^;]+;', lib)[0]
    (tmp / 'fixture.rs').write_text(f'''{sysno}
#[path = "{root}/usr/lib/libthyla-rs/src/account.rs"]
mod account;
use account::*;
fn main() {{
 use std::io::Write;
 let mut o = std::io::stdout();
 o.write_all(&T_SYS_ACCOUNT.to_le_bytes()).unwrap();
 let constants: [i32; 22] = [ACCOUNT_VERSION as i32, ACCOUNT_SELF,
  ACCOUNT_QUERY as i32, ACCOUNT_CHILD as i32, ACCOUNT_SET_LIMITS as i32, VOUCHER_CREATE as i32, VOUCHER_CLOSE as i32, ALLOCATE_FUNDED as i32,
  CLASS_ALLOC as i32, CLASS_RETAIN as i32, CLASS_META as i32, CLASS_DEVICE as i32, ACCOUNT_CLASSES as i32,
  ACCOUNT_NORMAL as i32, ACCOUNT_WARNING as i32, ACCOUNT_CRITICAL as i32,
  REASON_ACCOUNT_LIMIT as i32, REASON_DEVICE_BUDGET as i32, REASON_STALE_GRANT as i32, 0, 0, 0];
 for c in constants {{ o.write_all(&c.to_le_bytes()).unwrap(); }}
 let c = |i: u64| AccountClass {{ used: 0x10 + i, reserved: 0x20 + i, limit: 0x30 + i }};
 let q = AccountQuery {{
  h: AccountRecHdr {{ version: 1, size: 168, flags: 0x0a, pad: 0x0b, incarnation: 0x0102030405060708 }},
  generation: 0x1112131415161718, level: 2, limiting: 3, headroom: 0x2122232425262728,
  reclaim_target: 0x3132333435363738, retained_after_exit: 0x4142434445464748, awaiting_device: 0x5152535455565758,
  c: [c(0), c(1), c(2), c(3)] }};
 let r = AccountOutcome {{ request_id: 0x6162636465666768, cls: 1, reason: 9, requested: 0x7172737475767778,
  available: 0x8182838485868788, limiting_account: 0x9192939495969798 }};
 o.write_all(unsafe {{ core::slice::from_raw_parts((&q as *const AccountQuery).cast::<u8>(), 168) }}).unwrap();
 o.write_all(unsafe {{ core::slice::from_raw_parts((&r as *const AccountOutcome).cast::<u8>(), 40) }}).unwrap();
}}
''')
    subprocess.run(['rustc', '--edition=2021', '-A', 'dead_code', str(tmp / 'fixture.rs'), '-o', str(tmp / 'fixture')],
                   check=True)
    got = subprocess.check_output([str(tmp / 'fixture')])
    assert got == EXPECTED, f'Rust: {got.hex()} != {EXPECTED.hex()}'
    print('Rust: number, constants and record bytes PASS', flush=True)
