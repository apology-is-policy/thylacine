#!/usr/bin/env python3
"""Compile the three actual AS-0 ABI mirrors and compare independent byte vectors;
then assert the kernel, native C and Rust ring records, and the client cores' own
copies of the setup record and the CQE, against one layout table.

Blind to syscall decoding, permission checks, runtime cancellation and races.
Run under the repository's host-resource lease. Temporary binaries are removed.
"""
from pathlib import Path
import argparse, os, shlex, shutil, struct, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
CONSTANTS = [
    ('SERVICE_ABI_VERSION', 1), ('SETUP_PRIVATE_SERVICE', 4),
    ('REGISTER_SERVICE_TARGET', 2), ('REGISTER_SERVICE_SLOT', 3),
    ('REGISTER_ABORT_SCOPE', 4), ('REGISTER_QUERY_SERVICE_SLOT', 5),
    ('REGISTER_REAP_SERVICE_SLOT', 6), ('OP_SERVICE_CONNECT', 20),
    ('SERVICE_SLOT_COUNT', 64), ('SERVICE_TARGET', 1), ('SERVICE_SCOPE', 2),
    ('SERVICE_FID', 3), ('SERVICE_EMPTY', 0), ('SERVICE_RESERVED', 1),
    ('SERVICE_ADMITTED', 2), ('SERVICE_VERSION', 3), ('SERVICE_ATTACH', 4),
    ('SERVICE_READY', 5), ('SERVICE_ABORTING', 6), ('SERVICE_RETIRED', 7),
    ('SERVICE_LOCAL_RETIRED', 1), ('SERVICE_BYTES_SENT', 2),
    ('SETUP_SERVICE_BUFFERS', 8), ('REGISTER_SERVICE_POOL', 7),
    ('REGISTER_RETURN_SERVICE_BUFFER', 8), ('REGISTER_QUERY_SERVICE_POOL', 9),
    ('SERVICE_POOL', 4), ('SERVICE_POOL_MEMBERS', 64),
    ('SQE_BUFFER_SELECT', 16), ('CQE_SERVICE_BUFFER', 4),
]
# Independent wire construction; no parsed header/generator input.
def expected():
    ref = struct.pack('<IIQ', 17, 0, 0x1020304050607080)
    target = struct.pack('<IHH iI', 288, 1, 0, 31, 3) + b'abc' + bytes(253) + ref
    reserve = struct.pack('<IHHII', 48, 1, 0, 3, 0) + ref + ref
    control = struct.pack('<IHH', 32, 1, 0) + ref + bytes(8)
    snapshot = struct.pack('<IHH', 64, 1, 0) + ref + struct.pack('<II', 2, 7) + ref + struct.pack('<IIiI', 3, 2, -125, 1)
    member = struct.pack('<IIQQ', 7, 0, 0x0102030405060708, 4096)
    pool = (struct.pack('<IHHII', 1568, 1, 0, 2, 0) + ref + member
            + struct.pack('<IIQQ', 5, 0, 96, 2048) + bytes(62 * 24))
    receipt = ref + struct.pack('<IIQ', 1, 0, 0x8899aabbccddeeff)
    returned = struct.pack('<IHH', 40, 1, 0) + receipt
    pool_snapshot = (struct.pack('<IHH', 64, 1, 0) + ref
                     + struct.pack('<IIIIIIQQ', 2, 0, 0, 0, 2, 1, 0, 0))
    return [struct.pack('<' + 'I' * len(CONSTANTS), *(v for _, v in CONSTANTS)),
            ref, target, reserve, control, snapshot, member, pool, receipt,
            returned, pool_snapshot]

# The ring records and constants beside the three Loom syscalls, written here from
# the ABI rather than parsed from either header. Every field is listed, so a field
# the two headers order, size or add differently cannot pass both.
RING_CONSTANTS = ([('MAX_ENTRIES', 4096), ('SETUP_SQPOLL', 1), ('SETUP_CQSIZE', 2),
                   ('ENTER_GETEVENTS', 1), ('ENTER_NONBLOCK', 2), ('REGISTER_HANDLES', 0),
                   ('REGISTER_BUFFERS', 1), ('RING_SQ_NEED_WAKEUP', 1), ('SQE_LINK', 1),
                   ('SQE_DRAIN', 2), ('SQE_CQE_SKIP', 4), ('SQE_MULTISHOT', 8), ('CQE_MORE', 1)]
                  + [('OP_' + n, i) for i, n in enumerate(
                      'NOP WALK LOPEN LCREATE READ WRITE GETATTR SETATTR READDIR FSYNC CLUNK '
                      'RENAMEAT UNLINKAT MKDIR SYMLINK LINK MKNOD READLINK STATFS'.split())])
# The errno both sides answer for a full table: the kernel's slot table, pool
# bank and request table, and the native C client cores (T_LOOM_CLIENT_ENOSPC).
ENOSPC = 28
RING_RECORDS = [  # (struct, size, [(field, bytes, alignment)])
    ('loom_params', 88, [('flags', 4, 4), ('sq_entries', 4, 4), ('cq_entries', 4, 4),
                         ('ring_size', 4, 4), ('ring_va', 8, 8), ('hdr_off', 4, 4),
                         ('sq_array_off', 4, 4), ('sqe_off', 4, 4), ('cqe_off', 4, 4),
                         ('sq_array_size', 4, 4), ('sqe_size', 4, 4), ('cqe_size', 4, 4),
                         ('_resv0', 4, 4), ('_resv1', 32, 8)]),
    ('loom_sqe', 64, [('opcode', 1, 1), ('flags', 1, 1), ('_resv0', 2, 2), ('handle_idx', 4, 4),
                      ('offset', 8, 8), ('len', 4, 4), ('buf_idx_or_off', 4, 4),
                      ('user_data', 8, 8), ('_resv1', 32, 8)]),
    ('loom_cqe', 16, [('user_data', 8, 8), ('result', 4, 4), ('flags', 4, 4)]),
    ('loom_ring_hdr', 64, [('sq_head', 4, 4), ('sq_tail', 4, 4), ('sq_mask', 4, 4),
                           ('sq_entries', 4, 4), ('cq_head', 4, 4), ('cq_tail', 4, 4),
                           ('cq_mask', 4, 4), ('cq_entries', 4, 4), ('flags', 4, 4),
                           ('dropped', 4, 4), ('overflow', 4, 4), ('_pad', 20, 4)]),
    ('loom_buf_reg', 16, [('va', 8, 8), ('len', 8, 8)]),
]

# The client cores keep their own copies of two ring records (the setup record they
# validate and the CQE they read), checked field by field under their own names.
# (C struct, Rust path in loom-client, table record, {table field: twin field})
TWINS = [('t_owned_setup', 'owned_mapping::Setup', 'loom_params', {'_resv0': 'reserved', '_resv1': 'receipt'}),
         ('t_owned_cqe', 'paired_cq::Completion', 'loom_cqe', {})]
RUST_RECORDS = {'loom_params': 'Params', 'loom_sqe': 'Sqe', 'loom_cqe': 'Cqe',
                'loom_ring_hdr': 'RingHdr', 'loom_buf_reg': 'BufReg'}

def rust_constant(name):
    for prefix, module in (('SQE_', 'sqe_flag::'), ('OP_', 'op::')):
        if name.startswith(prefix): return 'abi::' + module + name[len(prefix):]
    return 'abi::' + name

def ring_layout(fields):
    off, align, out = 0, 1, []
    for name, size, a in fields:
        if off % a: raise AssertionError(f'layout table leaves a gap before {name}')
        out.append((name, off, size)); off += size; align = max(align, a)
    if off % align: raise AssertionError('layout table leaves tail padding')
    return out, off

def ring_source(kernel):
    prefix, macro = ('', 'LOOM_') if kernel else ('t_', 'T_LOOM_')
    lines = ['#include <' + ('thylacine/loom.h' if kernel else 'thyla/loom.h') + '>']
    if kernel:
        lines += ['#include <thylacine/errno.h>',
                  f'_Static_assert(T_E_NOSPC == {ENOSPC}, "T_E_NOSPC");']
    else:
        lines += ['#include <thyla/loom_client/owned_mapping.h>',
                  '#include <thyla/loom_client/retirement_executor.h>',
                  f'_Static_assert(T_LOOM_CLIENT_ENOSPC == -{ENOSPC}, "T_LOOM_CLIENT_ENOSPC");']
        records = dict((n, f) for n, _, f in RING_RECORDS)
        sizes = dict((n, z) for n, z, _ in RING_RECORDS)
        for twin, _, record, rename in TWINS:
            layout, _ = ring_layout(records[record])
            lines.append(f'_Static_assert(sizeof(struct {twin}) == {sizes[record]}, "{twin} size");')
            for field, off, n in layout:
                f = rename.get(field, field)
                lines.append(f'_Static_assert(__builtin_offsetof(struct {twin}, {f}) == {off} && '
                             f'sizeof(((struct {twin} *)0)->{f}) == {n}, "{twin}.{f}");')
    for name, value in RING_CONSTANTS:
        lines.append(f'_Static_assert({macro}{name} == {value}, "{macro}{name}");')
    for name, size, fields in RING_RECORDS:
        layout, total = ring_layout(fields)
        if total != size: raise AssertionError(f'layout table for {name} sums to {total}')
        s = f'struct {prefix}{name}'
        lines.append(f'_Static_assert(sizeof({s}) == {size}, "{s} size");')
        for field, off, n in layout:
            lines.append(f'_Static_assert(__builtin_offsetof({s}, {field}) == {off} && '
                         f'sizeof((({s} *)0)->{field}) == {n}, "{s}.{field}");')
    return '\n'.join(lines) + '\n'

def rust_ring_source(root):
    """The Rust ring mirror and the Rust cores' twins, against the same table."""
    abi = root / 'usr/lib/libthyla-rs/src/loom/ring_abi.rs'
    client = root / 'usr/lib/loom-client/src'
    out = ['#![allow(dead_code)]', f'#[path="{abi}"] mod abi;']
    out += [f'#[path="{client / m}.rs"] mod {m};' for m in ('service_abi', 'paired_cq', 'owned_mapping')]
    out += ['fn field<T, F>(_: fn(&T) -> &F) -> usize { core::mem::size_of::<F>() }',
            'fn check(what: &str, got: usize, want: usize) { assert_eq!(got, want, "{what}"); println!("{what} {got}"); }',
            'fn main() {']
    for name, value in RING_CONSTANTS:
        out.append(f' check("{name}", {rust_constant(name)} as usize, {value});')
    records = dict((n, (z, f)) for n, z, f in RING_RECORDS)
    checks = [(f'abi::{RUST_RECORDS[n]}', n, {}) for n, _, _ in RING_RECORDS]
    checks += [(path, record, rename) for _, path, record, rename in TWINS]
    for path, record, rename in checks:
        size, fields = records[record]
        layout, _ = ring_layout(fields)
        out.append(f' check("{path} size", core::mem::size_of::<{path}>(), {size});')
        for field, off, n in layout:
            f = rename.get(field, field)
            out.append(f' check("{path}.{f}", core::mem::offset_of!({path}, {f}), {off});')
            out.append(f' check("{path}.{f} size", field(|s: &{path}| &s.{f}), {n});')
    out.append('}')
    return '\n'.join(out) + '\n', len(checks), sum(line.startswith(' check(') for line in out)

def c_source(kernel):
    prefix = '' if kernel else 't_'
    macro = 'LOOM_' if kernel else 'T_LOOM_'
    header = 'thylacine/loom_service_abi.h' if kernel else 'thyla/loom_service.h'
    constants = ','.join(macro + name for name, _ in CONSTANTS)
    return f'''#include <{header}>
#include <stdio.h>
static void emit(const void *p, unsigned long n) {{
 const unsigned char *b=p; for(unsigned long i=0;i<n;i++) printf("%02x",b[i]); puts("");
}}
int main(void) {{
 unsigned int constants[]={{ {constants} }};
 struct {prefix}loom_service_ref r={{17,0,0x1020304050607080ULL}};
 struct {prefix}loom_service_target t={{0}}; t.size=288; t.version=1; t.registry_fd=31; t.name_len=3; t.name[0]='a'; t.name[1]='b'; t.name[2]='c'; t.result=r;
 struct {prefix}loom_service_reserve v={{0}}; v.size=48; v.version=1; v.kind=3; v.scope=r; v.result=r;
 struct {prefix}loom_service_control c={{0}}; c.size=32; c.version=1; c.object=r;
 struct {prefix}loom_service_snapshot s={{0}}; s.size=64; s.version=1; s.object=r; s.kind=2; s.state=7; s.scope=r; s.active_ops=3; s.pending_terminals=2; s.reason=-125; s.status_flags=1;
 struct {prefix}loom_service_pool_member m={{7,0,0x0102030405060708ULL,4096}};
 struct {prefix}loom_service_pool_create p={{0}}; p.size=1568; p.version=1; p.count=2; p.result=r; p.members[0]=m; p.members[1].buffer_index=5; p.members[1].offset=96; p.members[1].length=2048;
 struct {prefix}loom_service_buffer_receipt q={{0}}; q.pool=r; q.member=1; q.lease=0x8899aabbccddeeffULL;
 struct {prefix}loom_service_buffer_return z={{0}}; z.size=40; z.version=1; z.receipt=q;
 struct {prefix}loom_service_pool_snapshot ps={{0}}; ps.size=64; ps.version=1; ps.pool=r; ps.members=2; ps.leased=2; ps.streams=1;
 emit(constants,sizeof constants); emit(&r,sizeof r); emit(&t,sizeof t); emit(&v,sizeof v); emit(&c,sizeof c); emit(&s,sizeof s);
 emit(&m,sizeof m); emit(&p,sizeof p); emit(&q,sizeof q); emit(&z,sizeof z); emit(&ps,sizeof ps);
 return 0;
}}
'''

def rust_source(module):
    constants = ','.join('abi::' + name for name, _ in CONSTANTS)
    return f'''#[path={str(module)!r}] mod abi;
fn emit<T>(v:&T) {{ let b=unsafe{{std::slice::from_raw_parts(v as *const T as *const u8, std::mem::size_of::<T>())}}; for x in b {{print!("{{:02x}}",x);}} println!(); }}
fn main() {{
 let constants:[u32;{len(CONSTANTS)}]=[{constants}];
 let r=abi::SlotRef{{slot:17,reserved:0,incarnation:0x1020304050607080}};
 let mut t=abi::Target{{size:288,version:1,flags:0,registry_fd:31,name_len:3,name:[0;256],result:r}}; t.name[..3].copy_from_slice(b"abc");
 let v=abi::Reservation{{size:48,version:1,flags:0,kind:3,reserved:0,scope:r,result:r}};
 let c=abi::Control{{size:32,version:1,flags:0,object:r,reserved:0}};
 let s=abi::Snapshot{{size:64,version:1,flags:0,object:r,kind:2,state:7,scope:r,active_ops:3,pending_terminals:2,reason:-125,status_flags:1}};
 let m=abi::PoolMember{{buffer_index:7,reserved:0,offset:0x0102030405060708,length:4096}};
 let empty=abi::PoolMember{{buffer_index:0,reserved:0,offset:0,length:0}};
 let mut p=abi::PoolCreate{{size:1568,version:1,flags:0,count:2,reserved:0,result:r,members:[empty;64]}}; p.members[0]=m; p.members[1]=abi::PoolMember{{buffer_index:5,reserved:0,offset:96,length:2048}};
 let q=abi::BufferReceipt{{pool:r,member:1,reserved:0,lease:0x8899aabbccddeeff}};
 let z=abi::BufferReturn{{size:40,version:1,flags:0,receipt:q}};
 let ps=abi::PoolSnapshot{{size:64,version:1,flags:0,pool:r,members:2,available:0,busy:0,pending:0,leased:2,streams:1,reserved:[0;2]}};
 emit(&constants);emit(&r);emit(&t);emit(&v);emit(&c);emit(&s);
 emit(&m);emit(&p);emit(&q);emit(&z);emit(&ps);
}}
'''.replace("#[path='", '#[path="').replace("'] mod abi;", '"] mod abi;')

def run(root):
    clang = os.environ.get('CC', 'clang')
    host_flags = shlex.split(os.environ.get('CFLAGS', ''))
    rustc = os.environ.get('RUSTC', 'rustc')
    for tool in (clang, rustc):
        if not shutil.which(tool): raise RuntimeError(f'missing compiler: {tool}')
    want = [v.hex() for v in expected()]
    with tempfile.TemporaryDirectory(prefix='loom-service-abi-') as tmp:
        tmp = Path(tmp)
        for kernel, label, inc in [(True, 'kernel', root/'kernel/include'), (False, 'native-c', root/'usr/lib/libt/include')]:
            source = tmp/(label+'.c'); source.write_text(c_source(kernel))
            exe = tmp/label
            subprocess.run([clang,*host_flags,'-std=c11','-Wall','-Wextra','-Werror','-I',str(inc),str(source),'-o',str(exe)],check=True)
            got = subprocess.check_output([str(exe)],text=True).splitlines()
            assert got == want, f'ABI vector mismatch: {label}'
            # Freestanding target compilation validates ARM64 layout as well.
            probe=tmp/(label+'-target.c'); probe.write_text('#include <'+('thylacine/loom_service_abi.h' if kernel else 'thyla/loom_service.h')+'>\n')
            subprocess.run([clang,'--target=aarch64-none-elf','-ffreestanding','-std=c11','-fsyntax-only','-I',str(inc),str(probe)],check=True)
        source=tmp/'mirror.rs'; source.write_text(rust_source(root/'usr/lib/loom-client/src/service_abi.rs'))
        exe=tmp/'rust'; subprocess.run([rustc,'--edition=2021','-Dwarnings',str(source),'-o',str(exe)],check=True)
        got=subprocess.check_output([str(exe)],text=True).splitlines()
        assert got==want,'ABI vector mismatch: Rust'
        # Check against the actual legacy envelope, not just the new header.
        # This reservation checkpoint MUST NOT activate either private mode.
        full=tmp/'full-kernel.c'
        full.write_text('''#include <thylacine/loom.h>
_Static_assert(sizeof(struct loom_sqe)==64, "SQE unchanged");
_Static_assert(sizeof(struct loom_cqe)==16, "CQE unchanged");
_Static_assert(sizeof(struct loom_params)==88, "setup unchanged");
_Static_assert(__builtin_offsetof(struct loom_params,_resv1)==56, "receipt geometry carrier");
_Static_assert((LOOM_SETUP_VALID & (LOOM_SETUP_PRIVATE_SERVICE | LOOM_SETUP_SERVICE_BUFFERS))==0, "private modes remain disabled");
_Static_assert((LOOM_CQE_SERVICE_BUFFER & (LOOM_CQE_MORE | LOOM_CQE_F_NOTIF))==0, "receipt flag distinct");
_Static_assert((LOOM_SQE_BUFFER_SELECT & (LOOM_SQE_LINK | LOOM_SQE_DRAIN | LOOM_SQE_CQE_SKIP | LOOM_SQE_MULTISHOT))==0, "selection flag distinct");
_Static_assert(LOOM_OP_COUNT==20, "private opcode remains reserved");
''')
        subprocess.run([clang,'--target=aarch64-none-elf','-march=armv8-a',
                        '-ffreestanding','-std=c11','-fsyntax-only',
                        '-I',str(root/'kernel/include'),'-I',str(root/'arch/arm64'),
                        str(full)],check=True)
        for kernel, label, incs in [(True, 'kernel-ring', ['kernel/include', 'arch/arm64']),
                                    (False, 'native-c-ring', ['usr/lib/libt/include'])]:
            source = tmp/(label+'.c'); source.write_text(ring_source(kernel))
            subprocess.run([clang,'--target=aarch64-none-elf','-march=armv8-a',
                            '-ffreestanding','-std=c11','-fsyntax-only',
                            *[a for i in incs for a in ('-I', str(root/i))], str(source)],check=True)
        # Edition 2024 for the cores' twins, as their crate builds them; the ring
        # mirror is edition-neutral. The binary prints every check it made.
        source, rust_records, rust_checks = rust_ring_source(root)
        (tmp/'rust-ring.rs').write_text(source)
        subprocess.run([rustc,'--edition=2024','-Dwarnings',str(tmp/'rust-ring.rs'),'-o',str(tmp/'rust-ring')],check=True)
        got=subprocess.check_output([str(tmp/'rust-ring')],text=True).splitlines()
        assert len(got)==rust_checks, f'Rust ring leg printed {len(got)} checks, expected {rust_checks}'
    print(f'PASS: 3 actual mirrors; {len(CONSTANTS)} constants, 10 records and every declared size/offset; ARM64 C layouts.')
    print(f'PASS: kernel and native C ring mirrors; {len(RING_CONSTANTS)} constants, '
          f'{len(RING_RECORDS)} records, {sum(len(f) for _, _, f in RING_RECORDS)} fields against one layout table; '
          f'the full-table errno ({ENOSPC}) of the kernel and the native C client cores.')
    print(f'PASS: Rust ring mirror (ring_abi.rs) and the cores\' twins ({", ".join(t for t, *_ in TWINS)} in C; '
          f'{", ".join(p for _, p, *_ in TWINS)} in Rust): {rust_records} Rust records, {len(got)} checks.')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--root',type=Path,default=ROOT)
    run(parser.parse_args().root.resolve())
