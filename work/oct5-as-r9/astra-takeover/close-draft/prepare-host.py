from pathlib import Path
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent;H=D/'host-tree';H.mkdir(exist_ok=True)
# Overlay only draft files; no writes through symlinks.
def overlay(sub):
 out=H/sub;out.mkdir(parents=True,exist_ok=True)
 for p in (R/sub).iterdir():
  dst=out/p.name
  if dst.exists() or dst.is_symlink():continue
  override=D/sub/p.name
  dst.symlink_to(override if override.exists() else p,target_is_directory=p.is_dir())
(H/'kernel').mkdir(exist_ok=True);(H/'kernel/include').mkdir(exist_ok=True)
overlay('kernel/include/thylacine');overlay('kernel/include');overlay('kernel')
for p in (D/'kernel/include/thylacine').iterdir():
 dst=H/'kernel/include/thylacine'/p.name
 if not dst.exists():dst.symlink_to(p)
for sub in ['arch','mm']:
 p=H/sub
 if not p.exists():p.symlink_to(R/sub,target_is_directory=True)
work=D/'work/oct5-as-r9';work.mkdir(parents=True,exist_ok=True)
(H/'work/oct5-as-r9/astra-takeover').mkdir(parents=True,exist_ok=True)
for n in ['final-reclaim-control.py','slot-owner-control.py','private-fixed-control.py','private-pool-pins-control.py','connect-control.py','request-post-control.py','protocol-control.py','astra-yip.sh']:
 p=work/n
 if not p.exists():p.write_bytes((R/'work/oct5-as-r9'/n).read_bytes());p.chmod((R/'work/oct5-as-r9'/n).stat().st_mode)
 q=H/'work/oct5-as-r9'/n
 if not q.exists():q.symlink_to(p)
for name in ['final-reclaim-control.py','slot-owner-control.py']:
 p=work/name;s=p.read_text().replace('#include <thylacine/loom_service_engine.h>','#include <thylacine/loom_service_engine.h>\n#include <thylacine/loom_service_close_queue.h>')
 s=s.replace('void *service_fixed;u32 service_fixed_count;','void *service_fixed;u32 service_fixed_count,service_fixed_settling;struct loom_close_work service_close_work;');p.write_text(s)
p=work/'connect-control.py';s=p.read_text().replace("MAIN=r'''","PRE+=r'''\nstatic void loom_private_close_changed(struct Loom *l){(void)l;CHECK(!held,\"close notification outside Ll\");}\nstatic void loom_ref(struct Loom *l){(void)l;}\nstatic void loom_unref(struct Loom *l){(void)l;}\n'''\nMAIN=r'''");p.write_text(s)
p=work/'private-fixed-control.py';s=p.read_text();s=s.replace("PRE+=r'''",'''PRE=PRE.replace('u32 service_fixed_count;','u32 service_fixed_count,service_fixed_settling;')
PRE=PRE.replace('static bool burrow_unref_settled_in(', 'static struct Loom *settling_watch;\\nstatic bool burrow_unref_settled_in(')
PRE=PRE.replace(' CHECK(!held && b->refs>0,', ' if(settling_watch) CHECK(settling_watch->service_fixed_settling==1,"physical buffer release retains settling barrier");\\n CHECK(!held && b->refs>0,')
PRE+=r\'\'\'
static int close_notifications;
static void loom_private_close_changed(struct Loom *l){CHECK(!held && !l->service_fixed_settling,"close notification after physical release");close_notifications++;}
''')
s=s.replace('l->service_fixed_count >= l->cq_entries','l->service_fixed_count + l->service_fixed_settling >= l->cq_entries')
s=s.replace(' m.offset=192;',''' m.offset=192;
 l.service_fixed_settling=1;
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&g)==-T_E_AGAIN && !g.owner,"settling fixed owner still consumes admission capacity");
 l.service_fixed_settling=0;''')
s=s.replace(' buffer.refs-=2;l.n_reg_buf=0;',' buffer.refs-=2;l.n_reg_buf=0;settling_watch=&l;')
s=s.replace(' CHECK(!held,"no retained lock");',' CHECK(close_notifications==2,"each physical fixed return notifies once");\n CHECK(!held,"no retained lock");')
s=s.replace("('allow-close','loom_private_fixed_acquire'",'''('omit-settling','loom_private_fixed_release','    ++l->service_fixed_settling;','    /* omitted */','physical buffer release retains settling barrier'),
 ('omit-close-notify','loom_private_fixed_release','    loom_private_close_changed(l);','    if(false) loom_private_close_changed(l);','each physical fixed return notifies once'),
 ('allow-close','loom_private_fixed_acquire' ''')
p.write_text(s)
print(H)
