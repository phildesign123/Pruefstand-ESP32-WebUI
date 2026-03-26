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
  if (name === 'wifi') refreshWifi();
  if (name === 'dashboard') refreshRec();
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

  // Kraft (Newton)
  document.getElementById('weight-val').innerHTML =
    `${d.weight.toFixed(3)}<span class="unit"> N</span>`;
  document.getElementById('motor-speed-val').innerHTML =
    `${d.speed.toFixed(2)}<span class="unit"> mm/s</span>`;
  document.getElementById('motor-status').textContent =
    `Motor: ${d.motor ? 'AN' : 'AUS'}`;

  // Sequencer
  const prevState = seqState.state;
  seqState = { state: d.seq_state, active: d.seq, remain: d.seq_remain || 0 };
  updateSeqTable();
  if (prevState && prevState !== 'idle' && d.seq_state === 'idle') {
    // Verzögert refreshen: Datalog braucht ~2s zum Flushen nach Stop
    setTimeout(refreshRec, 3000);
  }

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
    document.getElementById('s-weight').textContent = d.weight.toFixed(3);
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
  const pad  = { l: 50, r: 85, t: 10, b: 25 };
  const n    = chartData.ts.length;

  ctx.clearRect(0, 0, W, H);
  if (n < 2) return;

  const showTemp   = document.getElementById('show-temp').checked;
  const showWeight = document.getElementById('show-weight').checked;
  const showSpeed  = document.getElementById('show-speed').checked;

  const tMin = chartData.ts[0];
  const tMax = chartData.ts[n - 1];

  // Feste Skalen
  const tempMin = 0, tempMax = 280;
  const speedMin = 0, speedMax = Math.max(1, Math.ceil(Math.max(...chartData.speed)));
  // Kraft (N): auf nächsten ganzen Newton aufrunden, min 1 N
  const weightMax = Math.max(1, Math.ceil(Math.max(...chartData.weight)));

  function tx(t) { return pad.l + (t - tMin) / (tMax - tMin) * (W - pad.l - pad.r); }
  function tyL(v, mn, mx) { return pad.t + (1 - (v - mn) / (mx - mn)) * (H - pad.t - pad.b); }

  // Grid – gemeinsame Linien für alle rechten Achsen
  const gridSteps = Math.max(weightMax, speedMax);
  const isDark = document.documentElement.getAttribute('data-theme') === 'dark';
  ctx.strokeStyle = isDark ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.08)';
  ctx.lineWidth = 1;
  for (let i = 0; i <= gridSteps; i++) {
    const f = i / gridSteps;
    const y = pad.t + (1 - f) * (H - pad.t - pad.b);
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

  if (showTemp)   drawLine(chartData.temp,   '#ef4444', v => tyL(v, tempMin, tempMax));
  if (showWeight) drawLine(chartData.weight, '#10b981', v => tyL(v, 0, weightMax));
  if (showSpeed)  drawLine(chartData.speed,  '#f59e0b', v => tyL(v, speedMin, speedMax));

  // Achsbeschriftung
  ctx.font = '10px Arial';
  // Linke Achse: Temperatur (indigo) – fest 0–280
  if (showTemp) {
    ctx.fillStyle = '#ef4444';
    ctx.textAlign = 'right';
    for (let v = 0; v <= tempMax; v += 50) {
      const y = tyL(v, tempMin, tempMax);
      ctx.fillText(v + '°C', pad.l - 3, y + 3);
    }
  }
  // Rechte Achse(n) – nebeneinander
  ctx.textAlign = 'left';
  const rx = W - pad.r + 3;
  if (showWeight) {
    ctx.fillStyle = '#10b981';
    for (let i = 0; i <= gridSteps; i++) {
      const nv = (i / gridSteps) * weightMax;
      if (Math.abs(nv - Math.round(nv)) < 0.01) {
        const y = pad.t + (1 - i / gridSteps) * (H - pad.t - pad.b);
        ctx.fillText(Math.round(nv) + 'N', rx, y + 3);
      }
    }
  }
  if (showSpeed) {
    ctx.fillStyle = '#f59e0b';
    const rx2 = rx + (showWeight ? 30 : 0);
    for (let i = 0; i <= gridSteps; i++) {
      const sv = (i / gridSteps) * speedMax;
      if (Math.abs(sv - Math.round(sv)) < 0.01) {
        const y = pad.t + (1 - i / gridSteps) * (H - pad.t - pad.b);
        ctx.fillText(Math.round(sv), rx2, y + 3);
      }
    }
  }

  // Zeitachse
  ctx.fillStyle = isDark ? 'rgba(255,255,255,0.5)' : 'rgba(0,0,0,0.5)';
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
function motorStart() {
  const speed = parseFloat(document.getElementById('jog-speed').value) || 3;
  api('POST', '/api/motor/move', { speed, duration_s: 3600, dir: 'fwd' });
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
  const statusIcons = { idle:'·', heating:'⏳', running:'<span class="active-dot"></span>', done:'✓', error:'✗', next:'»' };

  tbody.innerHTML = sequences.map((s, i) => {
    const isActive = i === seqState.active;
    const icon = isActive ? statusIcons[seqState.state] || '·' : '·';
    const cls  = isActive ? 'active-seq' : '';
    const remain = (isActive && seqState.state === 'running' && seqState.remain > 0)
      ? `${seqState.remain.toFixed(1)} / ${s.duration_s}` : `${s.duration_s}`;
    return `<tr class="${cls}">
      <td>${i + 1}</td>
      <td>${s.temp_c}</td>
      <td>${s.speed_mm_s}</td>
      <td>${remain}</td>
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
  const body = {};
  const fn = document.getElementById('seq-filename').value.trim();
  if (fn) body.filename = fn;
  const r = await api('POST', '/api/sequence/start', body);
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

// ── Messreihen-Presets (SD-Karte via API) ────────────────────

async function seqPresetsRefreshDropdown() {
  const sel = document.getElementById('seq-preset');
  if (!sel) return;
  const d = await api('GET', '/api/preset-list');
  const presets = (d && d.presets) ? d.presets : {};
  const names = Object.keys(presets).sort();
  sel.innerHTML = '<option value="">— Gespeicherte Messreihe —</option>'
    + names.map(n => `<option value="${n}">${n}</option>`).join('');
}

async function seqPresetSave() {
  const nameEl = document.getElementById('seq-preset-name');
  const name = nameEl.value.trim();
  if (!name) { toast('Bitte einen Namen eingeben.'); return; }
  if (sequences.length === 0) { toast('Keine Messreihe vorhanden.'); return; }
  const seqs = sequences.map(s => ({ temp_c: s.temp_c, speed_mm_s: s.speed_mm_s, duration_s: s.duration_s }));
  const r = await api('POST', '/api/preset-save', { name: name, sequences: seqs });
  if (r && r.ok) {
    nameEl.value = '';
    await seqPresetsRefreshDropdown();
    document.getElementById('seq-preset').value = name;
    toast(`Messreihe "${name}" gespeichert.`);
  } else {
    toast('Fehler beim Speichern.');
  }
}

async function seqPresetLoad() {
  const sel = document.getElementById('seq-preset');
  const name = sel.value;
  if (!name) return;
  const d = await api('GET', '/api/preset-list');
  const presets = (d && d.presets) ? d.presets : {};
  const rows = presets[name];
  if (!rows || rows.length === 0) { toast('Preset leer.'); return; }
  await api('POST', '/api/sequence/clear');
  for (const r of rows) {
    await api('POST', '/api/sequence/add', { temp_c: r.temp_c, speed_mm_s: r.speed_mm_s, duration_s: r.duration_s });
  }
  await loadSequences();
  toast(`Messreihe "${name}" geladen.`);
}

async function seqPresetDelete() {
  const sel = document.getElementById('seq-preset');
  const name = sel.value;
  if (!name) { toast('Bitte eine Messreihe auswählen.'); return; }
  const r = await api('POST', '/api/preset-del', { name });
  if (r && r.ok) {
    await seqPresetsRefreshDropdown();
    toast(`Messreihe "${name}" gelöscht.`);
  } else {
    toast('Fehler beim Löschen.');
  }
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

  // TMC2208 Konfiguration
  const mc = await api('GET', '/api/motor/config');
  if (mc) {
    document.getElementById('m-run-ma').value = mc.run_ma;
    document.getElementById('m-hold-ma').value = mc.hold_ma;
    document.getElementById('m-microstep').value = mc.microsteps;
    document.getElementById('m-stealthchop').value = mc.stealthchop ? '1' : '0';
    document.getElementById('m-intpol').value = mc.interpolation ? '1' : '0';
    document.getElementById('m-dir').value = mc.dir_invert ? '1' : '0';
  }

  // SD-Info – automatisch mounten falls nötig
  let sd = await api('GET', '/api/datalog/sdinfo');
  if (sd && !sd.mounted) {
    const m = await api('POST', '/api/datalog/mount');
    if (m && m.ok) sd = await api('GET', '/api/datalog/sdinfo');
  }
  if (sd && sd.mounted) {
    document.getElementById('sd-free').textContent  = formatBytes(sd.free);
    document.getElementById('sd-total').textContent = formatBytes(sd.total);
  } else {
    document.getElementById('sd-free').textContent  = '–';
    document.getElementById('sd-total').textContent = 'nicht erkannt';
  }

  // Dateien laden
  await refreshFiles();

  // Datalog Status
  const ls = await api('GET', '/api/datalog/status');
  if (ls) {
    const b = document.getElementById('log-state');
    if (ls.state === 'recording') {
      b.innerHTML = '<span class="rec-dot"></span>recording';
      b.className = 'badge ok';
    } else {
      b.textContent = ls.state;
      b.className = 'badge secondary';
    }
  }
}

async function refreshFiles() {
  const d = await api('GET', '/api/datalog/filelist');
  if (!d) return;
  const list = document.getElementById('file-list');
  list.innerHTML = d.files.reverse().map(f => `
    <div class="file-row">
      ${f.active ? '<span class="active-dot"></span>' : '<span style="width:10px;display:inline-block"></span>'}
      <span class="file-name">${f.name}</span>
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

async function setMotorDir() {
  const v = document.getElementById('m-dir').value === '1';
  await api('POST', '/api/motor/dir', { invert: v });
  toast('Richtung: ' + (v ? 'Invertiert' : 'Normal') + '.');
}

async function datalogStart() {
  const r = await api('POST', '/api/datalog/start', { interval_ms: 100 });
  if (r && r.ok) toast('Aufzeichnung gestartet.');
  else toast(r?.error || 'Start fehlgeschlagen!');
  refreshSettings();
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

async function mountSD() {
  const r = await api('POST', '/api/datalog/mount');
  if (r && r.ok) { toast('SD-Karte gemountet.'); refreshSettings(); }
  else toast('SD-Karte nicht gefunden!');
}

async function deleteAllFiles() {
  if (!confirm('ALLE Dateien auf der SD-Karte löschen?')) return;
  await api('POST', '/api/datalog/delete_all', { confirm: true });
  toast('Alle Dateien gelöscht.'); refreshFiles();
}

// ── Dashboard SD-Aufzeichnung ─────────────────────────────────

async function recStart() {
  const name = document.getElementById('rec-name').value.trim();
  const body = { interval_ms: 100 };
  if (name) body.filename = name;
  const r = await api('POST', '/api/datalog/start', body);
  if (r && r.ok) toast('Aufzeichnung gestartet.');
  else toast(r?.error || 'Start fehlgeschlagen!');
  refreshRec();
}

async function recStop() {
  await api('POST', '/api/datalog/stop');
  toast('Aufzeichnung gestoppt.');
  refreshRec();
}

async function refreshRec() {
  // SD-Info – automatisch mounten falls nötig
  let sd = await api('GET', '/api/datalog/sdinfo');
  if (sd && !sd.mounted) {
    const m = await api('POST', '/api/datalog/mount');
    if (m && m.ok) sd = await api('GET', '/api/datalog/sdinfo');
  }
  if (sd && sd.mounted) {
    document.getElementById('rec-sd-free').textContent  = formatBytes(sd.free);
    document.getElementById('rec-sd-total').textContent = formatBytes(sd.total);
  } else {
    document.getElementById('rec-sd-free').textContent  = '–';
    document.getElementById('rec-sd-total').textContent = 'nicht erkannt';
  }
  // Status
  const ls = await api('GET', '/api/datalog/status');
  if (ls) {
    const b = document.getElementById('rec-state');
    if (ls.state === 'recording') {
      b.innerHTML = '<span class="rec-dot"></span>recording';
      b.className = 'badge ok';
    } else {
      b.textContent = ls.state;
      b.className = 'badge secondary';
    }
  }
  // Dateiliste
  const d = await api('GET', '/api/datalog/filelist');
  if (!d) return;
  const list = document.getElementById('rec-file-list');
  list.innerHTML = d.files.reverse().map(f => `
    <div class="file-row">
      ${f.active ? '<span class="active-dot"></span>' : '<span style="width:10px;display:inline-block"></span>'}
      <span class="file-name">${f.name}</span>
      <span class="file-size">${formatBytes(f.size)}</span>
      <a href="/api/datalog/files/${f.name}" download>
        <button>⬇</button></a>
      <button class="danger" onclick="deleteFile('${f.name}');setTimeout(refreshRec,500)">🗑</button>
    </div>`).join('');
}

// ── Hilfsfunktionen ───────────────────────────────────────────
function formatBytes(b) {
  if (!b || b === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(b) / Math.log(k));
  return (b / Math.pow(k, i)).toFixed(1) + ' ' + sizes[i];
}

// ── WiFi ─────────────────────────────────────────────────────
async function refreshWifi() {
  const d = await api('GET', '/api/wifi/status');
  if (!d) return;
  document.getElementById('wifi-mac').textContent = d.mac;
  document.getElementById('wifi-mode').textContent = d.mode;

  const staBadge = document.getElementById('wifi-sta-status');
  if (d.sta_connected) {
    staBadge.textContent = 'Verbunden';
    staBadge.className = 'badge ok';
    document.getElementById('wifi-sta-ip').textContent = d.sta_ip;
    document.getElementById('wifi-sta-rssi').textContent = d.sta_rssi;
  } else {
    staBadge.textContent = 'AP-Modus';
    staBadge.className = 'badge warn';
    document.getElementById('wifi-sta-ip').textContent = d.ap_ip || '192.168.4.1';
    document.getElementById('wifi-sta-rssi').textContent = '–';
  }

  document.getElementById('wifi-sta-ssid').value = d.sta_ssid || '';
  document.getElementById('wifi-sta-pass').value = d.sta_password || '';
  document.getElementById('wifi-ap-ssid').value = d.ap_ssid || '';
  document.getElementById('wifi-ap-pass').value = d.ap_password || '';
}

async function wifiSave() {
  const sta_ssid = document.getElementById('wifi-sta-ssid').value.trim();
  const sta_pass = document.getElementById('wifi-sta-pass').value;
  const ap_ssid  = document.getElementById('wifi-ap-ssid').value.trim();
  const ap_pass  = document.getElementById('wifi-ap-pass').value;
  if (!ap_ssid) { toast('AP-SSID darf nicht leer sein!'); return; }
  if (ap_pass.length > 0 && ap_pass.length < 8) { toast('AP-Passwort: min. 8 Zeichen!'); return; }
  const r = await api('POST', '/api/wifi/save', {
    sta_ssid, sta_password: sta_pass,
    ap_ssid, ap_password: ap_pass
  });
  if (r && r.ok) toast('WiFi-Einstellungen gespeichert');
  else toast('Fehler beim Speichern');
}

async function wifiScanAndShow() {
  const list = document.getElementById('wifi-scan-list');
  list.style.display = 'block';
  list.innerHTML = '<span class="spin"></span> Suche Netzwerke...';
  const d = await api('GET', '/api/wifi/scan');
  if (!d || !d.networks || d.networks.length === 0) {
    list.innerHTML = '<span style="color:var(--text-dim)">Keine Netzwerke gefunden</span>';
    return;
  }
  list.innerHTML = '';
  d.networks.forEach(n => {
    const row = document.createElement('div');
    row.className = 'file-row';
    row.style.cursor = 'pointer';
    row.innerHTML = `<span class="file-name">${n.ssid} ${n.open ? '🔓' : '🔒'}</span>`
                  + `<span class="file-size">${n.rssi ? n.rssi + ' dBm' : 'gespeichert'}</span>`;
    row.addEventListener('click', () => {
      document.getElementById('wifi-sta-ssid').value = n.ssid;
      document.getElementById('wifi-sta-pass').value = '';
      document.getElementById('wifi-sta-pass').focus();
      toast('SSID ausgewählt: ' + n.ssid);
    });
    list.appendChild(row);
  });
}

async function wifiRestart() {
  if (!confirm('ESP32 wirklich neustarten?')) return;
  await api('POST', '/api/wifi/restart');
  toast('Neustart...');
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
  seqPresetsRefreshDropdown();
  refreshRec();
});
