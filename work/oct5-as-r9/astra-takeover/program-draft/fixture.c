static struct ProcAccessIdentity admitted;
static void start_program(u8 kind,u32 mode,u16 component_len) {
    setup(kind);admitted=(struct ProcAccessIdentity){.principal_id=101,.primary_gid=202};
    admitted.supp_gid_count=1;admitted.supp_gids[0]=303;
    w.mode=mode;w.component_len=component_len;
    if(component_len)memcpy(w.component,"abc",3);
    CHECK(!loom_private_program_start(&program,&admitted,kind),"program starts");
}
static void stat_reply(u32 mode,u32 uid,u32 gid,u8 type) {
    header(160,P9_RGETATTR,rpc.tag);put32(frame+7,P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID);
    frame[15]=type;put32(frame+28,mode);put32(frame+32,uid);put32(frame+36,gid);delivered(160);
}
static enum loom_program_action advance(void){return loom_private_program_reply(&program,&reply,capture_error);}
static void synthetic_stat(u32 mode,u32 uid,u32 gid,u8 type) {
    w.built=true;w.expect.kind=P9_TGETATTR;
    reply=(struct loom_service_reply){0};reply.attr.valid=P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID;
    reply.attr.mode=mode;reply.attr.uid=uid;reply.attr.gid=gid;reply.attr.qid.type=type;
}
int main(void) {
    wire_baseline();
    start_program(P9_TLOPEN,0,0);
    CHECK(program.phase==LPROGRAM_STAT&&w.kind==P9_TGETATTR,"open starts with stat");
    CHECK(!submit(),"open stat sent");stat_reply(0400,101,202,0);
    admitted.principal_id=999;admitted.caps=CAP_DAC_OVERRIDE;admitted.supp_gids[0]=404;
    CHECK(program.identity.principal_id==101&&!program.identity.caps&&program.identity.supp_gids[0]==303,"admission snapshot detached");
    CHECK(advance()==LPROGRAM_NEXT&&w.kind==P9_TLOPEN&&program.phase==LPROGRAM_EFFECT,"authorized stat advances open");
    CHECK(!submit(),"authorized open sent");header(24,P9_RLOPEN,rpc.tag);delivered(24);
    CHECK(advance()==LPROGRAM_TERMINAL&&!program.result&&program.phase==LPROGRAM_DONE,"open terminal remains staged");
    CHECK(advance()==LPROGRAM_ABORT,"duplicate program result rejected");
    start_program(P9_TWALK,0,3);CHECK(w.kind==P9_TGETATTR,"component walk starts with stat");
    CHECK(!submit(),"walk stat sent");stat_reply(0100,101,202,P9_QTDIR);
    CHECK(advance()==LPROGRAM_NEXT&&w.kind==P9_TWALK,"search permits walk stage");
    CHECK(!submit(),"authorized walk sent");header(22,P9_RWALK,rpc.tag);frame[7]=1;delivered(22);
    CHECK(advance()==LPROGRAM_TERMINAL&&!program.result,"walk success needs owner publication");
    start_program(P9_TWALK,0,0);CHECK(w.kind==P9_TWALK&&program.phase==LPROGRAM_EFFECT,"clone has no search traversal");
    CHECK(!submit(),"clone sent");header(9,P9_RWALK,rpc.tag);delivered(9);CHECK(advance()==LPROGRAM_TERMINAL,"clone completed");
    start_program(P9_TLOPEN,1,0);CHECK(!submit(),"denied open stat sent");stat_reply(0400,101,202,0);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES&&send_calls==1,"denied open sends no effect");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,999,999,P9_QTSYMLINK);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_LOOP,"symlink open refused before DAC");
    start_program(P9_TLOPEN,0,0);program.identity.caps=CAP_DAC_OVERRIDE;synthetic_stat(0777,999,999,P9_QTSYMLINK);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_LOOP,"override cannot bypass symlink");
    start_program(P9_TWALK,0,3);synthetic_stat(0,999,999,0);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_NOTDIR,"directory type before search");
    start_program(P9_TWALK,0,3);synthetic_stat(0,101,202,P9_QTDIR);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES,"search requires execute");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,101,202,0);reply.attr.valid&=~P9_GETATTR_GID;
    CHECK(advance()==LPROGRAM_ABORT&&program.result==-T_E_IO,"missing stat field poisons scope");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,101,202,0);capture_error=-T_E_IO;
    CHECK(advance()==LPROGRAM_ABORT,"malformed capture poisons scope");
    start_program(P9_TLOPEN,0,0);reply.result=-T_E_ACCES;
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES,"remote stat error terminal");
    start_program(P9_TWRITE,0,0);w.count=3;send_rc=P9_TRANSPORT_EAGAIN;
    CHECK(submit()==-P9_E_AGAIN&&advance()==LPROGRAM_NEXT&&program.phase==LPROGRAM_EFFECT,"local shortage keeps effect phase");
    send_rc=0;CHECK(!submit(),"retried write accepted");header(11,P9_RLERROR,rpc.tag);put32(frame+7,T_E_AGAIN);delivered(11);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_AGAIN,"remote shortage terminal no replay");
    start_program(P9_TREAD,0,0);w.count=3;CHECK(!submit(),"read direct effect");header(13,P9_RREAD,rpc.tag);put32(frame+7,2);memcpy(frame+11,"xy",2);delivered(13);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==2,"short read preserved");
    start_program(P9_TREAD,0,0);reply.result=0;w.expect.kind=P9_TREAD;
    CHECK(advance()==LPROGRAM_ABORT,"uncaptured success rejected");
    start_program(P9_TREAD,0,0);w.built=true;w.expect.kind=P9_TWRITE;
    CHECK(advance()==LPROGRAM_ABORT,"wrong frame expectation rejected");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,101,202,0);reply.result=1;
    CHECK(advance()==LPROGRAM_ABORT,"positive stat result rejected");
    setup(0);admitted.principal_id=0;CHECK(loom_private_program_start(&program,&admitted,P9_TREAD)==-T_E_INVAL,"invalid principal refused");
    admitted.principal_id=101;admitted.supp_gid_count=PROC_SUPP_GIDS_MAX+1;CHECK(loom_private_program_start(&program,&admitted,P9_TREAD)==-T_E_INVAL,"invalid group snapshot refused");
    admitted.supp_gid_count=0;CHECK(loom_private_program_start(&program,&admitted,P9_TGETATTR)==-T_E_OPNOTSUPP,"user stat stage not an operation");
    w.mode=3;CHECK(loom_private_program_start(&program,&admitted,P9_TLOPEN)==-T_E_INVAL,"unallocated open mode refused");
    start_program(P9_TREAD,0,0);CHECK(loom_private_program_start(&program,&admitted,P9_TREAD)==-T_E_INVAL,"cannot recapture admitted identity");
    // Independent POSIX bit oracle, all 512 modes and owner/group/other axes.
    // Owner deliberately also matches group: owner-first must not fall through.
    for(unsigned mode=0;mode<512;++mode)for(unsigned axis=0;axis<3;++axis)for(unsigned open=0;open<3;++open) {
        start_program(P9_TLOPEN,open,0);
        u32 uid=axis==0?101:999,gid=axis<2?202:999;
        synthetic_stat(mode,uid,gid,0);
        unsigned bits=(mode>>(axis==0?6:axis==1?3:0))&7;
        unsigned need=open==0?4:open==1?2:6;
        bool allowed=(bits&need)==need;
        enum loom_program_action a=advance();
        CHECK(allowed?(a==LPROGRAM_NEXT):(a==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES),"POSIX open permission matrix");
    }
    start_program(P9_TLOPEN,0,0);synthetic_stat(0040,999,303,0);CHECK(advance()==LPROGRAM_NEXT,"captured supplementary group");
    start_program(P9_TLOPEN,0,0);program.identity.principal_id=PRINCIPAL_SYSTEM;synthetic_stat(0,999,999,0);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES,"system principal has no DAC bypass");
    u64 caps[]={CAP_DAC_OVERRIDE,CAP_HOSTOWNER};
    for(unsigned i=0;i<2;++i){start_program(P9_TLOPEN,2,0);program.identity.caps=caps[i];synthetic_stat(0,999,999,0);CHECK(advance()==LPROGRAM_NEXT,"captured capability override");}
    printf("PASS staged program: %d total checks\n",checks);return 0;
}
