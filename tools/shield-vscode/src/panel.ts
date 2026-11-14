// [SHIELD_VSCODE] DevTools panel webview: CPU/memory curves, the live log
// stream (filter + search), and the read-only Lua script face. The panel is
// a thin shell: all data comes from devcore via postMessage, and the only
// message back is a click-to-open script jump.

import * as vscode from 'vscode';

import type {
  LuaServiceEntry,
  LogRecord,
  MetricsSample,
  ProfileResponse,
  SessionStatus,
} from '@shield/shield-devcore';

export interface ShieldPanelMessage {
  type: 'openScript';
  path: string;
}

export interface ProfilePanelMessage {
  type: 'profileStart';
  service: string;
  durationMs: number;
}

export class ShieldPanel {
  private static activePanel: ShieldPanel | undefined;

  private readonly panel: vscode.WebviewPanel;
  private disposables: vscode.Disposable[] = [];
  private onDidReceive:
    | ((message: ShieldPanelMessage | ProfilePanelMessage) => void)
    | undefined;

  public static createOrShow(
    extensionUri: vscode.Uri,
    onMessage: (message: ShieldPanelMessage | ProfilePanelMessage) => void,
  ): ShieldPanel {
    if (ShieldPanel.activePanel) {
      ShieldPanel.activePanel.panel.reveal(vscode.ViewColumn.One);
      ShieldPanel.activePanel.onDidReceive = onMessage;
      return ShieldPanel.activePanel;
    }
    const panel = new ShieldPanel(extensionUri, onMessage);
    ShieldPanel.activePanel = panel;
    return panel;
  }

  public static get current(): ShieldPanel | undefined {
    return ShieldPanel.activePanel;
  }

  private constructor(
    extensionUri: vscode.Uri,
    onMessage: (message: ShieldPanelMessage | ProfilePanelMessage) => void,
  ) {
    this.onDidReceive = onMessage;
    this.panel = vscode.window.createWebviewPanel(
      'shieldDevTools',
      'Shield DevTools',
      vscode.ViewColumn.One,
      {
        enableScripts: true,
        localResourceRoots: [vscode.Uri.joinPath(extensionUri, 'media')],
        retainContextWhenHidden: true,
      },
    );

    const webview = this.panel.webview;
    const nonce = getNonce();
    const scriptUri = webview.asWebviewUri(
      vscode.Uri.joinPath(extensionUri, 'media', 'main.js'),
    );
    const styleUri = webview.asWebviewUri(
      vscode.Uri.joinPath(extensionUri, 'media', 'panel.css'),
    );
    const csp = [
      "default-src 'none'",
      `img-src ${webview.cspSource} data:`,
      `script-src 'nonce-${nonce}'`,
      `style-src ${webview.cspSource} 'nonce-${nonce}'`,
    ].join('; ');

    webview.html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta http-equiv="Content-Security-Policy" content="${csp}" />
  <title>Shield DevTools</title>
  <link rel="stylesheet" href="${styleUri}" />
</head>
<body>
  <header id="header">
    <span id="node" class="mono">not attached</span>
    <span id="conn" class="pill pill-off">detached</span>
    <span id="sample-source" class="dim mono"></span>
  </header>
  <section class="metrics">
    <div class="chart-card">
      <div class="chart-head"><span>CPU %</span><span id="cpu-now" class="mono">--</span></div>
      <canvas id="cpu-canvas" width="560" height="120"></canvas>
    </div>
    <div class="chart-card">
      <div class="chart-head"><span>Memory</span><span id="rss-now" class="mono">--</span></div>
      <canvas id="rss-canvas" width="560" height="120"></canvas>
    </div>
  </section>
  <section class="services">
    <div class="section-head">
      <span>Lua services</span>
      <span id="services-note" class="dim">read-only face; hot reload stays in the console</span>
    </div>
    <table id="services-table">
      <thead>
        <tr><th>service</th><th>state</th><th>req</th><th>err</th><th>mem</th><th>script</th></tr>
      </thead>
      <tbody></tbody>
    </table>
  </section>
  <section class="flame">
    <div class="section-head">
      <span>Flame graph</span>
      <div class="log-controls">
        <select id="flame-service"><option value="">service...</option></select>
        <input id="flame-duration" type="number" min="500" max="60000" step="500" value="3000" />
        <span class="dim">ms</span>
        <button id="flame-start">Sample profile</button>
        <span id="flame-note" class="dim">entry point: engine /ops/profile sampler (needs http.profile_enabled + http.profile_token)</span>
      </div>
    </div>
    <div id="flame-view"></div>
  </section>
  <section class="logs">
    <div class="section-head">
      <span>Log stream</span>
      <div class="log-controls">
        <input id="log-filter" placeholder="filter substring" />
        <select id="log-level">
          <option value="all">all levels</option>
          <option value="debug">debug</option>
          <option value="info">info</option>
          <option value="warning">warning</option>
          <option value="error">error</option>
          <option value="fatal">fatal</option>
        </select>
        <input id="log-search" placeholder="search (highlight)" />
        <label class="wrap-toggle"><input id="log-wrap" type="checkbox" checked /> wrap</label>
        <button id="log-clear">clear</button>
      </div>
    </div>
    <div id="log-view" tabindex="0"></div>
  </section>
  <script nonce="${nonce}" src="${scriptUri}"></script>
</body>
</html>`;

    webview.onDidReceiveMessage(
      (message: ShieldPanelMessage) => {
        this.onDidReceive?.(message);
      },
      undefined,
      this.disposables,
    );

    this.panel.onDidDispose(() => {
      ShieldPanel.activePanel = undefined;
      this.dispose();
    });

    this.panel.iconPath = vscode.Uri.joinPath(
      extensionUri,
      'media',
      'shield.svg',
    );
  }

  reveal(): void {
    this.panel.reveal();
  }

  postStatus(status: SessionStatus, detail?: string): void {
    void this.panel.webview.postMessage({ type: 'status', status, detail });
  }

  postNode(node: string): void {
    void this.panel.webview.postMessage({ type: 'node', node });
  }

  postMetrics(sample: MetricsSample): void {
    void this.panel.webview.postMessage({ type: 'metrics', sample });
  }

  postLog(record: LogRecord): void {
    void this.panel.webview.postMessage({ type: 'log', record });
  }

  postServices(entries: LuaServiceEntry[]): void {
    void this.panel.webview.postMessage({ type: 'services', entries });
  }

  postProfileReport(report: ProfileResponse): void {
    void this.panel.webview.postMessage({ type: 'profileReport', report });
  }

  postProfileNote(note: string): void {
    void this.panel.webview.postMessage({ type: 'profileNote', note });
  }

  dispose(): void {
    ShieldPanel.activePanel = undefined;
    this.panel.dispose();
    while (this.disposables.length > 0) {
      this.disposables.pop()?.dispose();
    }
  }
}

function getNonce(): string {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789';
  let nonce = '';
  for (let i = 0; i < 32; i++) {
    nonce += chars[Math.floor(Math.random() * chars.length)];
  }
  return nonce;
}
