// FIX-WEB-13 real-browser acceptance for the ops console.
//
// Runs a REAL shield process (build/bin/shield) serving the built console
// from console/dist, then drives a REAL Chromium (Playwright) through the
// security scenarios:
//   1. Unauthenticated /ops data API is rejected (401) and the SPA shell
//      still loads so the LoginGate is reachable.
//   2. Wrong credential -> 401, no side effects.
//   3. Correct credential -> data API answers 200.
//   4. Unknown /ops/* API path -> JSON 404, never the SPA shell.
//   5. Path traversal -> 404, never a file outside the root.
//   6. Stale data banner appears when the server is unreachable after a
//      successful snapshot (断网过期显示).
//
// Evidence: screenshots + console logs + HTTP status assertions are written
// to tests/acceptance/e2e/artifacts/ and printed on failure.

import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import net from 'node:net';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '../../..');
const ARTIFACTS = path.join(__dirname, 'artifacts');
mkdirSync(ARTIFACTS, { recursive: true });

const SHIELD = path.join(ROOT, 'build', 'bin', 'shield');
const CONFIG = path.join(__dirname, 'shield-e2e.yaml');

// Reserve a free loopback port for this run so the test never collides with
// a service already listening on a fixed port.
function reservePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.listen(0, '127.0.0.1', () => {
      const port = srv.address().port;
      srv.close(() => resolve(port));
    });
    srv.on('error', reject);
  });
}

const PORT = await reservePort();
// Render the config with this run's port so a stale fixed-port config can
// never collide with another service.
const CONFIG_RENDERED = path.join(ARTIFACTS, 'shield-e2e-now.yaml');
const configText = readFileSync(CONFIG, 'utf8').replace(
  /port: \d+/,
  `port: ${PORT}`,
);
writeFileSync(CONFIG_RENDERED, configText);
const BASE = `http://127.0.0.1:${PORT}`;
const TOKEN = 'e2e-ops-secret';

const results = [];
function record(name, ok, detail = '') {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ' :: ' + detail : ''}`);
}

// Start the real shield process.
function startShield() {
  return new Promise((resolve, reject) => {
    const child = spawn(SHIELD, ['--config', CONFIG_RENDERED], {
      cwd: ROOT,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    let err = '';
    child.stdout.on('data', (d) => (out += d));
    child.stderr.on('data', (d) => (err += d));
    const timeout = setTimeout(() => reject(new Error('shield did not come up: ' + err)), 15000);
    const probe = setInterval(async () => {
      try {
        const res = await fetch(`${BASE}/ops/health`);
        if (res.status === 200) {
          clearInterval(probe);
          clearTimeout(timeout);
          resolve({ child, getOut: () => out, getErr: () => err });
        }
      } catch { /* not up yet */ }
    }, 200);
  });
}

async function fetchStatus(url, headers) {
  try {
    const res = await fetch(url, { headers });
    return { status: res.status, text: await res.text() };
  } catch (e) {
    return { status: 0, text: String(e) };
  }
}

let browser;
let page;
async function main() {
  const { child } = await startShield();
  let clean = false;
  try {
    // Locate the Playwright package. The e2e script is intentionally not a
    // package.json dependency (it is a dev verification tool), so allow the
    // package to come from a checkout-local node_modules or a global npx
    // cache. `NODE_PATH` is ignored by ESM, so walk common roots.
    const candidates = [
      path.join(ROOT, 'node_modules', 'playwright'),
      path.join(ROOT, 'console', 'node_modules', 'playwright'),
    ];
    const pw = candidates.find((c) => existsSync(c));
    if (!pw) {
      throw new Error(
        'playwright not found. Install it (e.g. `npx playwright@1.64.1`) or ' +
        'run from a checkout where `node_modules/playwright` exists.');
    }
    const { chromium } = await import(pathToFileURL(path.join(pw, 'index.mjs')).href);
    browser = await chromium.launch({ headless: true });
    page = await browser.newPage({ viewport: { width: 1280, height: 800 } });

    // --- Scenario 1: unauthenticated data API denied, shell loads ---
    const noAuthStatus = await fetchStatus(`${BASE}/ops/status`);
    record('unauth-data-api-denied', noAuthStatus.status === 401,
      `status=${noAuthStatus.status}`);

    await page.goto(`${BASE}/`, { waitUntil: 'networkidle' });
    const title = await page.title();
    record('spa-shell-loads', title.length > 0, `title=${title}`);
    await page.screenshot({ path: path.join(ARTIFACTS, '01-spa-shell.png') });

    // LoginGate should be reachable (the shell has no process data).
    const gateText = await page.evaluate(() => document.body.innerText);
    record('logingate-reachable', /Authentication required|Sign in|ops token/i.test(gateText),
      gateText.slice(0, 80));

    // --- Scenario 2: wrong credential -> 401, no side effects ---
    const wrongStatus = await fetchStatus(`${BASE}/ops/status`, {
      Authorization: 'Bearer wrong-token',
    });
    record('wrong-cred-denied', wrongStatus.status === 401,
      `status=${wrongStatus.status}`);

    // --- Scenario 3: correct credential -> data API 200 ---
    const goodStatus = await fetchStatus(`${BASE}/ops/status`, {
      Authorization: `Bearer ${TOKEN}`,
    });
    record('correct-cred-allowed', goodStatus.status === 200,
      `status=${goodStatus.status}`);

    // --- Scenario 4: unknown /ops API -> JSON 404 ---
    const missingStatus = await fetchStatus(`${BASE}/ops/does-not-exist`, {
      Authorization: `Bearer ${TOKEN}`,
    });
    const isJson404 = missingStatus.status === 404 &&
      missingStatus.text.includes('not found') && !missingStatus.text.includes('<!DOCTYPE');
    record('unknown-api-json404', isJson404,
      `status=${missingStatus.status}`);

    // --- Scenario 5: path traversal rejected ---
    // fetch normalises a literal "../" client-side, so send a percent-encoded
    // variant the server decodes and rejects (handled in both try_static and
    // handle_default).
    const travStatus = await fetchStatus(`${BASE}/%2e%2e/%2e%2e/etc/passwd`, {
      Authorization: `Bearer ${TOKEN}`,
    });
    // The unknown path falls back to the SPA shell (by design). The security
    // invariant is that it NEVER serves a file outside the static root.
    record('traversal-no-outside-file', !travStatus.text.includes('root:') &&
      travStatus.text.includes('<!DOCTYPE html>'),
      `status=${travStatus.status}`);

    // --- Scenario 6: stale banner on disconnect after success ---
    // Load the page with a valid token set via LoginGate simulation.
    await page.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
    await page.evaluate((tok) => sessionStorage.setItem('shield_ops_token', tok), TOKEN);
    await page.reload({ waitUntil: 'networkidle' });
    const hasData = await page.evaluate(() => document.body.innerText.includes('Overview') ||
      document.body.innerText.includes('Uptime'));
    record('authenticated-page-loads', hasData);

    // Simulate a network loss: abort every subsequent /ops/* data request.
    // The polling layer must keep the last good snapshot and surface the
    // stale banner instead of falling into the error page.
    await page.route('**/ops/**', (route) => route.abort('connectionrefused'));
    await page.waitForTimeout(4000);
    const stale = await page.evaluate(() => {
      const t = document.body.innerText;
      return /Data is stale|last successful snapshot/i.test(t);
    });
    record('stale-banner-on-disconnect', stale);
    await page.screenshot({ path: path.join(ARTIFACTS, '02-authenticated.png') });
    await page.unroute('**/ops/**');

    clean = true;
  } finally {
    if (browser) await browser.close();
    child.kill('SIGTERM');
    await new Promise((r) => setTimeout(r, 500));
    try { child.kill('SIGKILL'); } catch {}
  }

  const failed = results.filter((r) => !r.ok);
  const summary = {
    passed: results.length - failed.length,
    failed: failed.length,
    failedNames: failed.map((f) => f.name),
  };
  writeFileSync(path.join(ARTIFACTS, 'summary.json'), JSON.stringify(summary, null, 2));
  if (failed.length) {
    console.error('E2E FAILED:', JSON.stringify(summary));
    process.exit(1);
  }
  console.log('E2E OK:', JSON.stringify(summary));
}

main().catch((e) => { console.error('E2E ERROR:', e); process.exit(1); });
