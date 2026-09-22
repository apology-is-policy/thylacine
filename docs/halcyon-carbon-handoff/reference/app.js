const tileCatalog = {
  renderer: {
    name: "src / renderer.rs", meta: "MODIFIED", kind: "editor", state: "dirty",
    content: `<div class="editor"><div class="doc-path">src / renderer.rs · 248 lines</div><h1>Compositor geometry should remain legible under motion.</h1><p>The layout is a recursive tree of splits. Leaves own ordered tile stacks; every leaf maintains exactly one <code>expandedTile</code>.</p><h2>Resize constraints</h2><ul><li>Reserve the header budget before allocating body height.</li><li>Clamp each pane to its minimum usable dimension.</li><li>Keep divider movement continuous and reversible.</li></ul><pre><code><span class="syn-comment">// Ratios survive display changes; geometry is clamped at layout time.</span>
<span class="syn-keyword">const</span> MIN_RATIO<span class="syn-punctuation">:</span> <span class="syn-type">f32</span> <span class="syn-punctuation">=</span> <span class="syn-number">0.22</span><span class="syn-punctuation">;</span>
<span class="syn-keyword">const</span> MAX_RATIO<span class="syn-punctuation">:</span> <span class="syn-type">f32</span> <span class="syn-punctuation">=</span> <span class="syn-number">0.78</span><span class="syn-punctuation">;</span>

<span class="syn-attribute">#[derive</span><span class="syn-punctuation">(</span><span class="syn-type">Clone</span><span class="syn-punctuation">,</span> <span class="syn-type">Copy</span><span class="syn-punctuation">)]</span>
<span class="syn-keyword">struct</span> <span class="syn-type">Rect</span> <span class="syn-punctuation">{</span> width<span class="syn-punctuation">:</span> <span class="syn-type">f32</span><span class="syn-punctuation">,</span> height<span class="syn-punctuation">:</span> <span class="syn-type">f32</span> <span class="syn-punctuation">}</span>

<span class="syn-keyword">fn</span> <span class="syn-function">solve</span><span class="syn-punctuation">&lt;</span><span class="syn-lifetime">'a</span><span class="syn-punctuation">&gt;(</span>node<span class="syn-punctuation">:</span> <span class="syn-punctuation">&amp;</span><span class="syn-lifetime">'a</span> <span class="syn-type">SplitNode</span><span class="syn-punctuation">,</span> bounds<span class="syn-punctuation">:</span> <span class="syn-type">Rect</span><span class="syn-punctuation">)</span> <span class="syn-punctuation">-&gt; [</span><span class="syn-type">Rect</span><span class="syn-punctuation">;</span> <span class="syn-number">2</span><span class="syn-punctuation">] {</span>
    <span class="syn-keyword">let</span> ratio <span class="syn-punctuation">=</span> node<span class="syn-punctuation">.</span>ratio<span class="syn-punctuation">.</span><span class="syn-function">clamp</span><span class="syn-punctuation">(</span>MIN_RATIO<span class="syn-punctuation">,</span> MAX_RATIO<span class="syn-punctuation">);</span>
    <span class="syn-macro">debug_assert!</span><span class="syn-punctuation">(</span>ratio<span class="syn-punctuation">.</span><span class="syn-function">is_finite</span><span class="syn-punctuation">(),</span> <span class="syn-string">"split ratio must be finite"</span><span class="syn-punctuation">);</span>

    <span class="syn-keyword">match</span> node<span class="syn-punctuation">.</span>axis <span class="syn-punctuation">{</span>
        <span class="syn-type">Axis</span><span class="syn-punctuation">::</span><span class="syn-type">Vertical</span>   <span class="syn-punctuation">=&gt;</span> bounds<span class="syn-punctuation">.</span><span class="syn-function">split_x</span><span class="syn-punctuation">(</span>ratio<span class="syn-punctuation">),</span>
        <span class="syn-type">Axis</span><span class="syn-punctuation">::</span><span class="syn-type">Horizontal</span> <span class="syn-punctuation">=&gt;</span> bounds<span class="syn-punctuation">.</span><span class="syn-function">split_y</span><span class="syn-punctuation">(</span>ratio<span class="syn-punctuation">),</span>
    <span class="syn-punctuation">}</span>
<span class="syn-punctuation">}</span></code></pre><p>The active rail uses <span class="selection">geometry, weight, and luminance</span> so focus remains visible without color.</p></div>`
  },
  notes: { name: "project / notes", meta: "12 LINES", kind: "editor", content: `<div class="editor"><div class="doc-path">project / notes</div><h1>Workspace behavior</h1><p>A pane is spatial structure. A tile is content. Their chrome should never collapse into the same visual metaphor.</p><h2>Working principles</h2><ul><li>One expanded tile per pane.</li><li>All other headers remain visible.</li><li>No overlapping surfaces.</li></ul></div>` },
  build: { name: "build output", meta: "PASSED · 1.8s", kind: "terminal", content: `<div class="terminal"><div class="line comment">$ cargo build --release</div><div class="line">   Compiling lattice-core v0.8.2</div><div class="line">   Compiling instrument-shell v0.4.0</div><div class="line success">    Finished release [optimized] target(s) in 1.83s</div><div class="line"> </div><div class="line"><span class="prompt">λ</span> <span class="path">~/systems/compositor</span> <span class="cursor"></span></div></div>` },
  refs: { name: "references", meta: "6 ITEMS", kind: "editor", content: `<div class="editor"><div class="doc-path">references</div><h1>Interface references</h1><ul><li>Acme window tags and columns</li><li>Laboratory control surfaces</li><li>Typesetting grids</li><li>Physical patch panels</li></ul></div>` },
  shell: { name: "shell", meta: "RUNNING", kind: "terminal", content: `<div class="terminal"><div class="line comment">instrument shell · session 04</div><div class="line"><span class="prompt">λ</span> <span class="path">~/systems/compositor</span> <span class="command">git status --short</span></div><div class="line"> M src/renderer.rs</div><div class="line"> M src/layout/tree.rs</div><div class="line"><span class="prompt">λ</span> <span class="path">~/systems/compositor</span> <span class="command">cargo test layout::</span></div><div class="line">running 14 tests</div><div class="line success">test result: ok. 14 passed; 0 failed</div><div class="line"> </div><div class="line"><span class="prompt">λ</span> <span class="path">~/systems/compositor</span> <span class="cursor"></span></div></div>` },
  log: { name: "system log", meta: "2 EVENTS", kind: "terminal", content: `<div class="terminal"><div class="line comment">09:38:12 compositor[824] surface attached p2/t1</div><div class="line comment">09:38:18 input[112] pointer focus → p1/t2</div><div class="line">09:39:04 layout[824] ratio committed 0.517</div><div class="line warn">09:39:20 renderer[824] frame budget 17.2ms</div></div>` },
  proc: { name: "processes", meta: "18 ACTIVE", kind: "terminal", content: `<div class="terminal"><div class="line"> PID  CPU   MEM  COMMAND</div><div class="line"> 824  3.2%  88M  compositor</div><div class="line"> 112  0.4%  12M  inputd</div><div class="line"> 948  0.2%  31M  shell</div></div>` },
  architecture: { name: "architecture.md", meta: "64 LINES", kind: "editor", content: `<div class="editor"><div class="doc-path">architecture.md</div><h1>A workspace is a tree, not a pile.</h1><p>Every branch divides space. Every leaf is a pane, and every pane owns an ordered stack of content.</p></div>` },
  compositor: { name: "compositor notes", meta: "SAVED", kind: "editor", content: `<div class="editor"><div class="doc-path">notes / compositor</div><h1>Hard geometry, quiet surfaces.</h1><p>The divider network is the workspace chassis. Its intersections are small joints; its lines remain visible without demanding attention.</p><h2>Visual hierarchy</h2><ul><li>Pane focus is a structural state, not decoration.</li><li>Expanded content becomes fractionally brighter.</li><li>Amber means action, focus, or attention—nothing else.</li></ul><p>Tile rails stay compact so the visible stack reads as a navigable history rather than a set of tabs.</p></div>` },
  tasks: { name: "tasks", meta: "3 OPEN", kind: "editor", content: `<div class="editor"><div class="doc-path">tasks</div><h1>Next pass</h1><ul><li>Evaluate header density at 1280 × 720.</li><li>Test keyboard-only pane navigation.</li><li>Compare amber intensity on OLED panels.</li></ul></div>` }
};

const initialLayout = () => ({
  type: "split", id: "root", direction: "vertical", ratio: 0.515,
  first: { type: "pane", id: "p1", expanded: "renderer", tiles: ["notes", "renderer", "build", "refs"] },
  second: { type: "split", id: "right", direction: "horizontal", ratio: 0.49,
    first: { type: "pane", id: "p2", expanded: "shell", tiles: ["shell", "log", "proc"] },
    second: { type: "pane", id: "p3", expanded: "compositor", tiles: ["architecture", "compositor", "tasks"] }
  }
});

let layout = initialLayout();
let focusedPaneId = "p1";
let paneSequence = 4;
let activeDragCancel = null;
const workspace = document.querySelector("#workspace");
const statusText = document.querySelector("#status-text");
const themeMenu = document.querySelector("#theme-menu");
const themeToggle = document.querySelector("#theme-toggle");
const themeNames = { signal: "Signal Amber", carbon: "Carbon Optics", abyssal: "Abyssal Sonar", oxide: "Oxidized Archive", combine: "Combine Relay", deusex: "Deus Ex Access", shock: "System Shock Node", sin: "SiN Network", mesa: "Black Mesa Lab", strogg: "Strogg Process", genera: "Genera Ivory", mineral: "Mineral Sage", logic: "Warm Logic" };

function findPane(node, id) {
  if (node.type === "pane") return node.id === id ? node : null;
  return findPane(node.first, id) || findPane(node.second, id);
}

function listPanes(node, out = []) {
  if (node.type === "pane") out.push(node);
  else { listPanes(node.first, out); listPanes(node.second, out); }
  return out;
}

function replaceNode(node, id, replacement) {
  if (node.id === id) return replacement;
  if (node.type === "split") {
    node.first = replaceNode(node.first, id, replacement);
    node.second = replaceNode(node.second, id, replacement);
  }
  return node;
}

function tileMarkup(tileId, index, expanded, paneId) {
  const tile = tileCatalog[tileId];
  const classes = ["tile", expanded ? "expanded" : "", tile.state || ""].filter(Boolean).join(" ");
  return `<article class="${classes}" data-tile-id="${tileId}">
    <button class="tile-header" data-open-tile="${tileId}" data-pane-id="${paneId}" aria-expanded="${expanded}">
      <span class="tile-index">${String(index + 1).padStart(2, "0")}</span>
      <span class="tile-name">${tile.name}</span>
      <span class="tile-meta">${tile.meta}</span>
      <span class="tile-action" data-close-tile="${tileId}" title="Close tile" aria-label="Close ${tile.name}">×</span>
    </button>
    <div class="tile-body">${tile.content}</div>
  </article>`;
}

function renderNode(node) {
  if (node.type === "pane") {
    const el = document.createElement("section");
    el.className = `pane${node.id === focusedPaneId ? " focused" : ""}`;
    el.dataset.paneId = node.id;
    el.tabIndex = 0;
    el.setAttribute("aria-label", `Pane ${node.id}`);
    el.innerHTML = node.tiles.map((id, i) => tileMarkup(id, i, id === node.expanded, node.id)).join("");
    return el;
  }
  const split = document.createElement("div");
  split.className = `split ${node.direction}`;
  split.dataset.splitId = node.id;
  const firstWrap = document.createElement("div");
  firstWrap.className = "split-child";
  firstWrap.style.flexBasis = `${node.ratio * 100}%`;
  firstWrap.appendChild(renderNode(node.first));
  const divider = document.createElement("div");
  divider.className = "divider";
  divider.dataset.splitId = node.id;
  divider.setAttribute("role", "separator");
  divider.setAttribute("aria-orientation", node.direction === "vertical" ? "vertical" : "horizontal");
  divider.tabIndex = 0;
  const secondWrap = document.createElement("div");
  secondWrap.className = "split-child";
  secondWrap.style.flexBasis = `${(1 - node.ratio) * 100}%`;
  secondWrap.appendChild(renderNode(node.second));
  split.append(firstWrap, divider, secondWrap);
  wireDivider(divider, node, split, firstWrap, secondWrap);
  return split;
}

function render() {
  workspace.replaceChildren(renderNode(layout));
  document.querySelector("#pane-count").textContent = `${listPanes(layout).length} PANES`;
  const pane = findPane(layout, focusedPaneId) || listPanes(layout)[0];
  if (pane) {
    focusedPaneId = pane.id;
    document.querySelector("#focus-path").textContent = tileCatalog[pane.expanded].name;
  }
}

function setStatus(message, tone = "normal") {
  statusText.textContent = message.toUpperCase();
  statusText.style.color = tone === "error" ? "var(--error)" : tone === "active" ? "var(--amber)" : "";
  clearTimeout(setStatus.timer);
  setStatus.timer = setTimeout(() => { statusText.textContent = "READY"; statusText.style.color = ""; }, 1800);
}

workspace.addEventListener("click", event => {
  const close = event.target.closest("[data-close-tile]");
  if (close) {
    event.stopPropagation();
    const header = close.closest(".tile-header");
    closeTile(header.dataset.paneId, close.dataset.closeTile);
    return;
  }
  const header = event.target.closest("[data-open-tile]");
  if (header) {
    const pane = findPane(layout, header.dataset.paneId);
    focusedPaneId = pane.id;
    if (pane.expanded !== header.dataset.openTile) {
      pane.expanded = header.dataset.openTile;
      setStatus(`Opened ${tileCatalog[pane.expanded].name}`, "active");
    }
    render();
    return;
  }
  const paneEl = event.target.closest(".pane");
  if (paneEl && paneEl.dataset.paneId !== focusedPaneId) { focusedPaneId = paneEl.dataset.paneId; render(); }
});

function closeTile(paneId, tileId) {
  const pane = findPane(layout, paneId);
  if (pane.tiles.length === 1) { setStatus("Final tile is protected", "error"); return; }
  const index = pane.tiles.indexOf(tileId);
  pane.tiles.splice(index, 1);
  if (pane.expanded === tileId) pane.expanded = pane.tiles[Math.min(index, pane.tiles.length - 1)];
  setStatus(`Closed ${tileCatalog[tileId].name}`);
  render();
}

function findSplit(node, id) {
  if (node.type === "split" && node.id === id) return node;
  if (node.type === "split") return findSplit(node.first, id) || findSplit(node.second, id);
  return null;
}

function wireDivider(divider, node, splitEl, first, second) {
  let active = false;
  const move = event => {
    if (!active) return;
    const rect = splitEl.getBoundingClientRect();
    const position = node.direction === "vertical" ? event.clientX - rect.left : event.clientY - rect.top;
    const extent = node.direction === "vertical" ? rect.width : rect.height;
    node.ratio = Math.min(.78, Math.max(.22, position / extent));
    first.style.flexBasis = `${node.ratio * 100}%`;
    second.style.flexBasis = `${(1 - node.ratio) * 100}%`;
    setStatus(`Ratio ${Math.round(node.ratio * 100)} / ${Math.round((1-node.ratio)*100)}`, "active");
  };
  const end = () => {
    if (!active) return;
    active = false;
    divider.classList.remove("dragging");
    divider.releasePointerCapture?.(divider.pointerId);
    if (activeDragCancel === end) activeDragCancel = null;
  };
  divider.addEventListener("pointerdown", event => {
    active = true; activeDragCancel = end; divider.pointerId = event.pointerId; divider.setPointerCapture(event.pointerId); divider.classList.add("dragging");
  });
  divider.addEventListener("pointermove", move);
  divider.addEventListener("pointerup", end);
  divider.addEventListener("pointercancel", end);
  divider.addEventListener("dblclick", () => { node.ratio = .5; first.style.flexBasis = "50%"; second.style.flexBasis = "50%"; setStatus("Ratio reset"); });
  divider.addEventListener("keydown", event => {
    const delta = (event.key === "ArrowLeft" || event.key === "ArrowUp") ? -.025 : (event.key === "ArrowRight" || event.key === "ArrowDown") ? .025 : 0;
    if (!delta) return;
    event.preventDefault(); node.ratio = Math.min(.78, Math.max(.22, node.ratio + delta)); render();
  });
}

function splitFocused(direction) {
  const pane = findPane(layout, focusedPaneId);
  if (!pane) return;
  const newId = `p${paneSequence++}`;
  const cloneTile = pane.expanded;
  const newPane = { type: "pane", id: newId, expanded: cloneTile, tiles: [cloneTile] };
  const replacement = { type: "split", id: `s${Date.now()}`, direction, ratio: .5, first: pane, second: newPane };
  layout = replaceNode(layout, pane.id, replacement);
  focusedPaneId = newId;
  render();
  document.querySelector(`[data-pane-id="${newId}"]`)?.insertAdjacentHTML("afterbegin", '<span class="split-flash"></span>');
  setStatus(`Split ${direction}`, "active");
}

function cycleTile(delta) {
  const pane = findPane(layout, focusedPaneId);
  if (!pane) return;
  const current = pane.tiles.indexOf(pane.expanded);
  pane.expanded = pane.tiles[(current + delta + pane.tiles.length) % pane.tiles.length];
  setStatus(`Opened ${tileCatalog[pane.expanded].name}`, "active");
  render();
}

function focusNeighbor(key) {
  const panes = [...document.querySelectorAll(".pane")];
  const current = panes.find(p => p.dataset.paneId === focusedPaneId);
  if (!current) return;
  const r = current.getBoundingClientRect();
  const center = { x: r.left + r.width/2, y: r.top + r.height/2 };
  const candidates = panes.filter(p => p !== current).map(p => {
    const b = p.getBoundingClientRect(); const c = { x: b.left+b.width/2, y: b.top+b.height/2 };
    return { id:p.dataset.paneId, dx:c.x-center.x, dy:c.y-center.y, distance:Math.hypot(c.x-center.x,c.y-center.y) };
  }).filter(c => key === "ArrowLeft" ? c.dx < -5 : key === "ArrowRight" ? c.dx > 5 : key === "ArrowUp" ? c.dy < -5 : c.dy > 5)
    .sort((a,b) => a.distance-b.distance);
  if (candidates[0]) { focusedPaneId = candidates[0].id; render(); setStatus(`Focus ${focusedPaneId}`, "active"); }
}

document.querySelector("#split-h").addEventListener("click", () => splitFocused("horizontal"));
document.querySelector("#split-v").addEventListener("click", () => splitFocused("vertical"));
document.querySelector("#reset-layout").addEventListener("click", () => { layout = initialLayout(); focusedPaneId = "p1"; paneSequence = 4; render(); setStatus("Workspace reset"); });
document.querySelector("#open-help").addEventListener("click", () => document.querySelector("#help-dialog").showModal());

function closeThemeMenu() {
  themeMenu.hidden = true;
  themeToggle.setAttribute("aria-expanded", "false");
}

function applyTheme(theme, persist = true) {
  if (!themeNames[theme]) theme = "signal";
  document.documentElement.dataset.theme = theme;
  document.querySelector("#current-theme").textContent = themeNames[theme];
  document.querySelectorAll(".theme-option").forEach(option => option.setAttribute("aria-checked", String(option.dataset.theme === theme)));
  if (persist) { try { localStorage.setItem("instrument-theme", theme); } catch {} }
}

themeToggle.addEventListener("click", event => {
  event.stopPropagation();
  const opening = themeMenu.hidden;
  themeMenu.hidden = !opening;
  themeToggle.setAttribute("aria-expanded", String(opening));
  if (opening) document.querySelector(`.theme-option[data-theme="${document.documentElement.dataset.theme}"]`)?.focus();
});

themeMenu.addEventListener("click", event => {
  const option = event.target.closest(".theme-option");
  if (!option) return;
  applyTheme(option.dataset.theme);
  closeThemeMenu();
  themeToggle.focus();
  setStatus(`Theme · ${themeNames[option.dataset.theme]}`, "active");
});

themeMenu.addEventListener("keydown", event => {
  const options = [...themeMenu.querySelectorAll(".theme-option")];
  const current = options.indexOf(document.activeElement);
  let next = null;
  if (event.key === "ArrowDown") next = options[(current + 1 + options.length) % options.length];
  else if (event.key === "ArrowUp") next = options[(current - 1 + options.length) % options.length];
  else if (event.key === "Home") next = options[0];
  else if (event.key === "End") next = options.at(-1);
  if (next) { event.preventDefault(); next.focus(); }
});

document.addEventListener("click", event => { if (!event.target.closest(".theme-control")) closeThemeMenu(); });

document.addEventListener("keydown", event => {
  if (event.key === "Escape") {
    if (!themeMenu.hidden) { closeThemeMenu(); themeToggle.focus(); return; }
    activeDragCancel?.(); setStatus("Resize cancelled"); return;
  }
  if (!event.altKey) return;
  const key = event.key.toLowerCase();
  if (["arrowleft","arrowright","arrowup","arrowdown"].includes(key)) { event.preventDefault(); focusNeighbor(event.key); }
  else if (key === "j") { event.preventDefault(); cycleTile(1); }
  else if (key === "k") { event.preventDefault(); cycleTile(-1); }
  else if (key === "h") { event.preventDefault(); splitFocused("horizontal"); }
  else if (key === "v") { event.preventDefault(); splitFocused("vertical"); }
});

function registerModelTools() {
  const context = document.modelContext;
  if (!context?.registerTool) return;
  const register = tool => { try { Promise.resolve(context.registerTool(tool)).catch(() => {}); } catch {} };
  register({ name:"open_tile", title:"Open tile", description:"Expand one named tile in a pane and collapse the previously open tile.", inputSchema:{type:"object",properties:{paneId:{type:"string"},tileId:{type:"string"}},required:["paneId","tileId"],additionalProperties:false}, annotations:{readOnlyHint:false,untrustedContentHint:false}, execute(input){ const pane=findPane(layout,input?.paneId); if(!pane||!pane.tiles.includes(input?.tileId)) throw new Error("Unknown pane or tile"); pane.expanded=input.tileId; focusedPaneId=pane.id; render(); return {paneId:pane.id,expandedTile:pane.expanded}; } });
  register({ name:"split_pane", title:"Split pane", description:"Split the focused pane horizontally or vertically.", inputSchema:{type:"object",properties:{direction:{type:"string",enum:["horizontal","vertical"]}},required:["direction"],additionalProperties:false}, annotations:{readOnlyHint:false,untrustedContentHint:false}, execute(input){ if(!["horizontal","vertical"].includes(input?.direction)) throw new Error("Invalid direction"); splitFocused(input.direction); return {focusedPaneId,paneCount:listPanes(layout).length,direction:input.direction}; } });
  register({ name:"read_workspace", title:"Read workspace", description:"Return the current pane, tile, and focus state without changing it.", inputSchema:{type:"object",properties:{},additionalProperties:false}, annotations:{readOnlyHint:true,untrustedContentHint:false}, execute(){ return {focusedPaneId,panes:listPanes(layout).map(p=>({id:p.id,expandedTile:p.expanded,tiles:[...p.tiles]}))}; } });
}

function updateClock() {
  const now = new Date(); const value = now.toLocaleTimeString([], {hour:"2-digit", minute:"2-digit", hour12:false});
  const clock = document.querySelector("#clock"); clock.textContent = value; clock.dateTime = value;
}

applyTheme(document.documentElement.dataset.theme || "signal", false);
render(); registerModelTools(); updateClock(); setInterval(updateClock, 30000);
