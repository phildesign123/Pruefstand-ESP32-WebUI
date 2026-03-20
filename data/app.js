// ============================================================
// Prüfstand ESP32 – Frontend-Logik
// WebSocket 10 Hz + REST-API + Live-Chart (Canvas)
// ============================================================

'use strict';

// ── Zustand ──────────────────────────────────────────────────
const MAX_POINTS = 3000; // 5 min × 10 Hz
let chartData = { ts: [], temp: [], weight: [], speed: [] };
let chartWindowS = 30;
let ws = null;
let wsReconnectTimer = null;
let sequences = [];
let seqState = { state: 'idle', active: -1 };

// ── Navigation ───────────────────────────────────────────────
function showPage(name, el) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.getElementById('page-' + name).classList.add('active');
  document.querySelectorAll('nav a').forEach(a => a.classList.remove('active'));
  if (el) el.classList.add('active');
  if (name === 'settings') refreshSettings();
}

// ── Toast ────────────────────────────────────────────────────
function toast(msg, ms = 2500) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(t._timer);
  t._timer = setTimeout(() => t.classList.remove('show'), ms);
}

// ── API-Helfer ───────────────────────────────────────────────
async function api(method, path, body) {
  try {
    const opts = { method, headers: {} };
    if (body !== undefined) {
      opts.body = JSON.stringify(body);
      opts.headers['Content-Type'] = 'application/json';
    }
    const r = await fetch(path, opts);
    return await r.json();
  } catch (e) {
    toast('API Fehler: ' + e.message);
    return null;
  }
}

// ── WebSocket ────────────────────────────────────────────────
function wsConnect() {
  const url = `ws://${location.hostname}/ws`;
  ws = new WebSocket(url);

  ws.onopen = () => {
    document.getElementById('ws-dot').classList.add('connected');
    clearTimeout(wsReconnectTimer);
  };

  ws.onclose = () => {
    document.getElementById('ws-dot').classList.remove('connected');
    wsReconnectTimer = setTimeout(wsConnect, 2000);
  };

  ws.onerror = () => ws.close();

  ws.onmessage = (evt) => {
    const d = JSON.parse(evt.data);
    updateDashboard(d);
  };
}

function wsSend(obj) {
  if (ws && ws.readyState === WebSocket.OPEN)
    ws.send(JSON.stringify(obj));
}

// ── Dashboard updaten ────────────────────────────────────────
function updateDashboard(d) {
  const now = Date.now() / 1000;

  // Temp
  const diff = Math.abs(d.temp - d.temp_target);
  const cls = diff <= 2 ? 'temp-ok' : diff <= 10 ? 'temp-warn' : 'temp-hot';
  document.getElementById('temp-val').innerHTML =
    `<span class="${cls}">${d.temp.toFixed(1)}</span><span class="unit"> °C</span>`;
  document.getElementById('temp-target').textContent = d.temp_target.toFixed(1);
  document.getElementById('temp-duty').textContent = (d.duty * 100).toFixed(0);

  // Fault
  const faultBox = document.getElementById('fault-box');
  if (d.fault && d.fault > 0) {
    faultBox.style.display = '';
    document.getElementById('fault-text').textContent =
      ['', 'MAX TEMP', 'THERMAL RUNAWAY', 'TEMP JUMP', 'SENSOR FAULT'][d.fault] || 'FAULT';
  } else {
    faultBox.style.display = 'none';
  }

  // Gewicht
  document.getElementById('weight-val').innerHTML =
    `${d.weight.toFixed(2)}<span class="unit"> g</span>`;
  document.getElementById('motor-status').textContent =
    `Motor: ${d.motor ? 'AN' : 'AUS'}  |  ${d.speed.toFixed(2)} mm/s`;

  // Sequencer
  seqState = { state: d.seq_state, active: d.seq };
  updateSeqTable();

  // Chart-Daten
  chartData.ts.push(now);
  chartData.temp.push(d.temp);
  chartData.weight.push(d.weight);
  chartData.speed.push(d.speed);
  // Alte Punkte außerhalb des Fensters entfernen
  const cutoff = now - chartWindowS;
  while (chartData.ts.length > 0 && chartData.ts[0] < cutoff) {
    chartData.ts.shift(); chartData.temp.shift();
    chartData.weight.shift(); chartData.speed.shift();
  }
  if (chartData.ts.length > MAX_POINTS) {
    chartData.ts.shift(); chartData.temp.shift();
    chartData.weight.shift(); chartData.speed.shift();
  }

  drawChart();

  // Settings-Seite live-Update
  if (document.getElementById('s-weight')) {
    document.getElementById('s-weight').textContent = d.weight.toFixed(2);
  }
}

// ── Canvas-Chart ─────────────────────────────────────────────
let chartCanvas = null;

function initChart() {
  const wrap = document.getElementById('chart-wrap');
  chartCanvas = document.createElement('canvas');
  chartCanvas.style.width = '100%';
  chartCanvas.style.height = '200px';
  wrap.appendChild(chartCanvas);
  function resize() {
    chartCanvas.width  = wrap.clientWidth;
    chartCanvas.height = 200;
  }
  resize();
  window.addEventListener('resize', resize);
}

function drawChart() {
  if (!chartCanvas) return;
  const ctx  = chartCanvas.getContext('2d');
  const W    = chartCanvas.width;
  const H    = chartCanvas.height;
  const pad  = { l: 50, r: 50, t: 10, b: 25 };
  const n    = chartData.ts.length;

  ctx.clearRect(0, 0, W, H);
  if (n < 2) return;

  const showTemp   = document.getElementById('show-temp').checked;
  const showWeight = document.getElementById('show-weight').checked;
  const showSpeed  = document.getElementById('show-speed').checked;

  const tMin = chartData.ts[0];
  const tMax = chartData.ts[n - 1];

  // Temp Y-Achse (links)
  const tempMin = 0, tempMax = Math.max(300, ...chartData.temp) * 1.05;
  // Weight/Speed Y-Achse (rechts)
  const wMax = Math.max(1, ...chartData.weight, ...chartData.speed) * 1.1;

  function tx(t) { return pad.l + (t - tMin) / (tMax - tMin) * (W - pad.l - pad.r); }
  function tyL(v, mn, mx) { return pad.t + (1 - (v - mn) / (mx - mn)) * (H - pad.t - pad.b); }

  // Grid
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + i * (H - pad.t - pad.b) / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(W - pad.r, y); ctx.stroke();
  }

  function drawLine(data, color, yFn) {
    ctx.beginPath(); ctx.strokeStyle = color; ctx.lineWidth = 1.5;
    for (let i = 0; i < n; i++) {
      const x = tx(chartData.ts[i]);
      const y = yFn(data[i]);
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
  }

  if (showTemp)   drawLine(chartData.temp,   '#6366f1', v => tyL(v, tempMin, tempMax));
  if (showWeight) drawLine(chartData.weight, '#10b981', v => tyL(v, 0, wMax));
  if (showSpeed)  drawLine(chartData.speed,  '#f59e0b', v => tyL(v, 0, wMax));

  // Achsbeschriftung
  ctx.fillStyle = 'rgba(255,255,255,0.5)';
  ctx.font = '10px Arial';
  ctx.textAlign = 'right';
  [0, 0.25, 0.5, 0.75, 1].forEach(f => {
    const v = tempMin + f * (tempMax - tempMin);
    const y = pad.t + (1 - f) * (H - pad.t - pad.b);
    ctx.fillText(v.toFixed(0) + '°', pad.l - 3, y + 3);
  });
  ctx.textAlign = 'left';
  [0, 0.5, 1].forEach(f => {
    const v = f * wMax;
    const y = pad.t + (1 - f) * (H - pad.t - pad.b);
    ctx.fillText(v.toFixed(0), W - pad.r + 3, y + 3);
  });

  // Zeitachse
  ctx.textAlign = 'center';
  const relMax = tMax - tMin;
  for (let i = 0; i <= 5; i++) {
    const t  = tMin + i * relMax / 5;
    const x  = tx(t);
    const rs = -(tMax - t);
    ctx.fillText(rs.toFixed(0) + 's', x, H - 5);
  }
}

function updateChartWindow() {
  chartWindowS = parseInt(document.getElementById('chart-window').value);
}

function updateSeries() { drawChart(); }

// ── Hotend-Kommandos ──────────────────────────────────────────
async function setTarget() {
  const v = parseFloat(document.getElementById('set-temp-in').value);
  wsSend({ cmd: 'set_target', value: v });
}
async function setTargetVal(v) {
  wsSend({ cmd: 'set_target', value: v });
}
async function clearFault() {
  await api('POST', '/api/hotend/clear_fault');
  toast('Fault quittiert.');
}

// ── Motor-Kommandos ───────────────────────────────────────────
function jog(dist) {
  const speed = parseFloat(document.getElementById('jog-speed').value) || 3;
  wsSend({ cmd: 'motor_jog', dist: Math.abs(dist), speed,
           dir: dist < 0 ? 'rev' : 'fwd' });
}
async function motorStop() {
  wsSend({ cmd: 'motor_stop' });
}

// ── Wägezellen-Kommandos ──────────────────────────────────────
async function tare() {
  wsSend({ cmd: 'tare' });
  toast('Tariert.');
}

// ── Sequencer ────────────────────────────────────────────────
async function loadSequences() {
  const d = await api('GET', '/api/sequence');
  if (!d) return;
  sequences = d.sequences || [];
  seqState  = { state: d.state, active: d.active };
  updateSeqTable();
}

function updateSeqTable() {
  const tbody = document.getElementById('seq-table');
  if (!tbody) return;
  const statusIcons = { idle:'·', heating:'⏳', running:'►', done:'✓', error:'✗', next:'»' };

  tbody.innerHTML = sequences.map((s, i) => {
    const icon = i === seqState.active ? statusIcons[seqState.state] || '►' : '·';
    const cls  = i === seqState.active ? 'active-seq' : '';
    return `<tr class="${cls}">
      <td>${i + 1}</td>
      <td>${s.temp_c}</td>
      <td>${s.speed_mm_s}</td>
      <td>${s.duration_s}</td>
      <td class="seq-status">${icon}</td>
      <td><button class="secondary" style="padding:3px 8px" onclick="seqDelete(${i})">✕</button></td>
    </tr>`;
  }).join('');
}

async function seqAdd() {
  const temp  = parseFloat(document.getElementById('seq-temp').value);
  const speed = parseFloat(document.getElementById('seq-speed').value);
  const dur   = parseFloat(document.getElementById('seq-dur').value);
  if (!temp || !speed || !dur) { toast('Alle Felder ausfüllen!'); return; }
  await api('POST', '/api/sequence/add', { temp_c: temp, speed_mm_s: speed, duration_s: dur });
  await loadSequences();
}

async function seqDelete(i) {
  await api('POST', '/api/sequence/delete', { index: i });
  await loadSequences();
}

async function seqStart() {
  const r = await api('POST', '/api/sequence/start');
  if (r && !r.ok) toast('Fehler: ' + (r.error || 'unbekannt'));
  else { toast('Messreihe gestartet.'); await loadSequences(); }
}

async function seqStop() {
  await api('POST', '/api/sequence/stop');
  toast('Gestoppt.');
}

async function seqClear() {
  await api('POST', '/api/sequence/clear');
  sequences = [];
  updateSeqTable();
}

// ── Einstellungen ─────────────────────────────────────────────
async function refreshSettings() {
  // Wägezelle
  const lc = await api('GET', '/api/loadcell');
  if (lc) {
    document.getElementById('s-raw').textContent = lc.raw;
    const badge = document.getElementById('s-cal-state');
    badge.textContent = lc.calibrated ? 'Kalibriert' : 'Nicht kalibriert';
    badge.className = 'badge ' + (lc.calibrated ? 'ok' : 'warn');
  }

  // E-Steps
  const es = await api('GET', '/api/motor/esteps');
  if (es) {
    document.getElementById('s-esteps').textContent = es.steps_per_mm.toFixed(2);
    const b = document.getElementById('s-esteps-state');
    b.textContent = es.valid ? 'kalibriert' : 'DEFAULT';
    b.className = 'badge ' + (es.valid ? 'ok' : 'warn');
  }

  // SD-Info
  const sd = await api('GET', '/api/datalog/sdinfo');
  if (sd) {
    document.getElementById('sd-free').textContent  = formatBytes(sd.free);
    document.getElementById('sd-total').textContent = formatBytes(sd.total);
  }

  // Dateien laden
  await refreshFiles();

  // Datalog Status
  const ls = await api('GET', '/api/datalog/status');
  if (ls) {
    const b = document.getElementById('log-state');
    b.textContent = ls.state;
    b.className = 'badge ' + (ls.state === 'recording' ? 'ok' : 'secondary');
  }
}

async function refreshFiles() {
  const d = await api('GET', '/api/datalog/files');
  if (!d) return;
  const list = document.getElementById('file-list');
  list.innerHTML = d.files.map(f => `
    <div class="file-row">
      <span class="file-name">${f.name} ${f.active ? '● ' : ''}</span>
      <span class="file-size">${formatBytes(f.size)}</span>
      <a href="/api/datalog/files/${f.name}" download>
        <button>⬇</button></a>
      <button class="danger" onclick="deleteFile('${f.name}')">🗑</button>
    </div>`).join('');
}

async function calibrateLoadCell() {
  const w = parseFloat(document.getElementById('cal-weight').value);
  if (!w || w <= 0) { toast('Ungültiges Gewicht!'); return; }
  document.getElementById('cal-status').innerHTML = '<span class="spin"></span>';
  const r = await api('POST', '/api/loadcell/cal', { weight_g: w });
  document.getElementById('cal-status').textContent = r && r.ok ? '✓ Kalibriert!' : '✗ Fehler';
  if (r && r.ok) refreshSettings();
}

async function estepCalStart() {
  const dist  = parseFloat(document.getElementById('cal-dist').value)  || 100;
  const speed = parseFloat(document.getElementById('cal-speed-e').value) || 3;
  await api('POST', '/api/motor/cal/start', { distance_mm: dist, speed });
  toast('Extrusion gestartet – Strecke messen!');
}

async function estepCalApply() {
  const rem = parseFloat(document.getElementById('cal-remaining').value);
  if (isNaN(rem)) { toast('Restlänge eingeben!'); return; }
  const r = await api('POST', '/api/motor/cal/apply', { remaining_mm: rem });
  if (r && r.ok) { toast('Neue E-Steps gespeichert.'); refreshSettings(); }
}

async function estepSetManual() {
  const v = parseFloat(document.getElementById('s-esteps-manual').value);
  if (!v || v <= 0) { toast('Ungültiger Wert!'); return; }
  await api('POST', '/api/motor/esteps', { steps_per_mm: v });
  toast('E-Steps gesetzt.'); refreshSettings();
}

async function setMotorCurrent() {
  const run  = parseInt(document.getElementById('m-run-ma').value)  || 800;
  const hold = parseInt(document.getElementById('m-hold-ma').value) || 400;
  await api('POST', '/api/motor/current', { run_ma: run, hold_ma: hold });
  toast('Strom gesetzt.');
}

async function setMicrostep() {
  const ms = parseInt(document.getElementById('m-microstep').value);
  await api('POST', '/api/motor/microstep', { microstep: ms });
  toast('Mikroschritt gesetzt.');
}

async function setStealthchop() {
  const v = document.getElementById('m-stealthchop').value === '1';
  await api('POST', '/api/motor/stealthchop', { enable: v });
  toast('StealthChop ' + (v ? 'AN' : 'AUS') + '.');
}

async function setInterpolation() {
  const v = document.getElementById('m-intpol').value === '1';
  await api('POST', '/api/motor/interpolation', { enable: v });
  toast('Interpolation ' + (v ? 'AN' : 'AUS') + '.');
}

async function datalogStart() {
  await api('POST', '/api/datalog/start', { interval_ms: 1000 });
  toast('Aufzeichnung gestartet.'); refreshSettings();
}

async function datalogStop() {
  await api('POST', '/api/datalog/stop');
  toast('Aufzeichnung gestoppt.'); refreshSettings();
}

async function deleteFile(name) {
  if (!confirm('Datei löschen: ' + name + '?')) return;
  await api('DELETE', '/api/datalog/files/' + name);
  toast(name + ' gelöscht.'); refreshFiles();
}

async function deleteAllFiles() {
  if (!confirm('ALLE Dateien auf der SD-Karte löschen?')) return;
  await api('POST', '/api/datalog/delete_all', { confirm: true });
  toast('Alle Dateien gelöscht.'); refreshFiles();
}

// ── Hilfsfunktionen ───────────────────────────────────────────
function formatBytes(b) {
  if (!b || b === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(b) / Math.log(k));
  return (b / Math.pow(k, i)).toFixed(1) + ' ' + sizes[i];
}

// ── Theme Toggle ─────────────────────────────────────────────
function toggleTheme() {
  const html = document.documentElement;
  const isDark = html.getAttribute('data-theme') === 'dark';
  html.setAttribute('data-theme', isDark ? 'light' : 'dark');
  document.getElementById('theme-btn').textContent = isDark ? '🌙' : '☀️';
  localStorage.setItem('theme', isDark ? 'light' : 'dark');
}

function loadTheme() {
  const saved = localStorage.getItem('theme') || 'dark';
  document.documentElement.setAttribute('data-theme', saved);
  document.getElementById('theme-btn').textContent = saved === 'dark' ? '☀️' : '🌙';
}

// ── Init ─────────────────────────────────────────────────────
window.addEventListener('DOMContentLoaded', () => {
  loadTheme();
  initChart();
  wsConnect();
  loadSequences();
});
