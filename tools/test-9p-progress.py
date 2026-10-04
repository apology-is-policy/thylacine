#!/usr/bin/env python3
"""Actual 9P transport source, controlled byte boundaries; acquire Yip first.
Blind to locks, scheduler and SrvConn adapter ownership. No runtime activation.
"""
from pathlib import Path
import argparse,os,subprocess,tempfile,shlex
ROOT=Path(__file__).resolve().parent.parent
MUTANTS=[
 ('send-offset','p->tx + p->tx_sent, left','p->tx, left','send bytes exactly once'),
 ('recv-offset','t->recv_buf + p->rx_have, left','t->recv_buf, left','one frame exact bytes'),
 ('again-terminal','if (n == P9_TRANSPORT_EAGAIN) return 0;','if (n == P9_TRANSPORT_EAGAIN) { p9_transport_progress_abort(p); return -1; }','EAGAIN retains send'),
 ('abort-repeat','if (!p || p->aborted || !p->transport) return;','if (!p || !p->transport) return;','abort must be exactly once'),
 ('rx-bound','size > p->frame_limit','size > t->recv_cap','reject frame outside negotiated bound'),
 ('coalesced','size_t left = p->rx_goal - p->rx_have;','size_t left = p->frame_limit - p->rx_have;','receive resumes suffix'),
 ('close-before-abort','if (t->state == P9_TRANS_PROGRESS) return -1;','if (false) return -1;','legacy close cannot bypass abort'),
 ('send-limit','len > p->frame_limit','len > p->transport->recv_cap','send respects negotiated limit'),
 ('deadline','if (now_ns >= h->deadline_ns)','if (false)','absolute deadline at every handshake byte'),
 ('principal','NULL, 0, NULL, 0, h->principal','NULL, 0, NULL, 0, 0','captured principal on wire'),
 ('version:dialect','if (version_ptr[i] != P9_DEFAULT_VERSION[i]) return -1;','if (false) return -1;','unsupported dialect refused'),
 ('version:msize','if (msize < P9_HDR_LEN || s->msize < P9_HDR_LEN) return -1;','if (false) return -1;','framing-impossible msize refused'),
]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--mutants',action='store_true');parser.add_argument('--sanitize',action='store_true');parser.add_argument('--logs',type=Path);a=parser.parse_args()
    cc=os.environ.get('CC','clang');source=(ROOT/'kernel/9p_transport.c').read_text()
    with tempfile.TemporaryDirectory(prefix='9p-progress-') as d:
        d=Path(d);logs=a.logs or d;logs.mkdir(parents=True,exist_ok=True)
        cases=[('clean',None,None,None)]+(MUTANTS if a.mutants else [])
        for name,old,new,want in cases:
            path=d/(name.replace(':','-')+'.c');text=(ROOT/'kernel/9p_session.c').read_text() if name.startswith('version:') else source
            if old:
                assert old in text,name
                text=text.replace(old,new)
            path.write_text(text)
            exe=d/name
            cmd=[cc,'-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(ROOT/'kernel/include'),str(ROOT/'kernel/9p_transport.c') if name.startswith('version:') else str(path),str(ROOT/'kernel/9p_wire.c'),str(path) if name.startswith('version:') else str(ROOT/'kernel/9p_session.c'),str(ROOT/'tools/host-tests/9p-progress.c'),'-o',str(exe)]
            cmd += shlex.split(os.environ.get('CFLAGS',''))
            if a.sanitize:cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
            with (logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
            r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            (logs/(name+'.log')).write_text(r.stdout+r.stderr)
            if want:assert r.returncode==1 and want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
            else:assert r.returncode==0,(r.stdout,r.stderr)
            print('PASS '+name+(': '+want if want else ': actual transport byte-boundary fixture'),flush=True)
        with (logs/'arm64.log').open('wb') as out:
            subprocess.run([cc,'--target=aarch64-none-elf','-ffreestanding','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'arch/arm64'),str(ROOT/'kernel/9p_transport.c'),str(ROOT/'kernel/9p_srvconn_transport.c')],stdout=out,stderr=subprocess.STDOUT,check=True)
        print('PASS ARM64 transport and SrvConn adapter compilation',flush=True)
if __name__=='__main__':main()
