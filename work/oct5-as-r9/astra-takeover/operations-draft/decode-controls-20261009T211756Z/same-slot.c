#include <thylacine/loom.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/errno.h>

static bool service_no_buffer(const struct loom_sqe *s) {
    return !s->len && !s->buf_idx_or_off && !s->_resv1[0];
}
static bool service_fixed_shape(const struct loom_sqe *s) {
    return s->buf_idx_or_off<LOOM_MAX_REG_BUFFERS &&
           s->_resv1[0]<=UINT64_MAX-s->len;
}
static bool service_destination_shape(const struct loom_sqe *s) {
    return s->offset<LOOM_SERVICE_SLOT_COUNT && s->_resv1[2] &&
           true;
}
int loom_private_sqe_shape(const struct loom_sqe *s, bool pools_enabled) {
    if (!s || s->_resv0 || s->handle_idx>=LOOM_SERVICE_SLOT_COUNT ||
        !s->_resv1[1]) return -T_E_INVAL;
    bool pooled=s->opcode==LOOM_OP_READ && (s->flags&LOOM_SQE_BUFFER_SELECT);
    u8 allowed=pooled ? LOOM_SQE_BUFFER_SELECT|LOOM_SQE_MULTISHOT : 0;
    if (s->flags&~allowed) return -T_E_INVAL;
    switch (s->opcode) {
    case LOOM_OP_SERVICE_CONNECT:
        if (!service_no_buffer(s) || !service_destination_shape(s)) return -T_E_INVAL;
        break;
    case LOOM_OP_WALK:
        if (!service_destination_shape(s) || s->len>255 ||
            (s->len ? !service_fixed_shape(s) : !service_no_buffer(s))) return -T_E_INVAL;
        break;
    case LOOM_OP_LOPEN:
        if (s->offset>2 || s->_resv1[2] || !service_no_buffer(s)) return -T_E_INVAL;
        break;
    case LOOM_OP_READ:
        if (pooled) {
            if (!pools_enabled) return -T_E_OPNOTSUPP;
            if (!s->len || s->buf_idx_or_off>=LOOM_SERVICE_SLOT_COUNT ||
                !s->_resv1[2] || s->_resv1[0]) return -T_E_INVAL;
            break;
        }
        // Fixed READ and WRITE have identical slice encodings.
        /* fall through */
    case LOOM_OP_WRITE:
        if (s->_resv1[2] || !service_fixed_shape(s)) return -T_E_INVAL;
        break;
    case LOOM_OP_CLUNK:
        if (s->offset || s->_resv1[2] || !service_no_buffer(s)) return -T_E_INVAL;
        break;
    default: return -T_E_OPNOTSUPP;
    }
    return 0;
}
int loom_private_walk_component(const u8 *name, u32 n) {
    if (!n) return name ? -T_E_INVAL : 0;
    if (!name || n>255 || (n==1 && name[0]=='.') ||
        (n==2 && name[0]=='.' && name[1]=='.')) return -T_E_INVAL;
    for (u32 i=0;i<n;++i) if (!name[i] || name[i]=='/') return -T_E_INVAL;
    return 0;
}


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
 s.len=256;check(s,true,-T_E_INVAL,"oversized component");
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
