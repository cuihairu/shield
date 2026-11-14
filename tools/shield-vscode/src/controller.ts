// [SHIELD_VSCODE] Session controller: owns the devcore ShieldSession for the
// workspace, wires its events into the status bar and the panel, and owns
// the attach/detach command bodies. Attach is explicit (never auto-starts).

import * as vscode from 'vscode';

import {
  ShieldSession,
  type LogRecord,
  type LuaServiceEntry,
  type MetricsSample,
  type SessionStatus,
} from '@shield/shield-devcore';

import {
  ShieldPanel,
  type ProfilePanelMessage,
  type ShieldPanelMessage,
} from './panel.js';
import { ShieldStatusBar } from './statusBar.js';

export class ShieldController implements vscode.Disposable {
  private readonly statusBar: ShieldStatusBar;
  private session: ShieldSession | null = null;

  constructor(private readonly context: vscode.ExtensionContext) {
    this.statusBar = new ShieldStatusBar();
  }

  get isAttached(): boolean {
    return this.session !== null;
  }

  get currentStatus(): SessionStatus {
    return this.session?.status ?? 'detached';
  }

  async attach(): Promise<void> {
    const config = vscode.workspace.getConfiguration('shield');
    const opsUrl = config.get<string>('opsUrl', 'http://127.0.0.1:8080');
    const opsToken = config.get<string>('opsToken', '');
    const profileToken = config.get<string>('profileToken', '');
    const logFile = config.get<string>('logFile', '');
    const procPid = config.get<number>('procPid', 0);
    const metricsIntervalMs = config.get<number>('metricsIntervalMs', 2000);

    // Detach any previous session first (attach is idempotent-ish: restart).
    await this.detach();

    const session = new ShieldSession({
      ops:
        opsUrl && opsUrl.length > 0
          ? {
              baseUrl: opsUrl,
              token: opsToken || undefined,
              profileToken: profileToken || undefined,
              timeoutMs: 3000,
            }
          : undefined,
      logFile:
        logFile && logFile.length > 0 ? { path: logFile, intervalMs: 500 } : undefined,
      proc: procPid > 0 ? { pid: procPid, intervalMs: 1000 } : undefined,
      workspaceRoots: () => this.scriptRoots(),
      opsPollMs: metricsIntervalMs,
    });

    session.on('status', (status: SessionStatus) => {
      this.statusBar.setStatus(status);
      this.panel()?.postStatus(status);
    });
    session.on('metrics', (sample: MetricsSample) => {
      this.statusBar.setSample(sample);
      this.panel()?.postMetrics(sample);
    });
    session.on('log', (record: LogRecord) => {
      this.panel()?.postLog(record);
    });
    session.on('services', (entries: LuaServiceEntry[]) => {
      this.panel()?.postServices(entries);
    });
    session.on('error', (err: Error) => {
      // Transient poll failures are normal while a node restarts; surface
      // them once in the status bar (degraded) without spamming dialogs.
      void err;
    });

    this.session = session;
    session.start();
    this.statusBar.setStatus(session.status);

    // Open the panel so curves/logs are visible immediately.
    await this.openPanel();

    const faces: string[] = [];
    if (opsUrl) {
      faces.push(`ops ${opsUrl}`);
    }
    if (logFile) {
      faces.push(`log ${logFile}`);
    }
    if (procPid > 0) {
      faces.push(`proc ${procPid}`);
    }
    void vscode.window.showInformationMessage(
      `Shield: attaching to ${faces.join(', ') || 'nothing configured (set shield.*)'}`,
    );
  }

  async detach(): Promise<void> {
    if (this.session) {
      await this.session.detach();
      this.session = null;
    }
    this.statusBar.setStatus('detached');
  }

  async openPanel(): Promise<void> {
    const panel = ShieldPanel.createOrShow(
      this.context.extensionUri,
      (message: ShieldPanelMessage | ProfilePanelMessage) =>
        void this.handlePanelMessage(message),
    );
    const session = this.session;
    panel.postNode(this.nodeLabel());
    panel.postStatus(session?.status ?? 'detached');
  }

  async handlePanelMessage(message: ShieldPanelMessage | ProfilePanelMessage): Promise<void> {
    if (message.type === 'openScript') {
      try {
        const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(message.path));
        await vscode.window.showTextDocument(doc, { preview: true });
      } catch {
        void vscode.window.showErrorMessage(
          `Shield: cannot open script ${message.path} (not in this workspace?)`,
        );
      }
      return;
    }
    if (message.type === 'profileStart') {
      await this.runProfile(message);
    }
  }

  /// Flame-graph entry point: drive the engine's /ops/profile sampler
  /// (start -> wait duration -> report). Errors surface as a panel note,
  /// not a modal: the sampler is opt-in and commonly gated (401/404/429).
  private async runProfile(message: ProfilePanelMessage): Promise<void> {
    const panel = this.panel();
    const ops = this.session?.ops;
    if (!panel || !ops) {
      panel?.postProfileNote('attach to a node first (no ops face)');
      return;
    }
    try {
      const report = await ops.sampleOnce(message.service, message.durationMs);
      panel.postProfileReport(report);
    } catch (err) {
      const status = (err as { status?: number }).status;
      const hint =
        status === 404
          ? 'profile endpoint not registered (enable http.profile_enabled=true and set http.profile_token)'
          : status === 401
            ? 'profile token mismatch (set shield.profileToken to http.profile_token)'
            : status === 429
              ? 'profile start cooldown active'
              : err instanceof Error
                ? err.message
                : String(err);
      panel.postProfileNote(`profile failed: ${hint}`);
    }
  }

  /// Script paths recorded by the engine are config-relative; the anchors we
  /// can guess from the IDE are the workspace folders (usually the repo root
  /// whose config lives under <root>/<config-dir>). The recorded value is
  /// resolved against every root, first hit wins.
  private scriptRoots(): string[] {
    const roots: string[] = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
      roots.push(folder.uri.fsPath);
      roots.push(vscode.Uri.joinPath(folder.uri, 'config').fsPath);
      roots.push(vscode.Uri.joinPath(folder.uri, 'examples', 'hello_world', 'config').fsPath);
    }
    return roots;
  }

  private nodeLabel(): string {
    const config = vscode.workspace.getConfiguration('shield');
    const parts: string[] = [];
    const opsUrl = config.get<string>('opsUrl', '');
    if (opsUrl) {
      parts.push(opsUrl);
    }
    const logFile = config.get<string>('logFile', '');
    if (logFile) {
      parts.push(logFile);
    }
    return parts.join(' | ') || 'not configured';
  }

  private panel(): ShieldPanel | undefined {
    return ShieldPanel.current;
  }

  dispose(): void {
    void this.detach();
    this.statusBar.dispose();
  }
}
