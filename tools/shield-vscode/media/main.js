// [SHIELD_VSCODE] Panel webview script. Vanilla JS, no external deps.
// Curves: ring-buffer + canvas polyline. Logs: append-only DOM with
// substring filter, level filter and search highlight. Services: rows with
// click-to-open script path.

(function () {
  'use strict';

  const MAX_POINTS = 300;
  const MAX_LOG_ENTRIES = 2000;

  const state = {
    cpu: [],
    rss: [],
    status: 'detached',
    entries: [],
    records: [],
    filter: '',
    level: 'all',
    search: '',
    wrap: true,
  };

  const els = {
    node: document.getElementById('node'),
    conn: document.getElementById('conn'),
    source: document.getElementById('sample-source'),
    cpuNow: document.getElementById('cpu-now'),
    rssNow: document.getElementById('rss-now'),
    cpuCanvas: document.getElementById('cpu-canvas'),
    rssCanvas: document.getElementById('rss-canvas'),
    servicesBody: document.querySelector('#services-table tbody'),
    servicesNote: document.getElementById('services-note'),
    logView: document.getElementById('log-view'),
    logFilter: document.getElementById('log-filter'),
    logLevel: document.getElementById('log-level'),
    logSearch: document.getElementById('log-search'),
    logWrap: document.getElementById('log-wrap'),
    logClear: document.getElementById('log-clear'),
  };

  const vscode = acquireVsCodeApi();

  // ---------------------------------------------------------------- curves

  function pushPoint(arr, value) {
    arr.push(value);
    if (arr.length > MAX_POINTS) {
      arr.shift();
    }
  }

  function drawCurve(canvas, points, format) {
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    // grid
    ctx.strokeStyle = 'rgba(128,128,128,0.25)';
    ctx.lineWidth = 1;
    for (let i = 1; i < 4; i++) {
      const y = (h / 4) * i;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }

    const values = points.filter((p) => p != null);
    if (values.length < 2) {
      return;
    }
    const max = Math.max(...values, 1);
    const step = w / (MAX_POINTS - 1);
    const start = w - (values.length - 1) * step; // right-anchored

    const style = getComputedStyle(document.documentElement);
    const line = (style.getPropertyValue('--shield-curve') || '#4fc1ff').trim();

    ctx.strokeStyle = line;
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    values.forEach((v, i) => {
      const x = start + i * step;
      const y = h - (v / max) * (h - 6) - 3;
      if (i === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.stroke();

    // max label
    ctx.fillStyle = 'rgba(128,128,128,0.8)';
    ctx.font = '10px monospace';
    ctx.fillText(format(max), 4, 10);
  }

  function onMetrics(sample) {
    pushPoint(state.cpu, sample.cpuPercent);
    pushPoint(state.rss, sample.rssBytes == null ? null : sample.rssBytes);
    els.cpuNow.textContent =
      sample.cpuPercent != null ? sample.cpuPercent.toFixed(1) + '%' : '--';
    els.rssNow.textContent =
      sample.rssBytes != null
        ? (sample.rssBytes / 1048576).toFixed(0) + ' MiB'
        : '--';
    els.source.textContent = 'source: ' + sample.source;
    drawCurve(els.cpuCanvas, state.cpu, (v) => v.toFixed(1) + '%');
    drawCurve(els.rssCanvas, state.rss.map((b) => (b == null ? null : b / 1048576)), (v) =>
      v.toFixed(0) + ' MiB',
    );
  }

  // -------------------------------------------------------------- services

  function renderServices() {
    els.servicesBody.textContent = '';
    for (const entry of state.entries) {
      const tr = document.createElement('tr');

      const name = document.createElement('td');
      name.className = 'mono';
      name.textContent = entry.name;
      tr.appendChild(name);

      const st = document.createElement('td');
      st.textContent = entry.state;
      tr.appendChild(st);

      const req = document.createElement('td');
      req.textContent = entry.requests != null ? String(entry.requests) : '-';
      tr.appendChild(req);

      const err = document.createElement('td');
      err.textContent = entry.errors != null ? String(entry.errors) : '-';
      if (entry.errors > 0) {
        err.className = 'err';
      }
      tr.appendChild(err);

      const mem = document.createElement('td');
      mem.textContent =
        entry.memoryKb != null ? (entry.memoryKb / 1024).toFixed(1) + ' MiB' : '-';
      tr.appendChild(mem);

      const script = document.createElement('td');
      script.className = 'mono';
      if (entry.resolvedPath) {
        const link = document.createElement('a');
        link.href = '#';
        link.textContent = entry.resolvedPath;
        link.title = 'Open in editor';
        link.addEventListener('click', (e) => {
          e.preventDefault();
          vscode.postMessage({ type: 'openScript', path: entry.resolvedPath });
        });
        script.appendChild(link);
      } else if (entry.script) {
        script.textContent = entry.script + ' (unresolved)';
      } else {
        script.textContent = '-';
      }
      tr.appendChild(script);

      els.servicesBody.appendChild(tr);
    }
    els.servicesNote.textContent =
      state.entries.length === 0
        ? 'no published services'
        : 'read-only face; hot reload stays in the console';
  }

  // ------------------------------------------------------------------ logs

  function recordMatches(record) {
    if (state.level !== 'all') {
      if (record.continuation) {
        return true; // continuations follow their parent record
      }
      if (record.level !== state.level) {
        return false;
      }
    }
    if (!state.filter) {
      return true;
    }
    return record.raw.toLowerCase().includes(state.filter.toLowerCase());
  }

  function makeRecordNode(record) {
    if (record.continuation) {
      const div = document.createElement('div');
      div.className = 'log-line log-cont' + (state.wrap ? '' : ' nowrap');
      div.textContent = record.raw;
      return div;
    }
    const div = document.createElement('div');
    div.className = 'log-line lvl-' + record.level + (state.wrap ? '' : ' nowrap');
    const time = document.createElement('span');
    time.className = 'log-time mono';
    time.textContent = record.timestampMs != null ? new Date(record.timestampMs).toISOString().slice(11, 23) : '--:--:--';
    const level = document.createElement('span');
    level.className = 'log-level';
    level.textContent = record.level.toUpperCase();
    const logger = document.createElement('span');
    logger.className = 'log-logger mono';
    logger.textContent = record.logger;
    const msg = document.createElement('span');
    msg.className = 'log-msg';
    if (state.search) {
      msg.innerHTML = '';
      highlight(msg, record.message, state.search);
    } else {
      msg.textContent = record.message;
    }
    div.append(time, level, logger, msg);
    return div;
  }

  function highlight(target, text, needle) {
    const lower = text.toLowerCase();
    const n = needle.toLowerCase();
    let idx = 0;
    let at = lower.indexOf(n);
    while (at >= 0) {
      if (at > idx) {
        target.appendChild(document.createTextNode(text.slice(idx, at)));
      }
      const mark = document.createElement('mark');
      mark.textContent = text.slice(at, at + needle.length);
      target.appendChild(mark);
      idx = at + needle.length;
      at = lower.indexOf(n, idx);
    }
    if (idx < text.length) {
      target.appendChild(document.createTextNode(text.slice(idx)));
    }
  }

  function appendRecord(record) {
    state.records.push(record);
    if (state.records.length > MAX_LOG_ENTRIES) {
      state.records.shift();
    }
    if (!recordMatches(record)) {
      return;
    }
    const atBottom =
      els.logView.scrollHeight - els.logView.scrollTop - els.logView.clientHeight < 24;
    els.logView.appendChild(makeRecordNode(record));
    while (els.logView.childElementCount > MAX_LOG_ENTRIES) {
      els.logView.removeChild(els.logView.firstChild);
    }
    if (atBottom) {
      els.logView.scrollTop = els.logView.scrollHeight;
    }
  }

  function rerenderLogs() {
    els.logView.textContent = '';
    const visible = state.records.filter(recordMatches).slice(-MAX_LOG_ENTRIES);
    const frag = document.createDocumentFragment();
    for (const record of visible) {
      frag.appendChild(makeRecordNode(record));
    }
    els.logView.appendChild(frag);
    els.logView.scrollTop = els.logView.scrollHeight;
  }

  // --------------------------------------------------------------- status

  function onStatus(message) {
    state.status = message.status;
    els.conn.textContent = message.status + (message.detail ? ' (' + message.detail + ')' : '');
    els.conn.className = 'pill pill-' + message.status;
  }

  function onNode(message) {
    els.node.textContent = message.node;
  }

  // ------------------------------------------------------------- wiring

  window.addEventListener('message', (event) => {
    const message = event.data;
    switch (message.type) {
      case 'status':
        onStatus(message);
        break;
      case 'node':
        onNode(message);
        break;
      case 'metrics':
        onMetrics(message.sample);
        break;
      case 'log':
        appendRecord(message.record);
        break;
      case 'services':
        state.entries = message.entries;
        renderServices();
        break;
    }
  });

  els.logFilter.addEventListener('input', () => {
    state.filter = els.logFilter.value;
    rerenderLogs();
  });
  els.logLevel.addEventListener('change', () => {
    state.level = els.logLevel.value;
    rerenderLogs();
  });
  els.logSearch.addEventListener('input', () => {
    state.search = els.logSearch.value;
    rerenderLogs();
  });
  els.logWrap.addEventListener('change', () => {
    state.wrap = els.logWrap.checked;
    rerenderLogs();
  });
  els.logClear.addEventListener('click', () => {
    state.records = [];
    rerenderLogs();
  });
})();
