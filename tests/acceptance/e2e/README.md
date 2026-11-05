# Ops Console browser acceptance (FIX-WEB-13)

Runs a **real** `shield` process against the **built** console (`console/dist`)
and drives a **real Chromium** through the FIX-WEB-13 security scenarios. This
is the browser-level evidence required by FIX-WEB-13; it deliberately does NOT
substitute the in-process `test_cov_ops_http` unit cases or the embedded
single-page dashboard for browser proof.

## Scenarios

1. Unauthenticated `/ops` data API is rejected with `401`; the SPA shell still
   loads so the `LoginGate` is reachable.
2. Wrong credential -> `401`, no side effects.
3. Correct credential -> data API answers `200`.
4. Unknown `/ops/*` API path -> JSON `404`, never the HTML shell.
5. Path traversal (`%2e%2e`) never serves a file outside the static root.
6. Authenticated page loads with live data (no error page).

## Run

Prerequisites: a `build/bin/shield` binary, `console/dist` built (see FIX-WEB-14),
and the Playwright package + a Chromium browser installed.

```bash
# Build the console (FIX-WEB-14) and the shield binary first.
cd console && pnpm build && cd ..

# The script needs the `playwright` package. It resolves it from the checkout
# root node_modules or console/node_modules. Install it if absent, e.g.:
#   npm install --prefix console playwright@1.64.1
node tests/acceptance/e2e/run-e2e.mjs
```

Exit code is `0` on pass, `1` on failure.

## Evidence

On every run the script writes to `tests/acceptance/e2e/artifacts/`:

- `01-spa-shell.png` / `02-authenticated.png` — browser screenshots.
- `summary.json` — pass/fail summary with failed scenario names.
- `shield-e2e-now.yaml` — the config used for that run (rendered port).

The artifacts directory is git-ignored (binaries and generated config); the
scenario assertions and this README are the reproducible, traceable contract.
