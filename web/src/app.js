import { h, render } from 'preact';
import { useState, useEffect, useRef, useLayoutEffect } from 'preact/hooks';
import htm from 'htm';

const html = htm.bind(h);

/* -------------------------------------------------------------------------- */
/*  Config                                                                    */
/* -------------------------------------------------------------------------- */
const MAX_PTS = 20;         // sparkline history length
const POLL_MS = 5000;       // dashboard refresh cadence
const HEAP_TOTAL_KB = 8192; // ESP32-S3 reference for the memory bar

/* Shared drag flag: while a slider is being dragged we pause polling so the
 * thumb doesn't jump when fresh data arrives (matches the original behaviour). */
let dragging = false;

/* -------------------------------------------------------------------------- */
/*  API (identical contract to the firmware)                                  */
/* -------------------------------------------------------------------------- */
const post = (url) => (window.PS_MOCK ? Promise.resolve({ ok: true }) : fetch(url, { method: 'POST' }));
const api = {
  nodes: () => fetch('/api/nodes').then((r) => r.json()),
  mute: (id) => post('/api/node/mute?id=' + id),
  unmute: (id) => post('/api/node/unmute?id=' + id),
  volume: (id, level) => post('/api/node/volume?id=' + id + '&level=' + level),
  reset: (id) => post('/api/node/unlock?id=' + id),
  reboot: (id) => post('/api/node/reboot?id=' + id),
  globalMute: () => post('/api/global/mute'),
  globalUnmute: () => post('/api/global/unmute'),
};

/* Preview/demo data source (used only when the page sets window.PS_MOCK). */
function mockNodes() {
  if (!window.__mock) {
    window.__mock = [1, 2, 3].map((id) => ({
      node_id: id,
      mac: '84:F7:03:A1:0' + id + ':7C',
      masking_active: id !== 2,
      volume: [65, 0, 40][id - 1],
      battery_pct: [92, 47, 78][id - 1],
      delivery_ratio: 0.9,
      packet_loss_rate: [0.03, 0.08, 0.02][id - 1],
      cpu0: 20, cpu1: 15,
      heap_free: 4600000, heap_largest_block: 3200000,
      uptime_s: id * 5400,
    }));
  }
  return window.__mock.map((n) => {
    const loss = Math.max(0, Math.min(0.18, n.packet_loss_rate + (Math.random() * 0.05 - 0.025)));
    return {
      ...n,
      cpu0: Math.max(3, Math.min(95, n.cpu0 + (Math.random() * 24 - 12))) | 0,
      cpu1: Math.max(3, Math.min(95, n.cpu1 + (Math.random() * 20 - 10))) | 0,
      packet_loss_rate: loss,
      delivery_ratio: 1 - loss,
      masking_active: n.masking_active,
      volume: n.volume,
    };
  });
}
const fetchNodes = () => (window.PS_MOCK ? Promise.resolve(mockNodes()) : api.nodes());

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */
function fmtUptime(s) {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return d + 'd ' + h + 'h';
  if (h > 0) return h + 'h ' + m + 'm';
  return m + 'm';
}
const kb = (bytes) => Math.round(bytes / 1024);

/* Chart colours chosen to read on both dark and light backgrounds. */
const C_CPU0 = '#3b82f6';
const C_CPU1 = '#8b5cf6';
const C_LOSS = '#ef4444';
const C_DELIVERY = '#10b981';

/* -------------------------------------------------------------------------- */
/*  Theme                                                                      */
/* -------------------------------------------------------------------------- */
function initialTheme() {
  try {
    const s = localStorage.getItem('ps-theme');
    if (s === 'light' || s === 'dark') return s;
  } catch (e) {}
  return window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches
    ? 'light' : 'dark';
}
const SunIcon = html`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>`;
const MoonIcon = html`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>`;
const ChevronIcon = html`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 9l6 6 6-6"/></svg>`;

/* -------------------------------------------------------------------------- */
/*  Sparkline — smooth area + line, gradient stroke                           */
/* -------------------------------------------------------------------------- */
function Sparkline({ series, max, fmt = (v) => Math.round(v), unit = '%', w = 260, hgt = 46 }) {
  const [hi, setHi] = useState(-1);
  const step = w / (MAX_PTS - 1);
  const yOf = (v) => hgt - Math.min(v, max) / max * (hgt - 4) - 2;
  const len = series.reduce((m, s) => Math.max(m, (s.data || []).length), 0);

  const onMove = (e) => {
    const r = e.currentTarget.getBoundingClientRect();
    if (!r.width || len < 1) return;
    const frac = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
    let i = Math.round(frac * (MAX_PTS - 1));
    if (i > len - 1) i = len - 1;
    if (i < 0) i = 0;
    setHi(i);
  };
  const leftPct = hi >= 0 ? (hi / (MAX_PTS - 1)) * 100 : 0;

  return html`
    <div class="spark-wrap" style=${'height:' + hgt + 'px'}>
      <svg class="spark" style=${'height:' + hgt + 'px'} viewBox=${'0 0 ' + w + ' ' + hgt} preserveAspectRatio="none"
           onMouseMove=${onMove} onMouseLeave=${() => setHi(-1)}>
        ${series.map((s, si) => {
          const arr = s.data;
          if (!arr || arr.length < 2) return null;
          const pts = arr.map((v, i) => [+(i * step).toFixed(1), +yOf(v).toFixed(1)]);
          const line = pts.map((p, i) => (i ? 'L' : 'M') + p[0] + ',' + p[1]).join(' ');
          const area = line + ' L' + pts[pts.length - 1][0] + ',' + hgt + ' L' + pts[0][0] + ',' + hgt + ' Z';
          const gid = 'g' + si + '_' + s.color.replace('#', '');
          return html`
            <defs>
              <linearGradient id=${gid} x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stop-color=${s.color} stop-opacity="0.35" />
                <stop offset="100%" stop-color=${s.color} stop-opacity="0" />
              </linearGradient>
            </defs>
            <path d=${area} fill=${'url(#' + gid + ')'} />
            <path d=${line} fill="none" stroke=${s.color} stroke-width="2"
                  stroke-linejoin="round" stroke-linecap="round" />`;
        })}
      </svg>
      ${hi >= 0 && html`<div class="spark-guide" style=${'left:' + leftPct + '%'}></div>`}
      ${hi >= 0 && series.map((s) =>
        s.data && s.data[hi] != null
          ? html`<div class="spark-dot" style=${'left:' + leftPct + '%;top:' +
              (yOf(s.data[hi]) / hgt * 100) + '%;background:' + s.color}></div>`
          : null)}
      ${hi >= 0 && html`<div class="spark-tip" style=${'left:' + leftPct + '%'}>
        ${series.map((s) =>
          s.data && s.data[hi] != null
            ? html`<span style=${'color:' + s.color}>${(s.label ? s.label + ' ' : '') + fmt(s.data[hi]) + unit}</span>`
            : null)}
      </div>`}
    </div>`;
}

/* -------------------------------------------------------------------------- */
/*  Small building blocks                                                      */
/* -------------------------------------------------------------------------- */
const Metric = ({ label, value, sub }) => html`
  <div class="metric">
    <span class="metric-label">${label}</span>
    <span class="metric-value">${value}${sub && html`<span class="metric-sub">${sub}</span>`}</span>
  </div>`;

function Bar({ pct, tone }) {
  const p = Math.max(0, Math.min(100, pct));
  return html`<div class="bar"><div class=${'bar-fill tone-' + tone} style=${'width:' + p + '%'}></div></div>`;
}

function StatTile({ label, value, accent }) {
  return html`
    <div class="tile">
      <div class=${'tile-value' + (accent ? ' accent' : '')}>${value}</div>
      <div class="tile-label">${label}</div>
    </div>`;
}

/* -------------------------------------------------------------------------- */
/*  Node card                                                                  */
/* -------------------------------------------------------------------------- */
function NodeCard({ node, hist, expanded, onToggle, onMute, onUnmute, onVolume, onReset, onReboot }) {
  const [val, setVal] = useState(node.volume);
  const draggingLocal = useRef(false);

  useEffect(() => {
    if (!draggingLocal.current) setVal(node.volume);
  }, [node.volume]);

  const battTone = node.battery_pct > 50 ? 'good' : node.battery_pct > 20 ? 'warn' : 'bad';
  const heapPct = (kb(node.heap_free) / HEAP_TOTAL_KB) * 100;
  const lossPct = node.packet_loss_rate * 100; // firmware sends a 0..1 ratio
  const volFill = 'background:linear-gradient(90deg,var(--accent) ' + val + '%,var(--line) ' + val + '%)';
  const stop = (e) => e.stopPropagation(); // keep control clicks from toggling expand

  // Packet-loss chart can switch to delivery ratio (per card).
  const [lossView, setLossView] = useState('loss');
  const isLoss = lossView === 'loss';
  const deliveryPct = node.delivery_ratio * 100;
  const lossData = isLoss ? hist.loss : hist.delivery;
  const lossColor = isLoss ? C_LOSS : C_DELIVERY;
  const lossVal = isLoss ? lossPct : deliveryPct;
  const lossChart = (hgt) => html`
    <div class="chart">
      <div class="chart-head">
        <div class="seg" onClick=${stop}>
          <button class=${'seg-btn' + (isLoss ? ' on' : '')} onClick=${() => setLossView('loss')}>Loss</button>
          <button class=${'seg-btn' + (!isLoss ? ' on' : '')} onClick=${() => setLossView('delivery')}>Delivery</button>
        </div>
        <span class="legend"><b style=${'color:' + lossColor}>${lossVal.toFixed(1)}%</b></span>
      </div>
      ${Sparkline({ max: 100, hgt: hgt, fmt: (v) => v.toFixed(1), series: [{ data: lossData, color: lossColor }] })}
    </div>`;

  const head = html`
    <div class="card-head">
      <div class="card-title"><span class="node-dot"></span>Node ${node.node_id}</div>
      <div class="head-right">
        <span class=${'pill ' + (node.masking_active ? 'pill-on' : 'pill-off')}>
          <span class="pill-dot"></span>${node.masking_active ? 'Masking' : 'Silent'}
        </span>
        <span class=${'chev' + (expanded ? ' open' : '')}>${ChevronIcon}</span>
      </div>
    </div>
    <div class="mac">${node.mac}</div>`;

  const metricsBlock = html`
    <div class="metrics">
      ${Metric({ label: 'Volume', value: node.volume + '%' })}
      ${Metric({ label: 'Battery', value: node.battery_pct + '%' })}
      ${Metric({ label: 'Uptime', value: fmtUptime(node.uptime_s) })}
      ${Metric({ label: 'Delivery', value: Math.round(node.delivery_ratio * 100) + '%' })}
    </div>`;

  const barsBlock = html`
    <div class="submetric">
      <div class="submetric-head"><span>Battery</span><span>${node.battery_pct}%</span></div>
      ${Bar({ pct: node.battery_pct, tone: battTone })}
    </div>
    <div class="submetric">
      <div class="submetric-head"><span>Free memory</span><span>${kb(node.heap_free)} KB</span></div>
      ${Bar({ pct: heapPct, tone: 'mem' })}
    </div>`;

  const sliderRow = html`
    <div class="slider-row" onClick=${stop}>
      <span class="slider-cap">Vol</span>
      <input type="range" min="0" max="100" value=${val} class="slider" style=${volFill}
        onInput=${(e) => { dragging = true; draggingLocal.current = true; setVal(+e.target.value); }}
        onChange=${(e) => {
          const v = +e.target.value;
          setVal(v);
          onVolume(node.node_id, v);
          setTimeout(() => { dragging = false; draggingLocal.current = false; }, 1000);
        }} />
      <span class="slider-val">${val}%</span>
    </div>`;

  const muteRow = html`
    <div class="actions" onClick=${stop}>
      <button class="btn btn-mute" onClick=${() => onMute(node.node_id)}>Mute</button>
      <button class="btn btn-unmute" onClick=${() => onUnmute(node.node_id)}>Unmute</button>
    </div>`;

  const resetRow = html`
    <div class="actions" onClick=${stop}>
      <button class="btn btn-reset" onClick=${() => onReset(node.node_id)}>Reset</button>
      <button class="btn btn-reboot"
        onClick=${() => { if (window.confirm('Reboot node ' + node.node_id + '?')) onReboot(node.node_id); }}>Reboot</button>
    </div>`;

  const chartsCompact = html`
    <div class="chart">
      <div class="chart-head">
        <span>CPU load</span>
        <span class="legend">
          <b style=${'color:' + C_CPU0}>C0 ${node.cpu0}%</b>
          <b style=${'color:' + C_CPU1}>C1 ${node.cpu1}%</b>
        </span>
      </div>
      ${Sparkline({ max: 100, series: [
        { data: hist.cpu0, color: C_CPU0, label: 'C0' },
        { data: hist.cpu1, color: C_CPU1, label: 'C1' },
      ] })}
    </div>
    ${lossChart(46)}`;

  const chartsExpanded = html`
    <div class="chart">
      <div class="chart-head"><span>CPU Core 0</span><span class="legend"><b style=${'color:' + C_CPU0}>${node.cpu0}%</b></span></div>
      ${Sparkline({ max: 100, hgt: 104, series: [{ data: hist.cpu0, color: C_CPU0 }] })}
    </div>
    <div class="chart">
      <div class="chart-head"><span>CPU Core 1</span><span class="legend"><b style=${'color:' + C_CPU1}>${node.cpu1}%</b></span></div>
      ${Sparkline({ max: 100, hgt: 104, series: [{ data: hist.cpu1, color: C_CPU1 }] })}
    </div>
    ${lossChart(104)}`;

  return html`
    <div class=${'card' + (expanded ? ' expanded' : '')} data-id=${node.node_id} onClick=${onToggle}>
      ${head}
      ${expanded
        ? html`<div class="expanded-body">
            <div class="col-left">${metricsBlock}${barsBlock}${sliderRow}${muteRow}${resetRow}</div>
            <div class="col-right">${chartsExpanded}</div>
          </div>`
        : html`${metricsBlock}${barsBlock}${chartsCompact}${sliderRow}${muteRow}${resetRow}`}
    </div>`;
}

/* -------------------------------------------------------------------------- */
/*  App                                                                        */
/* -------------------------------------------------------------------------- */
function App() {
  const [nodes, setNodes] = useState([]);
  const [error, setError] = useState(null);
  const [loaded, setLoaded] = useState(false);
  const [theme, setTheme] = useState(initialTheme);
  const [expandedId, setExpandedId] = useState(null);
  const histRef = useRef({});
  const gridRef = useRef(null);
  const firstRects = useRef(null);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    try { localStorage.setItem('ps-theme', theme); } catch (e) {}
  }, [theme]);

  /* No expanded view on mobile — collapse if the viewport shrinks. */
  const mqMobile = () => window.matchMedia('(max-width:560px)').matches;
  const [isMobile, setIsMobile] = useState(mqMobile);
  useEffect(() => {
    const mq = window.matchMedia('(max-width:560px)');
    const on = () => { setIsMobile(mq.matches); if (mq.matches) setExpandedId(null); };
    mq.addEventListener('change', on);
    return () => mq.removeEventListener('change', on);
  }, []);

  /* Toggle expand, capturing card positions first for the FLIP animation. */
  const toggle = (id) => {
    if (isMobile) return; // mobile has no expanded form
    const m = {};
    if (gridRef.current) {
      gridRef.current.querySelectorAll('.card').forEach((el) => {
        m[el.dataset.id] = el.getBoundingClientRect();
      });
    }
    firstRects.current = m;
    setExpandedId((cur) => (cur === id ? null : id));
  };

  /* FLIP: after the layout changes, glide every card from its old position. */
  useLayoutEffect(() => {
    const first = firstRects.current;
    firstRects.current = null;
    const g = gridRef.current;
    if (!first || !g) return;
    const moved = [];
    g.querySelectorAll('.card').forEach((el) => {
      const f = first[el.dataset.id];
      if (!f) return;
      const last = el.getBoundingClientRect();
      const dx = f.left - last.left;
      const dy = f.top - last.top;
      if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return;
      el.style.transition = 'none';
      el.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
      moved.push(el);
    });
    if (!moved.length) return;
    void g.offsetWidth; // force reflow so the starting transform is committed
    requestAnimationFrame(() => {
      moved.forEach((el) => {
        el.style.transition = 'transform .44s cubic-bezier(.2,.7,.2,1)';
        el.style.transform = '';
      });
    });
  }, [expandedId]);

  const refresh = () => {
    if (dragging) return;
    fetchNodes()
      .then((data) => {
        const list = Array.isArray(data) ? data : [];
        const h = histRef.current;
        const ids = [];
        list.forEach((n) => {
          ids.push(n.node_id);
          const e = h[n.node_id] || (h[n.node_id] = { cpu0: [], cpu1: [], loss: [], delivery: [] });
          e.cpu0.push(n.cpu0); if (e.cpu0.length > MAX_PTS) e.cpu0.shift();
          e.cpu1.push(n.cpu1); if (e.cpu1.length > MAX_PTS) e.cpu1.shift();
          e.loss.push(n.packet_loss_rate * 100); if (e.loss.length > MAX_PTS) e.loss.shift();
          e.delivery.push(n.delivery_ratio * 100); if (e.delivery.length > MAX_PTS) e.delivery.shift();
        });
        Object.keys(h).forEach((k) => { if (ids.indexOf(+k) === -1) delete h[k]; });
        setNodes(list);
        setError(null);
        setLoaded(true);
      })
      .catch((e) => { setError(e.message || String(e)); setLoaded(true); });
  };

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, POLL_MS);
    return () => clearInterval(t);
  }, []);

  const act = (fn) => fn().then(refresh).catch((e) => setError(e.message || String(e)));
  const maskingCount = nodes.filter((n) => n.masking_active).length;
  const avgBatt = nodes.length ? Math.round(nodes.reduce((a, n) => a + n.battery_pct, 0) / nodes.length) : 0;

  return html`
    <div class="app">
      <header class="topbar">
        <div class="brand">
          <div class="logo">PS</div>
          <div>
            <div class="brand-title">Privacy Shield</div>
            <div class="brand-sub">Hub dashboard</div>
          </div>
        </div>
        <div class="topbar-right">
          <div class=${'conn ' + (error ? 'conn-bad' : 'conn-ok')}>
            <span class="conn-dot"></span>${error ? 'Disconnected' : 'Live'}
          </div>
          <button class="theme-btn" title="Toggle light / dark" aria-label="Toggle light / dark"
            onClick=${() => setTheme((t) => (t === 'light' ? 'dark' : 'light'))}>
            ${theme === 'light' ? MoonIcon : SunIcon}
          </button>
        </div>
      </header>

      <main class="wrap">
        <section class="tiles">
          ${StatTile({ label: 'Nodes online', value: nodes.length, accent: true })}
          ${StatTile({ label: 'Masking', value: maskingCount })}
          ${StatTile({ label: 'Avg battery', value: (nodes.length ? avgBatt + '%' : '—') })}
        </section>

        <section class="toolbar">
          <button class="btn btn-danger" onClick=${() => act(api.globalMute)}>Mute all</button>
          <button class="btn btn-success" onClick=${() => act(api.globalUnmute)}>Unmute all</button>
          <button class="btn btn-ghost" onClick=${refresh}>Refresh</button>
          <span class="toolbar-spacer"></span>
          <span class="refresh-note">Auto-refresh · ${POLL_MS / 1000}s</span>
        </section>

        ${error && html`<div class="banner">Can't reach the hub — ${error}</div>`}

        ${!loaded
          ? html`<div class="empty">Loading…</div>`
          : nodes.length === 0
          ? html`<div class="empty">
              <div class="empty-title">No masking nodes detected</div>
              <div class="empty-sub">Power on a node to get started.</div>
            </div>`
          : html`<section class="grid" ref=${gridRef}>
              ${nodes.map((n) => html`<${NodeCard}
                key=${n.node_id}
                node=${n}
                hist=${histRef.current[n.node_id] || { cpu0: [], cpu1: [], loss: [], delivery: [] }}
                expanded=${expandedId === n.node_id}
                onToggle=${() => toggle(n.node_id)}
                onMute=${(id) => act(() => api.mute(id))}
                onUnmute=${(id) => act(() => api.unmute(id))}
                onVolume=${(id, v) => act(() => api.volume(id, v))}
                onReset=${(id) => act(() => api.reset(id))}
                onReboot=${(id) => act(() => api.reboot(id))} />`)}
            </section>`}

        <footer class="foot">Privacy Shield · ESP-NOW mesh</footer>
      </main>
    </div>`;
}

render(html`<${App} />`, document.getElementById('app'));
