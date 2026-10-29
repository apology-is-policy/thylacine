from pathlib import Path
import subprocess,json,hashlib,datetime,os,resource,re
R=Path(__file__).resolve().parents[4];os.chdir(R);D=Path(__file__).resolve().parent
assert 'mac   HELD by you' in subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True)
run=D/('host-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
source=Path('kernel/loom.c').read_text();header=Path('kernel/include/thylacine/loom.h').read_text()
a=source.index('        u32 ring_pos = l->sq_head &');b=source.index('        spin_unlock(&l->lock);',a);block=source[a:b]
# Exact production claim/copy/release/routing block, with test hook enabled.
structs='\n'.join(re.search(r'struct '+n+r' \{.*?\n\};',header,re.S)[0] for n in ['loom_sqe','loom_ring_hdr'])
prefix='''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#define KERNEL_TESTS 1
#define LOOM_SQE_LINK 1
#define LOOM_SQE_DRAIN 2
'''+structs+'''
struct Loom{u32 sq_head,sq_entries,admitting;void *chain;};
static struct loom_ring_hdr header;
static struct loom_sqe entries[1];static u32 indices[1],visits;
static void producer(struct Loom *l){(void)l;++visits;assert(header.sq_head==1);indices[0]=0;entries[0].user_data=0x222;entries[0].flags=LOOM_SQE_LINK;}
static void (*sq_reuse_test_hook)(struct Loom *)=producer;
static void exercise(bool bad){
 struct Loom owner={.sq_entries=1},*l=&owner;struct loom_ring_hdr *h=&header;struct loom_sqe *sqes=entries,ksqe={0};u32 *sq_array=indices;bool have,to_chain;
 header=(struct loom_ring_hdr){0};entries[0]=(struct loom_sqe){.user_data=0x111};indices[0]=bad?7:0;visits=0;
'''
suffix='''
 assert(visits==1 && h->sq_head==1);assert(have==!bad);assert(h->dropped==(u32)bad);assert(l->admitting==(u32)!bad);
 if(have){assert(ksqe.user_data==0x111);assert(ksqe.flags==0 && !to_chain);}assert(entries[0].user_data==0x222);
}
int main(void){exercise(false);exercise(true);puts("SQ copied before reuse; invalid original index remains rejected PASS");}
'''
manifest={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in ['kernel/loom.c','kernel/include/thylacine/loom.h',str(Path(__file__))]};(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']));(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']));(run/'block.c').write_text(block)
cc='/opt/homebrew/opt/llvm@22/bin/clang';sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,fail=False,diag=None):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 with (run/(name+'.log')).open('wb') as f:r=subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,timeout=30,preexec_fn=limits)
 records.append(dict(name=name,rc=r.returncode,expected_failure=fail,diagnostic=diag));assert (r.returncode!=0)==fail
 if diag:assert diag in (run/(name+'.log')).read_text()
try:
 for label,flags in [('sanitized',['-fsanitize=address,undefined']),('optimized',['-O2'])]:
  c=run/(label+'.c');c.write_text(prefix+block+suffix);binary=run/(label+'-test');cmd([cc,'-isysroot',sdk,'-std=c11','-Wall','-Wextra','-Werror',*flags,str(c),'-o',str(binary)],label+'-build');cmd([str(binary)],label+'-run',diag='PASS')
 # Reproduce old production ordering while retaining the new immediate-reuse
 # observation hook: publish head + simulate producer BEFORE reading old index.
 begin=block.index('        l->sq_head++;');end=block.index('#endif',begin)+len('#endif\n');release=block[begin:end]
 mutant=block[:begin]+block[end:];pos=mutant.index('        u32 sqe_idx');mutant=mutant[:pos]+release+mutant[pos:]
 c=run/'early-release.c';c.write_text(prefix+mutant+suffix);binary=run/'mutant-test';cmd([cc,'-isysroot',sdk,'-std=c11','-O2',str(c),'-o',str(binary)],'early-release-build');cmd([str(binary)],'early-release-run',True,'ksqe.user_data==0x111')
 assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(state='passed',records=records,owner='astra',limit='Exact consume block with host stub owner and simulated producer; native regression separately exercises full drain'),indent=2)+'\n');print(run)
finally:(run/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
