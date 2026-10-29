#!/usr/bin/env python3
"""Controlled old-source schedule. Run only under an acquired Mac lease.
Poison the descriptor in the allocator double; do not access freed host memory.
This witnesses the kernel's stale restore attempt, not an end-to-end exploit.
"""
from pathlib import Path
import os, shlex, subprocess
W = Path(__file__).resolve().parent
prelude = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define VMO_MAGIC 0x42555252
struct AddrSpace { u64 id; };
struct Burrow { u32 magic; int lock, handle_count, mapping_count;
    u64 charge_as_id; u32 charge_pages; };
static void (*after_unlock)(void);
static int freed;
static void extinction(const char *why) {
    fprintf(stderr, "detected: %s\n", why);
    exit(strstr(why, "burrow_charge_restore on corrupted") ? 42 : 99);
}
static void spin_lock(int *l) { if (*l) abort(); *l=1; }
static void spin_unlock(int *l) {
    if (!*l) abort(); *l=0;
    void (*hook)(void)=after_unlock; after_unlock=NULL;
    if (hook) hook();
}
static void burrow_free_internal(struct Burrow *v) {
    if (v->lock || v->handle_count || v->mapping_count || freed) abort();
    v->magic=0; ++freed;
}
'''
postlude = r'''
static struct Burrow b;
static struct AddrSpace as={7};
static void final_holder(void) {
    u32 paid=burrow_charge_claim_in(&b, &as);
    if (paid) abort(); /* first claimant temporarily hid the record */
    if (!burrow_unref_freed(&b)) abort();
}
int main(int argc, char **argv) {
    bool mapping = argc>1 && strcmp(argv[1],"mapping")==0;
    b=(struct Burrow){.magic=VMO_MAGIC,.handle_count=mapping?1:2,
        .mapping_count=mapping?1:0,.charge_as_id=7,.charge_pages=5};
    u32 paid=burrow_charge_claim_in(&b,&as);
    if (paid!=5) abort();
    after_unlock=final_holder;
    bool dead=mapping ? burrow_release_mapping_deferred(&b)!=NULL : burrow_unref_freed(&b);
    if (dead || freed!=1) abort();
    burrow_charge_restore_in(&b,&as,paid);
    return 1;
}
'''
c=W/'old-reproduction.c'
c.write_text(prelude+(W/'old-source.c').read_text()+postlude)
exe=W/'old-reproduction'
cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
with (W/'old-build.log').open('w') as f: subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
for mode in ['handle','mapping']:
    p=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=10)
    (W/f'old-{mode}.log').write_text(p.stdout+p.stderr)
    assert p.returncode==42,(mode,p.returncode,p.stderr)
    print(f'REPRODUCED {mode}: nonfinal drop, concurrent final release, stale restore',flush=True)
