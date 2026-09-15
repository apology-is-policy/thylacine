// Capture harness supplied for the implementing machine. NOT executed in the authoring workspace.
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {chromium} from 'playwright';

const here=path.dirname(fileURLToPath(import.meta.url)), root=path.dirname(here);
const manifestPath=path.resolve(process.env.FONT_MANIFEST||path.join(here,'fonts.json'));
const mode=process.env.TARGET||'historical';
if(!['historical','native'].includes(mode)) throw Error('TARGET must be historical or native');
const manifest=JSON.parse(await fs.readFile(manifestPath,'utf8'));
const output=path.resolve(process.env.OUT||path.join(here,'captures-'+mode));
// Prevent accidentally replacing a previous capture run.
await fs.mkdir(output,{recursive:false});
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
const sourceHashes={};
for(const name of ['index.html','styles.css','app.js']) sourceHashes[name]=hash(await fs.readFile(path.join(root,'reference',name)));
const fonts=[];
for(const [key,family] of [['sans','IBM Plex Sans'],[mode==='native'?'cornucopia':'mono',mode==='native'?'Cornucopia':'IBM Plex Mono']]){
  if(!manifest[key]?.length) throw Error('Missing font manifest group '+key);
  for(const f of manifest[key]){
    if(!f.source||!f.version||[f.source,f.version].includes('REQUIRED')) throw Error('Fill actual source/version provenance for '+f.path);
    const absolute=path.resolve(path.dirname(manifestPath),f.path), bytes=await fs.readFile(absolute);
    const ext=path.extname(absolute).slice(1), mime=ext==='woff2'?'font/woff2':ext==='woff'?'font/woff':'font/ttf';
    fonts.push({...f,family,absolute,sha256:hash(bytes),size:bytes.length,data:'data:'+mime+';base64,'+bytes.toString('base64')});
  }
}
for(const weight of ['400','500','600'])if(!fonts.some(f=>f.family==='IBM Plex Sans'&&f.weight===weight&&f.style==='normal'))throw Error('Missing Sans '+weight);
if(mode==='historical')for(const weight of ['400','500'])if(!fonts.some(f=>f.family==='IBM Plex Mono'&&f.weight===weight&&f.style==='normal'))throw Error('Missing Mono '+weight);
const originalCss=await fs.readFile(path.join(root,'reference/styles.css'),'utf8');
const amendedCss=await fs.readFile(path.join(root,'contrast-amendments.css'),'utf8');
const browser=await chromium.launch({headless:true});
const results=[];
const matrix=[[1440,900,[100,125,150,175,200]],[1280,720,[100,200]],[1920,1080,[100,200]],[820,900,[100]],[821,900,[100]],[390,844,[100]],[840,600,[100]]];
const themes=['signal','carbon','abyssal','oxide','combine','deusex','shock','sin','mesa','strogg','genera','mineral','logic'];
const scenarios=[];
for(const [w,h,scales] of matrix)for(const scale of scales)for(const dpr of [1,2])scenarios.push({id:`matrix-carbon-${w}x${h}-s${scale}-baseDpr${dpr}`,w,h,scale,dpr,theme:'carbon',state:'baseline'});
for(const theme of themes)for(const dpr of [1,2])scenarios.push({id:`theme-${theme}-1440x900-baseDpr${dpr}`,w:1440,h:900,scale:100,dpr,theme,state:'baseline'});
const tiles={notes:'p1',renderer:'p1',build:'p1',refs:'p1',shell:'p2',log:'p2',proc:'p2',architecture:'p3',compositor:'p3',tasks:'p3'};
const states=[...Object.keys(tiles).map(x=>'open-'+x),'hover-collapsed','hover-expanded','hover-close','dirty-inactive','dirty-attention','divider-hover','divider-drag','picker','help','divider-keyboard','picker-narrow','status-opened','status-error'];
for(const state of states)for(const dpr of [1,2])scenarios.push({id:`state-carbon-${state}-baseDpr${dpr}`,w:state==='picker-narrow'?820:1440,h:900,scale:100,dpr,theme:'carbon',state});
if(process.env.ONLY){const wanted=process.env.ONLY;for(let i=scenarios.length-1;i>=0;i--)if(!scenarios[i].id.includes(wanted))scenarios.splice(i,1);if(!scenarios.length)throw Error('ONLY matched no scenario');}

async function dump(page){
  return page.evaluate(()=>{
    const box=r=>({x:r.x,y:r.y,width:r.width,height:r.height,top:r.top,right:r.right,bottom:r.bottom,left:r.left});
    const stable=el=>{
      const pieces=[];let p=el;
      while(p&&p.nodeType===1){
        const tag=p.tagName.toLowerCase();
        const tile=p.getAttribute('data-tile-id'),pane=p.getAttribute('data-pane-id');
        if(tile)pieces.unshift(`tile[${tile}]`);
        else if(p.classList.contains('pane')&&pane)pieces.unshift(`pane[${pane}]`);
        else if(p.id)pieces.unshift('#'+p.id);
        else{const n=p.parentElement?[...p.parentElement.children].filter(x=>x.tagName===p.tagName).indexOf(p)+1:1;pieces.unshift(tag+':'+n);}
        p=p.parentElement;
      }return pieces.join('/');
    };
    const canvas=document.createElement('canvas');canvas.width=canvas.height=1;const ctx=canvas.getContext('2d',{willReadFrequently:true});
    const rgba=value=>{try{ctx.clearRect(0,0,1,1);ctx.fillStyle='rgba(0,0,0,0)';ctx.fillStyle=value;ctx.fillRect(0,0,1,1);const p=[...ctx.getImageData(0,0,1,1).data];return `rgba(${p[0]}, ${p[1]}, ${p[2]}, ${p[3]/255})`;}catch{return null;}};
    const style=cs=>{const raw={},colors={};for(const k of cs){raw[k]=cs.getPropertyValue(k);if(k.endsWith('color')&&!['scrollbar-color','print-color-adjust','color-scheme'].includes(k))colors[k]={css:raw[k],rgba:rgba(raw[k])};}return {raw,colors};};
    const intersects=(a,b)=>a.right>b.left&&a.left<b.right&&a.bottom>b.top&&a.top<b.bottom;
    function visibleRect(el,r){
      if(!intersects(r,{left:0,top:0,right:innerWidth,bottom:innerHeight}))return false;
      for(let p=el;p;p=p.parentElement){const s=getComputedStyle(p);if(s.display==='none'||s.visibility==='hidden'||Number(s.opacity)===0)return false;if(/hidden|auto|scroll|clip/.test(s.overflow+s.overflowX+s.overflowY)&&!intersects(r,p.getBoundingClientRect()))return false;}
      return r.width>0&&r.height>0;
    }
    const elements={},representatives={},pseudo=[];
    for(const el of document.querySelectorAll('[class],[id],h1,h2,p,ul,li,pre,code')){
      const key=stable(el),r=el.getBoundingClientRect(),cs=getComputedStyle(el),st=style(cs);
      elements[key]={tag:el.tagName,class:el.getAttribute('class'),id:el.id,rect:box(r),visible:visibleRect(el,r),scroll:{top:el.scrollTop,left:el.scrollLeft,width:el.scrollWidth,height:el.scrollHeight,clientWidth:el.clientWidth,clientHeight:el.clientHeight},style:st};
      for(const c of el.classList)representatives[c]??={path:key,...st};
      for(const ps of ['::before','::after']){
        const s=getComputedStyle(el,ps);if(s.content==='none'||s.content==='normal')continue;
        const num=k=>parseFloat(s.getPropertyValue(k))||0;
        const cb={x:r.x+(parseFloat(cs.borderLeftWidth)||0),y:r.y+(parseFloat(cs.borderTopWidth)||0),w:r.width-(parseFloat(cs.borderLeftWidth)||0)-(parseFloat(cs.borderRightWidth)||0),h:r.height-(parseFloat(cs.borderTopWidth)||0)-(parseFloat(cs.borderBottomWidth)||0)};
        const extraW=num('padding-left')+num('padding-right')+num('border-left-width')+num('border-right-width');
        const extraH=num('padding-top')+num('padding-bottom')+num('border-top-width')+num('border-bottom-width');
        let w=s.width==='auto'?cb.w-num('left')-num('right'):parseFloat(s.width)+(s.boxSizing==='border-box'?0:extraW);
        let h=s.height==='auto'?cb.h-num('top')-num('bottom'):parseFloat(s.height)+(s.boxSizing==='border-box'?0:extraH);
        const x=cb.x+(s.left==='auto'?cb.w-num('right')-w:num('left')),y=cb.y+(s.top==='auto'?cb.h-num('bottom')-h:num('top'));
        pseudo.push({path:key,pseudo:ps,computed:style(s),derivedBorderBox:{x,y,width:w,height:h},derivation:'absolute padding-box containing block; own box-sizing; valid for pinned divider/header/brand pseudos; not transformed paint bounds'});
      }
    }
    const textLines={};
    for(const el of document.querySelectorAll('.editor p,.editor h1,.editor h2,.editor li,.editor pre,.terminal .line')){
      if(!visibleRect(el,el.getBoundingClientRect()))continue;
      const walker=document.createTreeWalker(el,NodeFilter.SHOW_TEXT);let node,globalOffset=0;const chars=[];
      while(node=walker.nextNode()){
        for(let i=0;i<node.data.length;){const ch=String.fromCodePoint(node.data.codePointAt(i)),range=document.createRange();range.setStart(node,i);range.setEnd(node,i+ch.length);const rr=[...range.getClientRects()].filter(r=>visibleRect(el,r)).map(box);chars.push({offset:globalOffset+i,length:ch.length,char:ch,rects:rr});i+=ch.length;}
        globalOffset+=node.data.length;
      }
      const lines=[];
      for(const ch of chars)for(const rect of ch.rects){let l=lines.find(x=>Math.abs(x.top-rect.top)<.1&&Math.abs(x.bottom-rect.bottom)<.1);if(!l){l={top:rect.top,bottom:rect.bottom,text:'',fragments:[]};lines.push(l);}l.text+=ch.char;l.fragments.push({...rect,offset:ch.offset,length:ch.length});}
      textLines[stable(el)]={source:el.textContent,computedLineHeight:getComputedStyle(el).lineHeight,lines:lines.sort((a,b)=>a.top-b.top),characters:chars,baseline:null,baselineNote:'DOM Range fragments are not font baselines. Mixed-style fragments can form multiple subgroups on a visual line.'};
    }
    return {viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio},elements,representatives,pseudo,textLines,fontFaces:[...document.fonts].map(f=>({family:f.family,weight:f.weight,style:f.style,status:f.status})),rootTheme:document.documentElement.dataset.theme};
  });
}

try{
for(const scenario of scenarios){
  const effectiveDpr=scenario.scale/100*scenario.dpr;
  const context=await browser.newContext({viewport:{width:scenario.w,height:scenario.h},deviceScaleFactor:effectiveDpr,reducedMotion:'reduce',locale:'en-GB',timezoneId:'UTC',colorScheme:['genera','mineral','logic'].includes(scenario.theme)?'light':'dark'});
  await context.route(/^https?:\/\//,route=>route.abort());
  await context.addInitScript(({theme})=>{localStorage.setItem('instrument-theme',theme);const NativeDate=Date;window.Date=class extends NativeDate{constructor(...args){super(...(args.length?args:['2026-09-14T09:41:00Z']));}static now(){return new NativeDate('2026-09-14T09:41:00Z').getTime();}};},{theme:scenario.theme});
  const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(pathToFileURL(path.join(root,'reference/index.html')).href,{waitUntil:'load'});
  await page.evaluate(async records=>{for(const f of records){const face=new FontFace(f.family,`url(${f.data})`,{weight:f.weight,style:f.style});await face.load();document.fonts.add(face);}await document.fonts.ready;},fonts.map(({family,weight,style,data})=>({family,weight,style,data})));
  if(mode==='native'){
    // An explicit specimen at the old CSS sizes; not a claimed native metrics retune.
    await page.addStyleTag({content:originalCss.replaceAll('"IBM Plex Mono"','"Cornucopia"')+'\n'+amendedCss+'\n*,*::before,*::after{font-synthesis-weight:none;}'});
    await page.evaluate(()=>{document.querySelectorAll('.status-center span,kbd').forEach(el=>{el.textContent=el.textContent.replaceAll('ALT','SUPER').replaceAll('Alt','Super');});});
  }
  await page.addStyleTag({content:'*,*::before,*::after{transition:none!important;animation:none!important}.terminal .cursor{opacity:1!important}'});
  await page.evaluate(()=>{document.querySelector('#clock').textContent='09:41';document.querySelector('#status-text').textContent='READY';clearTimeout(setStatus.timer);document.querySelectorAll('*').forEach(el=>{el.scrollTop=0;el.scrollLeft=0;});});
  const state=scenario.state;
  if(state.startsWith('open-')){
    const tile=state.slice(5);await page.evaluate(({tile,pane})=>{findPane(layout,pane).expanded=tile;focusedPaneId=pane;render();},{tile,pane:tiles[tile]});
  }
  await page.mouse.move(0,0);
  if(state==='hover-collapsed')await page.locator('[data-pane-id="p1"].pane [data-open-tile="notes"]').hover();
  if(state==='hover-expanded')await page.locator('[data-pane-id="p1"].pane [data-open-tile="renderer"]').hover();
  if(state==='hover-close')await page.locator('[data-pane-id="p1"].pane [data-close-tile="renderer"]').hover();
  if(state==='dirty-inactive'||state==='dirty-attention')await page.evaluate(att=>{findPane(layout,'p1').expanded='notes';render();if(att)document.querySelector('.pane[data-pane-id="p1"] [data-tile-id="renderer"]').classList.add('attention');},state==='dirty-attention');
  if(state==='divider-hover')await page.locator('.divider[data-split-id="root"]').hover();
  if(state==='divider-keyboard'){await page.keyboard.press('Tab');await page.locator('.divider[data-split-id="root"]').focus();}
  if(state==='divider-drag'){
    const b=await page.locator('.divider[data-split-id="root"]').boundingBox(),r=await page.locator('.split[data-split-id="root"]').boundingBox();
    await page.mouse.move(b.x+3,b.y+100);await page.mouse.down();await page.mouse.move(r.x+r.width*.60,b.y+100);
  }
  if(state==='picker'||state==='picker-narrow')await page.locator('#theme-toggle').click();
  if(state==='help')await page.locator('#open-help').click();
  if(state==='status-opened'||state==='status-error')await page.evaluate(error=>{const el=document.querySelector('#status-text');el.textContent=error?'FINAL TILE IS PROTECTED':'OPENED BUILD OUTPUT';el.style.color=error?'var(--error)':'var(--amber)';},state==='status-error');
  if(mode==='native')await page.evaluate(()=>document.querySelectorAll('.terminal .path').forEach(el=>el.insertAdjacentHTML('afterend',' <span class="native-turnstile" style="color:var(--secondary)">⊢</span>')));
  // Deliberately freeze status independently of the 1800ms live timer.
  await page.evaluate(()=>{clearTimeout(setStatus.timer);if(!['OPENED BUILD OUTPUT','FINAL TILE IS PROTECTED'].includes(document.querySelector('#status-text').textContent)){document.querySelector('#status-text').textContent='READY';document.querySelector('#status-text').style.color='';}});
  await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
  const data=await dump(page),dir=path.join(output,scenario.id);await fs.mkdir(dir);
  const png=await page.screenshot({path:path.join(dir,'page.png'),type:'png',fullPage:false,animations:'disabled'});
  const meta={scenario,mode,effectiveDpr,actualDpr:data.viewport.dpr,chromium:browser.version(),node:process.version,os:{platform:os.platform(),release:os.release(),arch:os.arch()},sourceHashes,pngSha256:hash(png),fontManifest:fonts.map(({data,...f})=>f),fontSynthesis:mode==='native'?'Cornucopia bold disabled; italic synthesis requested by source if no italic file':'Source requests italics without dedicated faces; browser synthesis may apply',reducedMotion:true,freeze:{clock:'09:41',caret:'visible',animations:'disabled',transitions:'disabled',status:'fixture'},errors,overlays:mode==='native'?['Cornucopia at existing CSS sizes','contrast amendments','prompt turnstile','Super labels','capture freeze']:['pinned local FontFace bytes','capture freeze'],note:'Native scale maps to backing factor. Range boxes are fragments, not baselines. New workspace/native surfaces not mocked by this harness.'};
  await fs.writeFile(path.join(dir,'geometry-styles.json'),JSON.stringify(data,null,2));
  await fs.writeFile(path.join(dir,'metadata.json'),JSON.stringify(meta,null,2));
  results.push({id:scenario.id,pngSha256:meta.pngSha256,errors});
  if(state==='divider-drag')await page.mouse.up();
  await context.close();
  if(errors.length)throw Error('Page error in '+scenario.id+': '+errors.join('; '));
  console.log('captured',scenario.id);
}
}finally{await browser.close();await fs.writeFile(path.join(output,'manifest.json'),JSON.stringify({status:results.length===scenarios.length?'complete':'incomplete',planned:scenarios.length,captured:results.length,results},null,2));}
