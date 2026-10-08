#!/usr/bin/env python3
"""Extracted private constructor/close/destructor plus real slot module.

Host ASan/UBSan with checked allocator, geometry, Loom layout, image and lock
doubles. Validates rollback/free/call ordering, not guest physical accounting,
actual image races or protocol retirement. Requires Astra's Mac lease, 8 GiB.
"""
import datetime
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('final-reclaim-control.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)

PRE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_slots.h>
#include <thylacine/errno.h>
#define PAGE_SIZE 4096
#define SLUB_MAX_OBJECT_SIZE 2048
#define KP_ZERO 1
#define PROC_MAGIC 123
#define LOOM_MAX_REG_HANDLES 2
#define CHECK(c,msg) do { if(!(c)) { puts(msg); exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int locks;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"recursive lock");l->held=1;locks++; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"unlock without lock");l->held=0;locks--; }
static void extinction(const char *s) { puts(s);exit(12); }
struct AddrSpace { spin_lock_t lock;u32 pages,budget,private_rings,refs; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct Burrow { int unused; };
struct Loom {
    struct AddrSpace *service_as;
    struct loom_slot_table *service_slots;
    u64 service_creator;
    u32 service_metadata_pages;
    spin_lock_t lock;
    bool service_closing,sqpoll;
    u32 n_reg_buf,inflight_ops;
    struct { void *spoor; } reg[LOOM_MAX_REG_HANDLES];
    void *ring;u32 magic;
    char buddy_padding[3000];
};
struct loom_layout { u32 ring_size; };
static bool service_retire_ready=true,layout_fault,slots_fault,alloc_fault,inner_fault;
static u64 g_loom_destroyed,service_retired_count;
static void *allocations[8];
static int live,allocated,freed,recorded;
static u32 ring_charge;
static bool loom_private_layout_fault_take(void) { bool b=layout_fault;layout_fault=false;return b; }
static bool loom_private_slots_fault_take(void) { bool b=slots_fault;slots_fault=false;return b; }
static bool loom_measure(u32 sq,u32 cq,bool receipts,struct loom_layout *g) {
    CHECK(receipts,"fixture receipts");g->ring_size=PAGE_SIZE;return sq==2 && cq==2;
}
static bool addrspace_private_begin(struct AddrSpace *a) {
    CHECK(!locks,"image pin outside leaf lock");a->private_rings++;a->refs++;return true;
}
static void addrspace_private_end(struct AddrSpace *a) {
    CHECK(!locks && a->private_rings && a->refs>1,"image release ordering");
    a->private_rings--;a->refs--;
}
static u64 burrow_backing_pages(u64 bytes) {
    u64 pages=1;while(pages*PAGE_SIZE<bytes) pages*=2;return pages;
}
static bool proc_resource_exempt(struct Proc *p) { (void)p;return false; }
static bool addrspace_charge_pages(struct AddrSpace *a,u32 n,bool exempt) {
    CHECK(a->lock.held && !exempt,"charge under image lock");
    if(n>a->budget-a->pages) return false;a->pages+=n;return true;
}
static void addrspace_uncharge_pages(struct AddrSpace *a,u32 n) {
    CHECK(a->lock.held && n<=a->pages,"refund under image lock");a->pages-=n;
}
static void *kmalloc(size_t n,unsigned flags) {
    CHECK(!locks && flags==KP_ZERO && allocated<8,"allocation contract");
    if(alloc_fault) { alloc_fault=false;return NULL; }
    void *p=calloc(1,n);CHECK(p,"host allocation");allocations[allocated++]=p;live++;return p;
}
static void kfree(void *p) {
    if(!p)return;CHECK(!locks,"free outside lock");
    for(int i=0;i<allocated;i++) if(allocations[i]==p) {
        allocations[i]=NULL;live--;freed++;free(p);return;
    }
    extinction("free without ownership");
}
static struct Loom *loom_create_layout(u32 sq,u32 cq,bool exempt,bool receipts) {
    (void)sq;(void)cq;(void)exempt;(void)receipts;
    if(inner_fault)return NULL;
    struct Loom *l=kmalloc(sizeof(*l),KP_ZERO);if(l)l->ring=(void*)1;return l;
}
static void burrow_charge_record(void *ring,struct Proc *p,u32 pages) {
    CHECK(!locks && ring==(void*)1 && p->as->private_rings==1,"ring charge publication");
    ring_charge=pages;recorded++;
}
static bool burrow_unref_settled_in(void *ring,struct AddrSpace *a,u32 *refund) {
    CHECK(!locks && ring==(void*)1 && a->private_rings==1,"settled drop before image put");
    *refund=ring_charge;ring_charge=0;return true;
}
static bool loom_is_private(const struct Loom *l) { return l && l->service_as; }
'''

MAIN = r'''
int main(int argc,char **argv) {
    int mode=argc>1?atoi(argv[1]):0;
    struct AddrSpace a={.pages=10,.budget=100,.refs=1};
    struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=987};
    // Separate processes isolate each failure edge and its exact allocation set.
    if(mode==1)a.budget=10;
    if(mode==2)slots_fault=true;
    if(mode==3)alloc_fault=true;
    if(mode==4)layout_fault=true;
    if(mode==5)inner_fault=true;
    if(mode==6)slots_fault=true;
    struct Loom *l=loom_create_private(&p,mode==6?3:2,2,true);
    CHECK(!slots_fault && !layout_fault,"one-shot faults consumed");
    if(mode>=1 && mode<=6) {
        CHECK(!l,"fault refuses publication");
        CHECK(live==0,"rollback frees slot page");
        CHECK(a.pages==10,"refusal restores accounting");
        CHECK(a.refs==1 && !a.private_rings,"refusal restores lifetime");
        CHECK(!recorded,"refusal publishes no ring charge");
        puts("PASS refusal");return 0;
    }
    CHECK(l && l->service_slots && live==2,"owner retains both allocations");
    CHECK(l->service_as==&a && l->service_creator==987,"exact image and identity");
    CHECK(l->service_metadata_pages==burrow_backing_pages(sizeof(*l))+1,
          "slot page charged in metadata");
    CHECK(a.pages==10+l->service_metadata_pages+2,"complete owner charge");
    struct loom_service_ref ref;
    spin_lock(&l->lock);
    CHECK(!loom_slot_reserve(l->service_slots,LOOM_SERVICE_SCOPE,&ref),"reserve");
    CHECK(!loom_slot_publish(l->service_slots,ref),"publish");
    CHECK(!loom_slot_pin(l->service_slots,ref,LOOM_SERVICE_SCOPE),"pin");
    spin_unlock(&l->lock);
    loom_exec_latch(l);
    CHECK(l->service_slots->closing,"close freezes ledger");
    if(mode==7) {
        loom_private_destroy(l);
        CHECK(false,"busy retirement returned");
    }
    spin_lock(&l->lock);
    CHECK(!loom_slot_unpin(l->service_slots,ref),"unpin after close");
    CHECK(!loom_slot_retire(l->service_slots,ref),"retire after close");
    CHECK(!loom_slot_reap(l->service_slots,ref),"reap after close");
    spin_unlock(&l->lock);
    loom_private_destroy(l);
    CHECK(live==0 && freed==2,"retirement frees owner and slot page");
    CHECK(a.pages==10 && a.refs==1 && !a.private_rings,"retirement restores image");
    CHECK(service_retired_count==1 && g_loom_destroyed==1,"retirement published once");
    puts("PASS owner");return 0;
}
'''


def main():
    os.chdir(ROOT)
    lease = subprocess.check_output(['work/oct5-as-r9/astra-yip.sh', 'resources'], text=True)
    assert re.search(r'^mac +HELD by you', lease, re.M)
    st = os.statvfs(ROOT)
    assert st.f_bavail * st.f_frsize >= 8 * 1024**3
    out = Path('work/oct5-as-r9/astra-takeover') / ('slot-owner-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out.mkdir()
    paths = ['kernel/loom.c', 'kernel/loom_service_slots.c', 'kernel/include/thylacine/loom_service_slots.h', 'kernel/include/thylacine/loom_service_abi.h']
    sources = {p: Path(p).read_text() for p in paths}
    funcs = {n: extract.fun(sources['kernel/loom.c'], n) for n in ('loom_create_private', 'loom_exec_latch', 'loom_private_destroy')}
    def program(fs):
        return PRE + '\n' + '\n'.join(fs.values()) + MAIN
    control = program(funcs)
    cases = [('control', None, None, None, list(range(8)), None),
        ('omit-slot-charge', 'loom_create_private', 'burrow_backing_pages(sizeof(struct Loom)) + 1u', 'burrow_backing_pages(sizeof(struct Loom))', [0], 'slot page charged in metadata'),
        ('omit-unwind-free', 'loom_create_private', '        kfree(slots);', '', [4], 'rollback frees slot page'),
        ('omit-unwind-refund', 'loom_create_private', '        addrspace_uncharge_pages(as, metadata + backing);', '', [2], 'refusal restores accounting'),
        ('omit-unwind-pin', 'loom_create_private', '        addrspace_private_end(as);', '', [2], 'refusal restores lifetime'),
        ('omit-retire-free', 'loom_private_destroy', '    kfree(l->service_slots);', '', [0], 'retirement frees owner and slot page'),
        ('omit-close', 'loom_exec_latch', '    loom_slots_close(l->service_slots);', '', [0], 'close freezes ledger'),
        ('omit-busy-guard', 'loom_private_destroy', '            extinction("private Loom retained a service slot");', '            (void)0;', [7], 'busy retirement returned')]
    sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
    results = []
    for name, function, old, new, modes, want in cases:
        fs = dict(funcs)
        if old:
            assert fs[function].count(old) == 1, name
            fs[function] = fs[function].replace(old, new)
        src = out / (name + '.c'); src.write_text(program(fs))
        (out / (name + '.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True), src.read_text().splitlines(True), fromfile='control', tofile=name)))
        exe = out / name
        cmd = [os.environ.get('CC', '/opt/homebrew/opt/llvm@22/bin/clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-isysroot', sdk, '-I', str(ROOT / 'kernel/include'), str(src), 'kernel/loom_service_slots.c', '-o', str(exe)]
        (out / (name + '-command.json')).write_text(json.dumps(cmd) + '\n')
        with (out / (name + '-build.log')).open('wb') as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
        for mode in modes:
            r = subprocess.run([str(exe.resolve()), str(mode)], capture_output=True, text=True, timeout=15, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0', 'UBSAN_OPTIONS':'halt_on_error=1'})
            text = r.stdout + r.stderr
            (out / f'{name}-{mode}.log').write_text(text)
            expected = want or ('private Loom retained a service slot' if mode == 7 else 'PASS refusal' if mode else 'PASS owner')
            rc = 11 if want else 12 if mode == 7 else 0
            assert r.returncode == rc and text == expected + '\n', (name, mode, r.returncode, text)
            results.append(dict(case=name, mode=mode, rc=r.returncode, expected=expected, generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()))
            print('PASS', name, mode, expected, flush=True)
    assert all(Path(p).read_text() == s for p, s in sources.items())
    (out / 'result.json').write_text(json.dumps(dict(source_head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(), source_hashes={p:hashlib.sha256(s.encode()).hexdigest() for p,s in sources.items()}, results=results, scope=__doc__), indent=2) + '\n')
    print(out)


if __name__ == '__main__':
    main()
