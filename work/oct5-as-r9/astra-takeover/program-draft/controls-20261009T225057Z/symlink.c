#include <thylacine/loom_service_program.h>
#include <thylacine/perm.h>
#include <thylacine/syscall.h>
#include <thylacine/errno.h>

int loom_private_program_start(struct loom_service_program *p,
                              const struct ProcAccessIdentity *id,u8 kind) {
    if(!p || !id || p->phase!=LPROGRAM_NEW || !id->principal_id ||
       id->supp_gid_count>PROC_SUPP_GIDS_MAX)return -T_E_INVAL;
    if(kind!=P9_TWALK && kind!=P9_TLOPEN && kind!=P9_TREAD &&
       kind!=P9_TWRITE && kind!=P9_TCLUNK)return -T_E_OPNOTSUPP;
    if((kind==P9_TLOPEN && p->wire.mode>2) ||
       (kind==P9_TWALK && p->wire.component_len>255))return -T_E_INVAL;
    // Explicit copy avoids a freestanding libc dependency and detaches the
    // authority from the caller's stack. Identity is never resampled on retry.
    const u8 *src=(const u8 *)id;u8 *dst=(u8 *)&p->identity;
    for(size_t i=0;i<sizeof(*id);++i)dst[i]=src[i];
    p->operation=kind;p->result=0;
    bool stat=kind==P9_TLOPEN || (kind==P9_TWALK && p->wire.component_len);
    p->phase=stat?LPROGRAM_STAT:LPROGRAM_EFFECT;
    p->wire.kind=stat?P9_TGETATTR:kind;
    return 0;
}
static enum loom_program_action program_end(struct loom_service_program *p,s32 result,
                                            enum loom_program_action action) {
    p->result=result;p->phase=LPROGRAM_DONE;return action;
}
enum loom_program_action loom_private_program_reply(struct loom_service_program *p,
                            const struct loom_service_reply *r,int capture_error) {
    if(!p)return LPROGRAM_ABORT;
    if(!r || (p->phase!=LPROGRAM_STAT && p->phase!=LPROGRAM_EFFECT) || capture_error)
        return program_end(p,-T_E_IO,LPROGRAM_ABORT);
    // Only submit-return proof licenses no-wire retry. A server's EAGAIN is
    // an ordinary terminal error, including after a prior local shortage.
    if(loom_private_wire_retry(&p->wire,0,r))return LPROGRAM_NEXT;
    if(r->result<0)return program_end(p,r->result,LPROGRAM_TERMINAL);
    if(!p->wire.built || p->wire.expect.kind!=p->wire.kind)
        return program_end(p,-T_E_IO,LPROGRAM_ABORT);
    if(p->phase==LPROGRAM_STAT) {
        u64 needed=P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID;
        if(p->wire.kind!=P9_TGETATTR || r->result || (r->attr.valid&needed)!=needed)
            return program_end(p,-T_E_IO,LPROGRAM_ABORT);
        // Native private services use the local numeric authority domain.
        // They are never foreign/cape mounts; no identity is taken from worker.
        struct t_stat st={0};
        st.mode=r->attr.mode;st.uid=r->attr.uid;st.gid=r->attr.gid;
        unsigned want;
        if(p->operation==P9_TWALK) {
            // Match ordinary single-hop fail ordering: type before X-search.
            if(!(r->attr.qid.type&P9_QTDIR))
                return program_end(p,-T_E_NOTDIR,LPROGRAM_TERMINAL);
            want=PERM_X;
        } else if(p->operation==P9_TLOPEN) {
            // Relative private FIDs have no namespace symlink resolver. A
            // link's permissive mode cannot authorize opening its target.
            if(false)
                return program_end(p,-T_E_LOOP,LPROGRAM_TERMINAL);
            if(p->wire.mode>2)return program_end(p,-T_E_INVAL,LPROGRAM_TERMINAL);
            want=perm_want_for_omode(p->wire.mode);
        } else return program_end(p,-T_E_IO,LPROGRAM_ABORT);
        if(perm_check_identity(&p->identity,&st,want))
            return program_end(p,-T_E_ACCES,LPROGRAM_TERMINAL);
        p->phase=LPROGRAM_EFFECT;p->wire.kind=p->operation;
        return LPROGRAM_NEXT;
    }
    if(p->wire.kind!=p->operation)return program_end(p,-T_E_IO,LPROGRAM_ABORT);
    return program_end(p,r->result,LPROGRAM_TERMINAL);
}
