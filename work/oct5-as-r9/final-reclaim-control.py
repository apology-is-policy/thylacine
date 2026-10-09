#!/usr/bin/env python3
"""Extracted retirement -> AddrSpace -> page-table destruction, host boundary.

Actual C functions and AddrSpace declaration; real host heap frees, with doubles
for allocator entry points, locks, VMA drain, ring refund and Loom layout.
Sequential schedules only. Does not certify guest buddy/magazine reclamation,
ARM scheduling, or an engine that is not implemented. Requires Mac lease and
8 GiB disk admission. Leaves all generated sources, diffs and logs per run.
"""
from pathlib import Path
import datetime, difflib, hashlib, json, os, re, subprocess, sys
ROOT = Path(__file__).resolve().parents[2]

def fun(s, name):
    m = re.search(r'^((?:static |inline )*[\w *]+\b' + name + r'\([^;]*?\)\s*\{)', s, re.M)
    assert m, name
    start = m.start(); i = s.index('{', start); depth = 1; end = i + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}'); end += 1
    return s[start:end]

PRE = r'''
#define _POSIX_C_SOURCE 200112L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/loom_service_nodes.h>
#include <thylacine/loom_service_requests.h>
#define THYLACINE_DEVSRV_H
struct SrvServiceTarget { void *view,*service;u64 generation;u8 name_len;char name[32]; };
#include <thylacine/loom_service_targets.h>

typedef struct { int held; } spin_lock_t;
#define ENTRIES_PER_TABLE 512
#define PTE_VALID (1ull << 0)
#define PTE_TYPE_TABLE (1ull << 1)
#define KP_ZERO 1
#define LOOM_MAX_REG_HANDLES 2
#define LOOM_MAX_REG_BUFFERS 2
#define REQUIRE(c, msg) do { if (!(c)) { puts(msg); exit(11); } } while (0)
static int held_locks;
static void spin_lock_init(spin_lock_t *p) { p->held = 0; }
static void spin_lock(spin_lock_t *p) { REQUIRE(!p->held,"recursive lock"); p->held=1; held_locks++; }
static void spin_unlock(spin_lock_t *p) { REQUIRE(p->held,"unlock without lock"); p->held=0; held_locks--; }
static void extinction(const char *s) { puts(s); exit(12); }
struct page { void *mem; bool live; };
static struct page pages[8];
static int nr_pages, page_frees, as_frees, loom_frees, slot_frees, pool_frees, target_frees, node_frees, request_frees, drains, refunds;
static uintptr_t as_addr, loom_addr, slot_addr, pool_addr, target_addr, node_addr, request_addr;
static u64 g_addrspace_next_id, g_loom_destroyed, service_retired_count;
static struct page *alloc_pages(unsigned order, unsigned flags) {
    REQUIRE(order==0 && flags==KP_ZERO && nr_pages<8,"page allocation contract");
    struct page *p=&pages[nr_pages++];
    REQUIRE(!posix_memalign(&p->mem,4096,4096),"host page allocation");
    memset(p->mem,0,4096);p->live=true;return p;
}
static paddr_t page_to_pa(struct page *p) { return (uintptr_t)p->mem; }
static struct page *pa_to_page(paddr_t pa) {
    for(int i=0;i<nr_pages;i++) if((uintptr_t)pages[i].mem==pa) return &pages[i];
    extinction("unknown table page");return NULL;
}
static void *pa_to_kva(paddr_t pa) { return pa_to_page(pa)->mem; }
static void free_pages(struct page *p, unsigned order) {
    REQUIRE(!held_locks && !order && p->live,"table free ownership/lock");
    unsigned char *bytes=p->mem;
    for(int i=0;i<4096;i++) REQUIRE(bytes[i]==0,"table cleared before free");
    p->live=false;page_frees++;free(p->mem);
}
static void *kzalloc(size_t n, unsigned flags) { (void)flags; return calloc(1,n); }
static void kfree(void *p) {
    REQUIRE(!held_locks,"descriptor free outside lock");
    if((uintptr_t)p==as_addr) { REQUIRE(!as_frees,"descriptor freed once"); as_frees++; }
    else if((uintptr_t)p==loom_addr) { REQUIRE(!loom_frees,"Loom freed once"); loom_frees++; }
    else if((uintptr_t)p==slot_addr) { REQUIRE(!slot_frees,"slot page freed once"); slot_frees++; }
    else if((uintptr_t)p==pool_addr) { REQUIRE(!pool_frees,"pool table freed once"); pool_frees++; }
    else if((uintptr_t)p==target_addr) { REQUIRE(!target_frees,"target table freed once"); target_frees++; }
    else if((uintptr_t)p==request_addr) { REQUIRE(!request_frees,"request page freed once");request_frees++; }
    else if((uintptr_t)p==node_addr) { REQUIRE(!node_frees,"node page freed once"); node_frees++; }
    else extinction("unexpected object free");
    free(p);
}
'''
DOUBLES = r'''
static void devsrv_service_target_clear(struct SrvServiceTarget *t) { REQUIRE(!held_locks && !t->view,"empty target clear"); }
struct Burrow;
struct loom_reg_buffer { struct Burrow *burrow; void *kva; u32 len, _pad; };
struct Loom {
    struct AddrSpace *service_as;
    struct loom_slot_table *service_slots;
    struct loom_service_pool_table *service_pools;
    struct loom_service_target_table *service_targets;
    struct loom_service_node_table *service_nodes;
    struct loom_request_table service_requests;
    u32 cq_entries;
    void *service_fixed;u32 service_fixed_count;
    u32 service_metadata_pages;
    bool sqpoll; u32 n_reg_buf, inflight_ops;
    struct loom_reg_buffer reg_buf[LOOM_MAX_REG_BUFFERS];
    struct { void *spoor; } reg[LOOM_MAX_REG_HANDLES];
    void *ring; u32 magic;
};
static void vma_drain_in(struct AddrSpace *as) {
    REQUIRE(!held_locks && as->owners==0,"last owner drains outside lock");
    drains++;as->vmas=NULL;
}
static bool burrow_unref_settled_in(void *ring, struct AddrSpace *as, u32 *refund) {
    REQUIRE(!held_locks && ring==(void *)1 && as->private_rings==1,"settled ring release");
    REQUIRE(!refunds,"ring released once");refunds++;*refund=3;return true;
}
static void addrspace_uncharge_pages(struct AddrSpace *as, u32 pages) {
    REQUIRE(as->lock.held && as->page_count==pages,"exact retirement refund");
    as->page_count-=pages;
}
'''
MAIN = r'''
int main(int argc,char **argv) {
    int order=argc>1?atoi(argv[1]):0;
    struct AddrSpace *as=addrspace_alloc(100);REQUIRE(as,"AddrSpace allocated");
    as_addr=(uintptr_t)as;
    // A root plus L1/L2/L3, with invalid and block entries that are NOT tables.
    u64 *l0=pa_to_kva(as->pgtable_root);
    for(int i=0;i<3;i++) (void)alloc_pages(0,KP_ZERO);
    l0[0]=page_to_pa(&pages[1])|3;l0[1]=0x40000000|PTE_VALID;
    u64 *l1=pages[1].mem,*l2=pages[2].mem,*l3=pages[3].mem;
    l1[0]=page_to_pa(&pages[2])|3;l1[1]=0x80000000|PTE_VALID;
    l2[0]=page_to_pa(&pages[3])|3;l2[1]=0xc0000000|PTE_VALID;
    // A leaf PA must be cleared but never interpreted as a table to free.
    l3[0]=0xdead000|3;
    REQUIRE(addrspace_private_begin(as),"private pin admitted");
    struct Loom *l=calloc(1,sizeof(*l));REQUIRE(l,"Loom allocated");
    loom_addr=(uintptr_t)l;l->service_as=as;l->service_metadata_pages=7;
    l->service_slots=calloc(1,4096);REQUIRE(l->service_slots,"slot page allocated");
    slot_addr=(uintptr_t)l->service_slots;
    l->service_pools=calloc(1,sizeof(*l->service_pools));REQUIRE(l->service_pools,"pool table allocated");
    pool_addr=(uintptr_t)l->service_pools;
    l->service_targets=calloc(1,sizeof(*l->service_targets));REQUIRE(l->service_targets,"target table allocated");
    target_addr=(uintptr_t)l->service_targets;
    l->service_nodes=calloc(1,4096);REQUIRE(l->service_nodes,"node page allocated");
    node_addr=(uintptr_t)l->service_nodes;
    l->service_requests.records=calloc(1,4096);REQUIRE(l->service_requests.records,"request page allocated");
    l->service_requests.capacity=2;l->cq_entries=2;request_addr=(uintptr_t)l->service_requests.records;
    l->ring=(void *)1;as->page_count=10;
    if(order==0) {
        addrspace_unref(as);
        REQUIRE(drains==1 && !as_frees && !page_frees,"private pin retains ownerless image");
        loom_private_destroy(l);
    } else {
        loom_private_destroy(l);
        REQUIRE(!drains && !as_frees && !page_frees,"creator retains retired image");
        REQUIRE(as->ref==1 && !as->private_rings && !as->page_count,"retirement releases only its pin");
        addrspace_unref(as);
    }
    // No descriptor or table pointer is dereferenced beyond final drop.
    REQUIRE(as_frees==1,"final descriptor reclaimed exactly once");
    REQUIRE(page_frees==4,"all four page-table levels reclaimed exactly once");
    REQUIRE(request_frees==1,"request page reclaimed exactly once");
    REQUIRE(node_frees==1,"node page reclaimed exactly once");
    REQUIRE(target_frees==1,"target table reclaimed exactly once");
    REQUIRE(pool_frees==1,"pool table reclaimed exactly once");
    REQUIRE(slot_frees==1,"slot page reclaimed exactly once");
    REQUIRE(loom_frees==1 && refunds==1 && drains==1,"local retirements occur once");
    REQUIRE(service_retired_count==1 && g_loom_destroyed==1,"retirement completion published");
    for(int i=0;i<nr_pages;i++) REQUIRE(!pages[i].live,"no retained table allocation");
    puts("PASS: final descriptor and four table levels freed through extracted code");
    return 0;
}
'''

def main():
    os.chdir(ROOT)
    yip=ROOT/'work/oct5-as-r9/astra-yip.sh'
    lease=subprocess.check_output([str(yip),'resources'],text=True)
    assert re.search(r'^mac +HELD by you',lease,re.M), 'Mac lease required'
    st=os.statvfs(ROOT);free=st.f_bavail*st.f_frsize
    assert free>=8*1024**3, f'8 GiB floor: {free/1024**3:.3f} available'
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out=ROOT/'work/oct5-as-r9/astra-takeover'/('final-reclaim-'+stamp);out.mkdir()
    sources={n:(ROOT/n).read_text() for n in ['kernel/addrspace.c','kernel/include/thylacine/addrspace.h','arch/arm64/mmu.c','kernel/loom.c','kernel/loom_service_pool_table.c','kernel/include/thylacine/loom_service_pool_table.h','kernel/include/thylacine/loom_service_pool.h','kernel/include/thylacine/loom_service_slots.h','kernel/include/thylacine/loom_service_abi.h','kernel/loom_service_nodes.c','kernel/include/thylacine/loom_service_nodes.h','kernel/loom_service_slots.c','kernel/loom_service_requests.c','kernel/include/thylacine/loom_service_requests.h','work/oct5-as-r9/final-reclaim-control.py']}
    header=sources['kernel/include/thylacine/addrspace.h'];a=header.index('struct AddrSpace {');b=header.index('\n};',a)+3
    funcs={}
    names=['clear_page_then_free','l3_walk_and_free','l2_walk_and_free','l1_walk_and_free','proc_pgtable_create','proc_pgtable_destroy']
    for n in names:funcs[n]=fun(sources['arch/arm64/mmu.c'],n)
    for n in ['addrspace_alloc','addrspace_lifetime_get','addrspace_private_begin','addrspace_lifetime_put','addrspace_unref','addrspace_private_end']:
        funcs[n]=fun(sources['kernel/addrspace.c'],n)
    funcs['loom_service_pools_empty']=fun(sources['kernel/loom_service_pool_table.c'],'loom_service_pools_empty')
    funcs['loom_private_drop_buffer']=fun(sources['kernel/loom.c'],'loom_private_drop_buffer')
    funcs['loom_private_targets_destroy']=fun(sources['kernel/loom.c'],'loom_private_targets_destroy')
    funcs['loom_private_nodes_destroy']=fun(sources['kernel/loom.c'],'loom_private_nodes_destroy')
    funcs['loom_private_requests_destroy']=fun(sources['kernel/loom.c'],'loom_private_requests_destroy')
    funcs['loom_private_destroy']=fun(sources['kernel/loom.c'],'loom_private_destroy')
    def program(fs):return PRE+'\n'+header[a:b]+'\n'+DOUBLES+'\n'+'\n'.join(fs.values())+'\n'+MAIN
    control=program(funcs)
    variants=[('control',None,None,None,None),
      ('omit-private-put','addrspace_private_end','    addrspace_lifetime_put(as);','', 'final descriptor reclaimed exactly once'),
      ('omit-descriptor-free','addrspace_lifetime_put','    kfree(as);','', 'final descriptor reclaimed exactly once'),
      ('omit-table-destroy','addrspace_lifetime_put','    proc_pgtable_destroy(as->pgtable_root);','', 'all four page-table levels reclaimed exactly once'),
      ('omit-root-free','proc_pgtable_destroy','    clear_page_then_free(root);','', 'all four page-table levels reclaimed exactly once'),
      ('omit-request-free','loom_private_destroy','    kfree(l->service_requests.records);','', 'request page reclaimed exactly once'),
      ('omit-node-free','loom_private_destroy','    kfree(l->service_nodes);','', 'node page reclaimed exactly once'),
      ('omit-target-free','loom_private_destroy','    kfree(l->service_targets);','', 'target table reclaimed exactly once'),
      ('omit-pool-free','loom_private_destroy','    kfree(l->service_pools);','', 'pool table reclaimed exactly once'),
      ('omit-slot-free','loom_private_destroy','    kfree(l->service_slots);','', 'slot page reclaimed exactly once')]
    cc=os.environ.get('CC','/opt/homebrew/opt/llvm@22/bin/clang')
    sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
    results=[]
    for tag,name,old,new,want in variants:
        fs=dict(funcs)
        if name:
            assert fs[name].count(old)==1,(name,old)
            fs[name]=fs[name].replace(old,new)
        src=out/(tag+'.c');src.write_text(program(fs))
        (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True),src.read_text().splitlines(True),fromfile='control',tofile=tag)))
        exe=out/tag
        cmd=[cc,'-I',str(ROOT/'kernel/include'),'-std=c11','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-isysroot',sdk,str(src),'kernel/loom_service_nodes.c','kernel/loom_service_slots.c','kernel/loom_service_requests.c','-o',str(exe)]
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
        (out/(tag+'-build.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,r.stderr
        for order in ([0,1] if tag=='control' else [0]):
            # Mutants deliberately strand allocations until process exit; leak
            # classification is the exact assertion, not process-exit LSan.
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
            r=subprocess.run([str(exe),str(order)],capture_output=True,text=True,env=env,timeout=15)
            text=r.stdout+r.stderr;(out/f'{tag}-{order}.log').write_text(text)
            expected=0 if tag=='control' else 11
            good=r.returncode==expected and ((want in text) if want else text.startswith('PASS:'))
            assert 'ERROR: AddressSanitizer' not in text and 'runtime error:' not in text,text
            results.append({'variant':tag,'owner_drop_order':order,'rc':r.returncode,'expected':expected,'matched':good,'expected_assertion':want,'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'command':cmd})
            assert good,(tag,r.returncode,text)
    result={'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'source_hashes':{n:hashlib.sha256(s.encode()).hexdigest() for n,s in sources.items()},'free_bytes_before':free,'results':results,'scope':__doc__}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(out)
    print('PASS: both drop orders and nine distinct omission mutants; ASan/UBSan, host allocator boundary only')
if __name__=='__main__':main()
