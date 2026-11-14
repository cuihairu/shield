// [SHIELD_VSCODE] Bundle smoke: load the compiled dist/extension.js under a
// minimal vscode mock and drive the real command bodies end to end against
// a local ops HTTP stub. This exercises the esbuild bundle (devcore
// included), command registration, the status bar, the panel webview
// message pump, and the attach pipeline — without downloading a VS Code
// instance. Run with: node test/smoke.mjs

import * as http from 'node:http';
import * as assert from 'node:assert';
import Module from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------- vscode mock

const registered = new Map();
const statusItems = [];
const panels = [];
const shownMessages = [];
const openedDocs = [];
const folders = [{ uri: { fsPath: '/tmp/shield-smoke-ws', scheme: 'file' } }];

function uriOf(...parts) {
  const fsPath = path.join(...parts);
  return { fsPath, scheme: 'file', toString: () => fsPath };
}

const workspaceMock = {
  workspaceFolders: folders,
  getConfiguration(section) {
    // The smoke config: ops URL points at the stub server started below;
    // set via globalThis before activate().
    const cfg = globalThis.shieldSmokeConfig ?? {};
    return {
      get(key, fallback) {
        if (cfg[key] !== undefined) {
          return cfg[key];
        }
        return fallback;
      },
    };
  },
  async openTextDocument(uri) {
    openedDocs.push(uri.fsPath);
    return { uri };
  },
};

const windowMock = {
  createStatusBarItem(alignment, priority) {
    const item = {
      alignment,
      priority,
      text: '',
      tooltip: '',
      name: '',
      command: undefined,
      backgroundColor: undefined,
      show() {
        item.visible = true;
      },
      dispose() {
        item.disposed = true;
      },
    };
    statusItems.push(item);
    return item;
  },
  createWebviewPanel(viewType, title, column, options) {
    const listeners = [];
    const posted = [];
    const panel = {
      viewType,
      title,
      column,
      options,
      iconPath: undefined,
      posted,
      listeners,
      webview: {
        options,
        html: '',
        cspSource: 'https://file.vscode-cdn.net',
        asWebviewUri: (uri) => ({ toString: () => `vscode-webview:${uri.fsPath}` }),
        async postMessage(message) {
          posted.push(message);
          return true;
        },
        onDidReceiveMessage(listener) {
          listeners.push(listener);
          return { dispose() {} };
        },
      },
      onDidDispose(cb) {
        panel.disposeCb = cb;
        return { dispose() {} };
      },
      reveal() {},
      dispose() {},
    };
    panels.push(panel);
    return panel;
  },
  async showTextDocument(doc, opts) {
    shownMessages.push({ doc, opts });
    return doc;
  },
  async showInformationMessage(msg) {
    shownMessages.push({ msg });
  },
  async showErrorMessage(msg) {
    shownMessages.push({ msg });
  },
};

const vscodeMock = {
  workspace: workspaceMock,
  window: windowMock,
  commands: {
    registerCommand(id, cb) {
      registered.set(id, cb);
      return { dispose() {} };
    },
  },
  Uri: {
    file: (p) => ({ fsPath: p, scheme: 'file', toString: () => p }),
    joinPath: (base, ...rest) => uriOf(base.fsPath, ...rest),
  },
  ViewColumn: { One: 1 },
  StatusBarAlignment: { Left: 1, Right: 2 },
  ThemeColor: class {
    constructor(id) {
      this.id = id;
    }
  },
  Disposable: {
    from(...items) {
      return { dispose() {} };
    },
  },
};

// Inject the mock for the 'vscode' require inside the CJS bundle.
const origLoad = Module._load;
Module._load = function patchedLoad(request, parent, isMain) {
  if (request === 'vscode') {
    return vscodeMock;
  }
  return origLoad.call(this, request, parent, isMain);
};

// ---------------------------------------------------------------- ops stub

const opsHits = [];
const opsServer = http.createServer((req, res) => {
  opsHits.push(req.url);
  if (req.url === '/ops/metrics') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end(
      [
        '# TYPE shield_process_cpu_percent gauge',
        'shield_process_cpu_percent 3.5',
        '# TYPE shield_process_rss_bytes gauge',
        'shield_process_rss_bytes 52428800',
      ].join('\n'),
    );
    return;
  }
  if (req.url === '/ops/services') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ type: 'result', data: ['auth'] }));
    return;
  }
  if (req.url === '/ops/services/auth') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(
      JSON.stringify({
        type: 'result',
        data: {
          name: 'auth',
          state: 'running',
          script: '../scripts/auth.lua',
          requests: 1,
          errors: 0,
          memory_kb: 256,
        },
      }),
    );
    return;
  }
  if (req.url === '/ops/profile' && req.method === 'POST') {
    const auth = req.headers.authorization ?? '';
    if (auth !== 'Bearer profile-tok') {
      res.writeHead(401);
      res.end(JSON.stringify({ type: 'error', data: 'unauthorized' }));
      return;
    }
    let raw = '';
    req.on('data', (c) => (raw += c));
    req.on('end', () => {
      const body = JSON.parse(raw || '{}');
      if (body.action === 'start') {
        res.writeHead(200, { 'content-type': 'application/json' });
        res.end(JSON.stringify({ type: 'result', data: { started: true, service: body.service } }));
        return;
      }
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(
        JSON.stringify({
          type: 'result',
          data: {
            service: body.service ?? 'auth',
            total_samples: 42,
            frames: [
              {
                what: 'function',
                name: 'login',
                source: 'auth.lua',
                line: 20,
                tail: false,
                hits: 20,
                pct: 47.6,
                children: [],
              },
            ],
          },
        }),
      );
    });
    return;
  }
  res.writeHead(404);
  res.end('{}');
});

const listen = () =>
  new Promise((resolve) => {
    opsServer.listen(0, '127.0.0.1', () => resolve(opsServer.address().port));
  });

async function waitFor(cond, label, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (cond()) {
      return;
    }
    await new Promise((r) => setTimeout(r, 25));
  }
  throw new Error(`timeout waiting for ${label}`);
}

// ---------------------------------------------------------------- run

const port = await listen();
globalThis.shieldSmokeConfig = {
  opsUrl: `http://127.0.0.1:${port}`,
  opsToken: '',
  profileToken: 'profile-tok',
  logFile: '',
  procPid: 0,
  metricsIntervalMs: 100,
};

const { activate, deactivate } = await import(path.join(here, '..', 'dist', 'extension.js'));

const subscriptions = [];
activate({ subscriptions, extensionUri: uriOf(here, '..') });

assert.ok(registered.has('shield.attach'), 'shield.attach registered');
assert.ok(registered.has('shield.detach'), 'shield.detach registered');
assert.ok(registered.has('shield.openPanel'), 'shield.openPanel registered');
assert.equal(statusItems.length, 1, 'one status bar item');
console.log('ok: commands registered, status bar created');

await registered.get('shield.attach')();

await waitFor(() => statusItems[0].text.includes('shield'), 'status bar text');
await waitFor(() => panels.length > 0, 'panel created');
const panel = panels[0];
assert.ok(panel.webview.html.includes('Content-Security-Policy'), 'panel html has CSP');
assert.ok(panel.webview.html.includes('nonce-'), 'panel html uses nonce');

await waitFor(
  () => panel.posted.some((m) => m.type === 'metrics' && m.sample.cpuPercent === 3.5),
  'metrics sample posted',
);
await waitFor(
  () =>
    panel.posted.some(
      (m) => m.type === 'services' && m.entries.some((e) => e.name === 'auth' && e.resolvedPath),
    ),
  'services posted with resolved script path',
);
assert.ok(opsHits.includes('/ops/metrics'), 'ops stub was polled');
console.log('ok: attach -> metrics + services flow into the panel');

// click-to-open: drive the webview's openScript message through the
// listener the panel registered, and expect the editor to open the file.
const openPath = '/tmp/shield-smoke-ws/scripts/auth.lua';
assert.ok(panel.listeners.length >= 1, 'panel registered a message listener');
for (const listener of panel.listeners) {
  listener({ type: 'openScript', path: openPath });
}
await waitFor(() => openedDocs.includes(openPath), 'script opened in editor');
console.log('ok: openScript message opens the file in the editor');

// flame-graph entry point: the webview posts profileStart; the controller
// drives /ops/profile start+report through devcore and posts the report back.
for (const listener of panel.listeners) {
  listener({ type: 'profileStart', service: 'auth', durationMs: 0 });
}
await waitFor(
  () => panel.posted.some((m) => m.type === 'profileReport' && m.report.total_samples === 42),
  'profile report posted',
);
console.log('ok: profileStart round-trips through /ops/profile into the panel');

await registered.get('shield.detach')();
assert.equal(statusItems[0].text, '$(shield) shield', 'status bar back to detached');
console.log('ok: detach resets the status bar');

deactivate();
opsServer.close();
console.log('SMOKE PASS');
process.exit(0);
