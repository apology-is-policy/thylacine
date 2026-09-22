#!/usr/bin/env python3
"""Round-2 authored ANSI tables and minimal explicit contrast amendments."""
import json, tomllib, re, hashlib
from pathlib import Path
R=Path(__file__).resolve().parent
OLD=R.parent/'halcyon-carbon-handoff'
NAMES=json.loads((OLD/'resolved-tokens.json').read_text())
# Independently authored semantic ANSI16, in conventional slot order.
# Bright slots follow appearance polarity, not a generic signal-color alias.
ANSI={
'signal': ['#666D69','#BD7770','#7D9D78','#BAA15D','#7B98BA','#AA86AD','#6AA7A5','#C1C7C1','#909992','#E29890','#9DC196','#DDC57F','#A0BBDF','#CBA8CD','#8DCAC6','#EDF1E9'],
'carbon': ['#666D68','#B97B77','#829F88','#B5A26B','#819EBB','#A48CAE','#79A6AA','#BEC5BF','#929B94','#D69A95','#A5BFAA','#D1C08D','#A6BDCF','#C3ACCB','#9EC3C4','#F2F3EF'],
'abyssal':['#64716F','#B87977','#77A58B','#B0A16B','#779FC4','#A28CAF','#69ADAD','#B9CBC6','#8FA6A0','#D99B96','#9CC7AC','#D1C18C','#9DC1E5','#C2AED0','#8DCECB','#E4F1EA'],
'oxide':['#716960','#BF8070','#92A179','#BBA16B','#839AB2','#B08DAA','#7CA8A2','#D0C5B7','#A29687','#DDA18D','#B2BE99','#DCC18D','#A7B8CF','#D0AEC7','#9DC7BD','#F1E7D8'],
'combine':['#647078','#B87C76','#7CA18D','#B7A26C','#7CA2C5','#A78DB5','#6EADB9','#BECBD0','#90A0AB','#D89E95','#9EC2AA','#D6C28C','#9FC4E5','#C5AFD2','#94CDD3','#E6EFF2'],
'deusex':['#65717B','#B57B78','#82A18C','#B7A062','#7F9FC8','#A98DB9','#74A9B7','#BCC8D3','#93A1B0','#D99B96','#A4C0A8','#DBC385','#A4BFE8','#CCAFD7','#9BC9D2','#E8EDF3'],
'shock':['#756B7A','#BB7A87','#84A18E','#B4A16E','#8A9AC5','#AC86B9','#76ABA9','#CBC1D1','#A497AC','#DDA0AD','#A6C3AC','#D7C492','#ADBCE7','#CAAADB','#9ACAC5','#EEE7F3'],
'sin':['#68756B','#BC7C70','#86A078','#B8A063','#819AB8','#A88CAC','#70A99C','#C4CCC0','#94A18E','#DE9E8B','#A8C295','#D9C282','#A4BADA','#CAAECC','#96CABB','#EBF0E4'],
'mesa':['#6C746B','#B97B70','#88A17D','#B5A16B','#819AB3','#A68EAB','#78A79D','#C7CDC1','#98A18E','#DC9E91','#ABC19A','#D8C38D','#A5BAD1','#C7AEC9','#9AC6B9','#ECF0E4'],
'strogg':['#776B60','#BC796E','#929B74','#B49D65','#8999AE','#AE88A0','#7FA59A','#CEC2B3','#A59581','#DB9D8A','#B3BA94','#D5BD87','#ABB8C9','#CCABC0','#A2C3B5','#EDE1CF'],
'genera':['#737A75','#9B4948','#456D4B','#82622C','#435E91','#83527D','#326F72','#303A34','#5C655F','#813432','#305637','#6B4D19','#304773','#693C65','#215759','#18221C'],
'mineral':['#6D776E','#984B46','#416D4B','#7D622C','#405F89','#7F537A','#2E6E68','#2C3930','#56645A','#7E3632','#2D5436','#674D1C','#2D486B','#633D61','#1D5550','#15251A'],
'logic':['#7A736C','#984D43','#536D45','#85602E','#4B5F86','#825076','#3F6D69','#39312A','#655C53','#7E382F','#3E5432','#6D4A1D','#364867','#693A5E','#2B5350','#211A14'],
}
def lum(c):
    a=[x/255 for x in bytes.fromhex(c[1:])]
    a=[x/12.92 if x<=.04045 else ((x+.055)/1.055)**2.4 for x in a]
    return sum(x*w for x,w in zip(a,(.2126,.7152,.0722)))
def cr(a,b):
    x,y=sorted((lum(a),lum(b)));return (y+.05)/(x+.05)
def tint(a,b,p):
    return '#'+''.join(f'{int(x+(y-x)*p+.5):02X}' for x,y in zip(bytes.fromhex(a[1:]),bytes.fromhex(b[1:])))
def flatten(d,prefix=''):
    return {f'{prefix}{k}':v for k,v in d.items() if not isinstance(v,dict)} | {k:v for a,b in d.items() if isinstance(b,dict) for k,v in flatten(b,f'{prefix}{a}.').items()}
def serialize(d):
    lines=[]
    def walk(section,values):
        lines.append('['+section+']')
        for k,v in values.items():
            if not isinstance(v,dict):lines.append(k+' = '+json.dumps(v,ensure_ascii=False))
        lines.append('')
        for k,v in values.items():
            if isinstance(v,dict):walk(section+'.'+k,v)
    for k,v in d.items():walk(k,v)
    return '\n'.join(lines)
def main():
    weak=[r for r in json.loads((OLD/'contrast-report.json').read_text()) if r['ratio']<4.5]
    assert len(weak)==45
    revised=json.loads(json.dumps(NAMES)); changes=[];ansi_report=[]
    for r in weak:
        t=revised['themes'][r['theme']]; c=t['colors']; target='#000000' if t['light'] else '#FFFFFF'
        old=c[r['token']]
        for step in range(1,1001):
            new=tint(old,target,step/1000)
            if cr(new,r['bg'])>=4.6:break
        c[r['token']]=new
        changes.append({**r,'old':old,'new':new,'new_ratio':round(cr(new,r['bg']),4)})
    assert revised['themes']['carbon']==NAMES['themes']['carbon']
    template=tomllib.loads((OLD/'source-docs/09-TEMPLATE.toml').read_text())
    for id,t in revised['themes'].items():
        ansi=ANSI[id]; assert len(ansi)==len(set(ansi))==16
        bg=t['colors']['terminal-bg']
        for i,c in enumerate(ansi):
            assert cr(c,bg)>=3,(id,i,c,cr(c,bg))
            ansi_report.append({'theme':id,'slot':i,'hex':c,'background':bg,'contrast':round(cr(c,bg),4)})
        for i in range(8):assert (lum(ansi[i+8])<lum(ansi[i]))==t['light'],(id,i)
        for offset in (0,8):
            lums=list(map(lum,ansi[offset:offset+8]))
            assert lums[0]==(max(lums) if t['light'] else min(lums)),(id,offset,'black')
            assert lums[7]==(min(lums) if t['light'] else max(lums)),(id,offset,'white')
        stock=tomllib.loads((OLD/'palettes'/f'{id}.toml').read_text()); c=t['colors']
        stock['terminal']['ansi']=ansi
        stock['palette']['fg_subtle']=c['dim']
        for fam in ('sage','cinnabar'):stock['palette'][fam]['fg_muted']=c['dim']
        for slot,token in {'slate':'keyword','sage':'type','sand':'attribute','moss':'number','ash':'function','dusk':'string','smoke':'comment'}.items():
            stock['palette']['syntax'][slot]=c['syntax-'+token]
        assert set(flatten(stock))==set(flatten(template))
        (R/'palettes'/f'{id}.toml').write_text('# Round 2: authored semantic ANSI16 + explicit contrast corrections.\n# Still a stock-schema compatibility projection, not the full UI profile.\n'+serialize(stock))
        side=tomllib.loads((OLD/'ui-palettes'/f'{id}.toml').read_text())
        side['color']={k.replace('-','_'):v for k,v in c.items()}
        (R/'ui-palettes'/f'{id}.toml').write_text('# Round 2 exact-role sidecar. Requires NEW Instrument loader.\n'+serialize(side))
    (R/'resolved-tokens-round2.json').write_text(json.dumps(revised,indent=2)+'\n')
    (R/'ansi16.json').write_text(json.dumps(ANSI,indent=2)+'\n')
    (R/'ansi-contrast.json').write_text(json.dumps(ansi_report,indent=2)+'\n')
    (R/'contrast-changes.json').write_text(json.dumps(changes,indent=2)+'\n')
    css=['/* Overlay only for revised native target, NEVER for historical v5 goldens. */']
    for id in revised['themes']:
        rows=[r for r in changes if r['theme']==id]
        if rows:css+=[':root[data-theme="'+id+'"] {']+[f'  --{r["token"]}: {r["new"]};' for r in rows]+['}']
    (R/'contrast-amendments.css').write_text('\n'.join(css)+'\n')
    md=['# Contrast amendments — exact replacements','',
        '45 existing failing pairs corrected to at least 4.6:1. Carbon unchanged. Only listed roles change.',
        '', '| Theme | Token | Old | New | Measured ground | Old ratio | New ratio |','|---|---|---|---|---|---|---|']
    md += [f'| {r["theme"]} | {r["token"]} | {r["old"]} | {r["new"]} | {r["bg"]} | {r["ratio"]} | {r["new_ratio"]} |' for r in changes]
    (R/'CONTRAST-AMENDMENTS.md').write_text('\n'.join(md)+'\n')
    md=['# Authored ANSI16 register','', 'Slots 0–7 normal, 8–15 bright. RGB8 sRGB. Bright means lighter on dark, darker on light.',
        '', '| Theme | 0 black | 1 red | 2 green | 3 yellow | 4 blue | 5 magenta | 6 cyan | 7 white |','|---|---|---|---|---|---|---|---|---|']
    for id,a in ANSI.items():md += ['| '+id+' normal | '+' | '.join(a[:8])+' |','| '+id+' bright | '+' | '.join(a[8:])+' |']
    (R/'ANSI16.md').write_text('\n'.join(md)+'\n')
    validation={'themes':13,'ansi_slots':len(ansi_report),'minimum_ansi_contrast':min(r['contrast'] for r in ansi_report),'changed_pairs':len(changes),'carbon_unchanged':True,'all_ansi_distinct':True,'bright_polarity':'PASS','black_white_order_per_ramp':'PASS','stock_schema':'PASS 57-key template shape','stock_ansi':'PASS 16 distinct authored slots per theme','contrast_amendments':'PASS 45 pairs >=4.6:1','reference_commit':'074bc5646b2f7a03872859890c2891017ceaaf9d','capture_harness':'SYNTAX-CHECKED ONLY; not run in authoring workspace','goldens':'NOT CAPTURED; exact font bytes and controlled browser matrix were unavailable','cornucopia':'MANIFEST-REQUIRED; TTF not attached','native_halcyon':'NOT RUN; native OS repository not attached'}
    (R/'VALIDATION.json').write_text(json.dumps(validation,indent=2)+'\n')
    files=sorted(p for p in R.rglob('*') if p.is_file() and p.name not in ('SHA256SUMS','VALIDATION.json') and '__pycache__' not in p.parts)
    (R/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(R).as_posix()+'\n' for p in files))
    print(json.dumps(validation,indent=2))
if __name__=='__main__':main()
