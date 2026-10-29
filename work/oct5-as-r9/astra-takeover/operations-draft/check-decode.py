from pathlib import Path
import subprocess,re,json,datetime,hashlib
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
out=D/('decode-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
inc=out/'include/thylacine';inc.mkdir(parents=True)
real=(R/'kernel/include/thylacine/loom.h').read_text()
sqe=re.search(r'struct loom_sqe \{.*?\n\};',real,re.S).group()
opcodes=re.search(r'enum \{\s*LOOM_OP_NOP.*?\n\};',real,re.S).group()
macros='\n'.join(x for x in real.splitlines() if re.match(r'#define LOOM_(OP_\w+|SQE_(?:MULTISHOT|LINK|DRAIN|CQE_SKIP)|MAX_REG_BUFFERS)\s',x))
(inc/'loom.h').write_text('#include <thylacine/loom_service_abi.h>\n'+sqe+'\n_Static_assert(sizeof(struct loom_sqe)==64,"actual extracted SQE");\n'+opcodes+'\n'+macros+'\n')
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int count;
static struct loom_sqe base(u8 op) {
 struct loom_sqe s={.opcode=op,.handle_idx=1,.user_data=UINT64_MAX};
 s._resv1[1]=UINT64_MAX;s._resv1[3]=UINT64_MAX;
 if(op==LOOM_OP_SERVICE_CONNECT || op==LOOM_OP_WALK){s.offset=2;s._resv1[2]=UINT64_MAX;}
 return s;
}
static void check(struct loom_sqe s,bool pools,int want,const char *why) {
 struct loom_sqe before=s;int got=loom_private_sqe_shape(&s,pools);++count;
 if(got!=want || memcmp(&before,&s,sizeof(s))) {fprintf(stderr,"%s: got%d want%d\n",why,got,want);assert(0);}
}
int main(void) {
 u8 ops[]={LOOM_OP_SERVICE_CONNECT,LOOM_OP_WALK,LOOM_OP_LOPEN,LOOM_OP_READ,LOOM_OP_WRITE,LOOM_OP_CLUNK};
 assert(loom_private_sqe_shape(NULL,true)==-T_E_INVAL);
 for(unsigned i=0;i<sizeof(ops);++i){
  struct loom_sqe s=base(ops[i]);check(s,true,0,"valid operation");
  s._resv0=1;check(s,true,-T_E_INVAL,"reserved common");s=base(ops[i]);
  s.handle_idx=64;check(s,true,-T_E_INVAL,"source bound");s=base(ops[i]);
  s._resv1[1]=0;check(s,true,-T_E_INVAL,"source generation");
  for(unsigned flags=1;flags<256;++flags){
   s=base(ops[i]);s.flags=flags;
   if(ops[i]==LOOM_OP_READ && (flags==16 || flags==24))continue;
   check(s,true,-T_E_INVAL,"unknown flag or unqualified combination");
  }
 }
 for(unsigned op=0;op<256;++op){
  bool supported=false;for(unsigned i=0;i<sizeof(ops);++i)if(op==ops[i])supported=true;
  if(!supported)check(base(op),true,-T_E_OPNOTSUPP,"unsupported opcode");
 }
 for(unsigned i=0;i<2;++i){
  struct loom_sqe s=base(ops[i]);s.offset=64;check(s,true,-T_E_INVAL,"destination bound");
  s=base(ops[i]);s.offset=1ull<<32;check(s,true,-T_E_INVAL,"destination truncation");
  s=base(ops[i]);s.offset=1;check(s,true,-T_E_INVAL,"same shared slot");
  s=base(ops[i]);s._resv1[2]=0;check(s,true,-T_E_INVAL,"destination generation");
 }
 struct loom_sqe s=base(LOOM_OP_SERVICE_CONNECT);s.len=1;check(s,true,-T_E_INVAL,"connect buffer");
 s=base(LOOM_OP_WALK);s.len=255;s.buf_idx_or_off=63;s._resv1[0]=UINT64_MAX-255;check(s,true,0,"component maximum");
 s.len=256;s._resv1[0]=0;check(s,true,-T_E_INVAL,"oversized component");
 s=base(LOOM_OP_WALK);s.buf_idx_or_off=1;check(s,true,-T_E_INVAL,"empty clone buffer residue");
 for(unsigned i=0;i<3;++i){s=base(LOOM_OP_LOPEN);s.offset=i;check(s,true,0,"open mode");}
 s.offset=3;check(s,true,-T_E_INVAL,"unallocated open mode");
 s=base(LOOM_OP_CLUNK);s.offset=1;check(s,true,-T_E_INVAL,"clunk residue");
 for(unsigned i=0;i<2;++i){
  s=base(i?LOOM_OP_WRITE:LOOM_OP_READ);s.len=1;s._resv1[0]=UINT64_MAX;check(s,true,-T_E_INVAL,"slice addition overflow");
  s._resv1[0]=0;s.buf_idx_or_off=64;check(s,true,-T_E_INVAL,"buffer bound");
  s.buf_idx_or_off=0;s._resv1[2]=1;check(s,true,-T_E_INVAL,"fixed incarnation residue");
 }
 s=base(LOOM_OP_READ);s.flags=LOOM_SQE_BUFFER_SELECT;s.len=9;s.buf_idx_or_off=63;s._resv1[2]=UINT64_MAX;
 check(s,true,0,"pooled single shot");check(s,false,-T_E_OPNOTSUPP,"pool feature absent");
 s.flags|=LOOM_SQE_MULTISHOT;check(s,true,0,"pooled multishot");
 s.len=0;check(s,true,-T_E_INVAL,"pooled nonzero size");s.len=9;
 s._resv1[2]=0;check(s,true,-T_E_INVAL,"pool generation");s._resv1[2]=1;
 s._resv1[0]=1;check(s,true,-T_E_INVAL,"pooled offset residue");s._resv1[0]=0;
 s.buf_idx_or_off=64;check(s,true,-T_E_INVAL,"pool bound");
 assert(!loom_private_walk_component(NULL,0));
 const u8 *bad[]={(const u8*)".",(const u8*)"..",(const u8*)"a/b",(const u8*)"a\0b"};
 u32 n[]={1,2,3,3};for(unsigned i=0;i<4;++i)assert(loom_private_walk_component(bad[i],n[i])==-T_E_INVAL);
 u8 max[256];memset(max,'a',sizeof(max));assert(!loom_private_walk_component(max,255));
 assert(loom_private_walk_component(max,256)==-T_E_INVAL);
 assert(loom_private_walk_component(NULL,1)==-T_E_INVAL);
 assert(!loom_private_walk_component((const u8*)"..x",3));
 printf("PASS %d copied-SQE shape cases and WALK component boundaries\n",count);
}
'''
source=(D/'kernel/loom_service_decode.c').read_text()
mutants={
 'source-generation':('!s->_resv1[1]','false'),
 'source-bound':('s->handle_idx>=LOOM_SERVICE_SLOT_COUNT','false'),
 'reserved':('s->_resv0','false'),
 'flags':('if (s->flags&~allowed)','if (false && (s->flags&~allowed))'),
 'destination-generation':('&& s->_resv1[2]','&& true'),
 'same-slot':('s->offset!=s->handle_idx','true'),
 'buffer-overflow':('s->_resv1[0]<=UINT64_MAX-s->len','true'),
 'component-size':('s->len>255','false'),
 'open-mode':('s->offset>2','false'),
 'pool-feature':('if (!pools_enabled)','if (false && !pools_enabled)'),
 'pool-empty':('!s->len || s->buf_idx_or_off>=','false || s->buf_idx_or_off>='),
 'path-slash':("name[i]=='/'","false"),
 'path-dot':("(n==1 && name[0]=='.')","false")}
results=[];clang='/opt/homebrew/opt/llvm@22/bin/clang'
sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
for name,edit in [('clean',None),*mutants.items()]:
 text=source
 if edit:
  assert edit[0] in text,(name,edit);text=text.replace(*edit,1)
 c=out/(name+'.c');c.write_text(text+'\n'+harness);exe=out/name
 cmd=[clang,'-isysroot',sdk,'-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(out/'include'),'-I'+str(D/'kernel/include'),'-I'+str(R/'kernel/include'),str(c),'-o',str(exe)]
 with (out/(name+'-compile.log')).open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert r.returncode==0,(name,'compile')
 with (out/(name+'.log')).open('wb') as f:r=subprocess.run([str(exe)],stdout=f,stderr=subprocess.STDOUT)
 assert (r.returncode==0)==(name=='clean'),(name,r.returncode)
 if edit:
  expected=json.loads((D/'mutation-witnesses.json').read_text())
  assert expected[name] in (out/(name+'.log')).read_text(),(name,'wrong rejection witness')
 results.append(dict(case=name,rc=r.returncode));print('PASS',name,flush=True)
(out/'result.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(source.encode()).hexdigest(),cases=results,limits='Isolated uninstalled ABI shape decoder; no admission, authority, RPC, payload, native or dispatch qualification.'),indent=2)+'\n')
print(out)
