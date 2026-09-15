#!/usr/bin/env python3
"""Reproducible extraction, palette conversion, and offline packaging. Python 3.11+."""
import hashlib, html, json, re, shutil, tomllib, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CSS = (ROOT / 'reference/styles.css').read_text()
APP = (ROOT / 'reference/app.js').read_text()
NAMES = dict(re.findall(r'(\w+): "([^"]+)"', re.search(r'const themeNames = \{(.*?)\};', APP).group(1)))
BASE = dict(re.findall(r'--([\w-]+):\s*([^;]+);', CSS.split('\n}\n', 1)[0]))

def resolve(t):
    def one(k, seen=()):
        if k in seen: raise ValueError('cyclic token')
        v = t[k]
        if v.startswith('var('): return one(v[6:-1], (*seen,k))
        return v.upper() if v.startswith('#') else v
    return {k: one(k) for k in t}

def mix(a,b,p):
    aa,bb=[bytes.fromhex(c[1:]) for c in (a,b)]
    return '#' + ''.join(f'{int(x*(1-p)+y*p+0.5):02X}' for x,y in zip(aa,bb))

def flatten(d, prefix=''):
    out={}
    for k,v in d.items():
        key=f'{prefix}.{k}' if prefix else k
        if isinstance(v,dict): out.update(flatten(v,key))
        else: out[key]=v
    return out

def toml(d):
    out=[]
    for section, values in d.items():
        out.append(f'[{section}]')
        for key,value in values.items(): out.append(f'{key} = {json.dumps(value, ensure_ascii=False)}')
        out.append('')
    return '\n'.join(out)

def luminance(c):
    v=[n/255 for n in bytes.fromhex(c[1:])]
    v=[n/12.92 if n<=.04045 else ((n+.055)/1.055)**2.4 for n in v]
    return sum(n*w for n,w in zip(v,(.2126,.7152,.0722)))

def contrast(a,b):
    x,y=sorted((luminance(a),luminance(b)))
    return (y+.05)/(x+.05)

def palette(name,t,light):
    p={
        'floor':t['desktop'],'surface':t['open'],'header':t['header'],
        'raised':t['hover'],'border':t['structure'],'blank':t['pane'],
        'selection':mix(t['open'],t['amber'],.15),'island_rule':t['amber-muted'],
        'fg':t['text'],'fg_dim':t['body-text'],'fg_muted':t['secondary'],'fg_subtle':t['dim'],
        'bevel_top':mix(t['desktop'],'#FFFFFF',.16),
        'bevel_left':mix(t['desktop'],'#FFFFFF',.09),
        'bevel_right':mix(t['desktop'],'#000000',.22),
        'bevel_bottom':mix(t['desktop'],'#000000',.46),
        'ember':t['amber'],'ember_dim':t['amber-muted'],'ember_deep':t['amber-muted'],
        'status_bg':t['rail'],'status_fg':t['text'],'status_muted':t['secondary'],'status_idle':t['dim']}
    families={}
    for family,signal in [('sage','success'),('cinnabar','error')]:
        families['palette.'+family]={'key':t[signal],'tint':t['header'],'raised':t['hover'],
            'border':t['separator'],'fg':t['text'],'fg_dim':t['secondary'],'fg_muted':t['dim']}
    syntax=dict(zip(('slate','sage','sand','moss','ash','dusk','smoke','fen','cinnabar'),
        [t[k] for k in ('syntax-keyword','syntax-type','syntax-attribute','syntax-number',
                       'syntax-function','syntax-string','syntax-comment','success','error')]))
    # Authored ANSI approximation: the browser did not define an ANSI table.
    ansi=[t['pane'],t['error'],t['success'],t['amber'],t['terminal-path'],t['syntax-number'],
          mix(t['terminal-path'],t['success'],.5),t['secondary'],t['dim']]
    for c in ansi[1:7]: ansi.append(mix(c,t['text'],.28))
    ansi.append(t['terminal-text'])
    # Stable de-duplication with a one-channel increment; recorded as derived.
    seen=set()
    for i,c in enumerate(ansi):
        while c in seen: c=f'#{(int(c[1:],16)+1)&0xFFFFFF:06X}'
        ansi[i]=c;seen.add(c)
    return {'meta':{'name':name},'palette':p,**families,'palette.syntax':syntax,
        'terminal':{'bg':t['terminal-bg'],'fg':t['terminal-text'],'ansi':ansi},
        'type':{'smooth':12 if light else 0},
        'geometry':{'bevel':2,'gap':3,'hairline':1,'header_h':32,'status_h':25,'tag_pad_x':0,'tab_strip_h':0}}

def main():
    resolved={}; contrasts=[]
    template=tomllib.loads((ROOT/'source-docs/09-TEMPLATE.toml').read_text())
    expected=set(flatten(template)); expected.remove('meta.name')
    assert len(expected)==57, len(expected)
    for key,name in NAMES.items():
        t=BASE.copy()
        if key!='signal':
            block=re.search(r':root\[data-theme="'+key+r'"\] \{(.*?)\n\}',CSS,re.S).group(1)
            t.update(dict(re.findall(r'--([\w-]+):\s*([^;]+);',block)))
        t=resolve(t); colors={k:v for k,v in t.items() if v.startswith('#')}
        assert len(colors)==35, len(colors)
        light=key in ('genera','mineral','logic')
        resolved[key]={'name':name,'light':light,'colors':colors}
        data=palette(name,t,light)
        target=ROOT/'palettes'/f'{key}.toml'
        target.write_text('# Stock Halcyon schema: complete 57-key compatibility projection.\n'
            '# Not pixel-exact alone: pair with ui-palettes/'+key+'.toml AFTER implementing Instrument v1.\n'
            '# Ember changes are an explicit new-profile policy amendment. No base inheritance.\n'
            '# ANSI and bevel faces are DERIVED, not sampled from the mockup.\n'+toml(data))
        parsed=tomllib.loads(target.read_text()); flat=flatten(parsed)
        assert set(flat)-{'meta.name'}==expected
        assert all(re.fullmatch('#[0-9A-F]{6}',v) for k,v in flat.items() if k.startswith('palette.'))
        assert len(parsed['terminal']['ansi'])==len(set(parsed['terminal']['ansi']))==16
        assert len(target.read_bytes())<65536
        bounds={'bevel':(2,64),'gap':(0,64),'hairline':(1,32),'header_h':(1,256),
                'status_h':(1,256),'tag_pad_x':(0,128),'tab_strip_h':(0,128)}
        for k,(lo,hi) in bounds.items(): assert lo<=parsed['geometry'][k]<=hi
        assert 0<=parsed['type']['smooth']<=200
        side={'meta':{'schema':1,'id':key,'name':name,'profile':'instrument-v1','color_scheme':'light' if light else 'dark'},
              'color':{k.replace('-','_'):v for k,v in colors.items()}}
        companion=ROOT/'ui-palettes'/f'{key}.toml'
        companion.write_text('# NEW sidecar schema; NEVER install as theme.toml.\n'+toml(side))
        cp=tomllib.loads(companion.read_text())
        assert cp['color']==side['color'] and len(cp['color'])==35
        for k in ('text','secondary','dim','syntax-keyword','syntax-type','syntax-function','syntax-string','syntax-number','syntax-attribute','syntax-lifetime','syntax-comment','syntax-punctuation'):
            bg=t['code-bg'] if k.startswith('syntax-') else t['header']
            contrasts.append({'theme':key,'token':k,'fg':t[k],'bg':bg,'ratio':round(contrast(t[k],bg),3)})
    (ROOT/'resolved-tokens.json').write_text(json.dumps({'default':'carbon','themes':resolved},indent=2)+'\n')
    (ROOT/'contrast-report.json').write_text(json.dumps(contrasts,indent=2)+'\n')
    lines=['# Exact resolved palette register','',
      'Generated from the pinned CSS, not the earlier abbreviated themes.json. All values are sRGB RGB8.','',
      '| Token | '+' | '.join(NAMES)+' |','|---|'+'---|'*len(NAMES)]
    for token in resolved['carbon']['colors']:
        lines.append('| '+token+' | '+' | '.join(resolved[k]['colors'][token] for k in NAMES)+' |')
    lines += ['','## Carbon Optics syntax contrast','', '| Token | Foreground | Code ground | Ratio |','|---|---|---|---|']
    for r in contrasts:
        if r['theme']=='carbon' and r['token'].startswith('syntax-'):
            lines.append(f"| {r['token']} | {r['fg']} | {r['bg']} | {r['ratio']}:1 |")
    (ROOT/'PALETTE-REGISTER.md').write_text('\n'.join(lines)+'\n')
    # The frozen reference remains byte-for-byte intact. Build a separate offline convenience page.
    page=(ROOT/'reference/index.html').read_text()
    page=re.sub(r'  <link rel="preconnect"[^>]+>\n','',page)
    page=re.sub(r'  <link href="https://fonts.googleapis.com[^>]+>\n','',page)
    page=page.replace('<link rel="stylesheet" href="styles.css">','<style>\n'+CSS+'\n</style>')
    page=page.replace('<script src="app.js"></script>','<script>\n'+APP+'\n</script>')
    page=page.replace('localStorage.getItem("instrument-theme")||"signal"','localStorage.getItem("halcyon-reference-theme")||"carbon"')
    page=page.replace('dataset.theme="signal"','dataset.theme="carbon"')
    page=page.replace('localStorage.setItem("instrument-theme", theme)','localStorage.setItem("halcyon-reference-theme", theme)')
    (ROOT/'prototype-offline.html').write_text(page)
    # Static readable manual; embedded text avoids a CDN/Markdown runtime dependency.
    docs=['README.md','IMPLEMENTATION-SPEC.md','MIGRATION-GUIDE.md','THEME-CONVERSION.md','ACCEPTANCE-TESTS.md','AGENT-START.md','PALETTE-REGISTER.md']
    body='<nav>'+''.join(f'<a href="#{i}">{html.escape(n)}</a>' for i,n in enumerate(docs))+'</nav>'
    for i,n in enumerate(docs): body+=f'<section id="{i}"><h1>{html.escape(n)}</h1><pre>{html.escape((ROOT/n).read_text())}</pre></section>'
    (ROOT/'manual.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Halcyon · Carbon Optics implementation manual</title><style>body{margin:0;background:#090b0c;color:#d9ddd9;font:16px/1.6 system-ui}nav{padding:24px;border-bottom:1px solid #454b48;display:flex;gap:18px;flex-wrap:wrap}a{color:#c7b98b}section{max-width:1100px;margin:50px auto;padding:0 25px}h1{color:#f2f3ef;font-size:26px}pre{font:14px/1.65 ui-monospace,monospace;white-space:pre-wrap;overflow-wrap:anywhere}section:last-child{max-width:none} @media print{body{background:white;color:black}nav{display:none}section{break-before:page}}</style>'+body+'</html>')
    tomllib.loads((ROOT/'instrument-profile.toml').read_text())
    fixture=json.loads((ROOT/'fixtures.json').read_text())
    assert fixture['defaultTheme']=='carbon'
    assert len(resolved)==13
    assert all(r['ratio']>=4.5 for r in contrasts if r['theme']=='carbon' and r['token'].startswith('syntax-'))
    files=[p for p in ROOT.rglob('*') if p.is_file() and p.name not in ('SHA256SUMS','VALIDATION.json')]
    (ROOT/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(ROOT).as_posix()+'\n' for p in sorted(files)))
    validation={'themes':len(resolved),'legacy_keys_per_theme_excluding_meta_name':len(expected),
      'lossless_colors_per_theme':35,'toml_syntax':'PASS Python tomllib',
      'stock_key_shape':'PASS exact template key set','ansi_length_uniqueness':'PASS',
      'stock_geometry_bounds':'PASS','carbon_syntax_contrast':'PASS all 9 categories >=4.5:1',
      'source_sha':'074bc5646b2f7a03872859890c2891017ceaaf9d',
      'native_halcyon_lint':'NOT RUN: Halcyon source/binary not attached',
      'native_pixel_and_behavior_tests':'NOT RUN: implementation deliverable is a specification',
      'offline_fonts':'Not bundled; system-installed Plex required for typography match',
      'contrast_policy':'Measured, not repaired; some non-Carbon tokens below 4.5:1 are preserved for fidelity'}
    (ROOT/'VALIDATION.json').write_text(json.dumps(validation,indent=2)+'\n')
    archive=ROOT.parent/'Halcyon-Carbon-Optics-Implementation-Kit.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(ROOT.rglob('*')):
            if p.is_file(): z.write(p,p.relative_to(ROOT.parent))
    with zipfile.ZipFile(archive) as z: assert z.testzip() is None
    print(json.dumps(validation,indent=2)); print(archive)

if __name__=='__main__': main()
