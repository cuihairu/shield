// [SHIELD_VSCODE] Status bar face: attach state plus the latest CPU/RSS
// reading at a glance. One item, click opens the panel.

import * as vscode from 'vscode';
import type { MetricsSample, SessionStatus } from '@shield/shield-devcore';

export class ShieldStatusBar {
  private item: vscode.StatusBarItem;
  private lastSample: MetricsSample | null = null;
  private status: SessionStatus = 'detached';

  constructor() {
    this.item = vscode.window.createStatusBarItem(
      vscode.StatusBarAlignment.Right,
      90,
    );
    this.item.name = 'Shield DevTools';
    this.item.command = 'shield.openPanel';
    this.render();
    this.item.show();
  }

  setStatus(status: SessionStatus): void {
    this.status = status;
    if (status === 'detached') {
      this.lastSample = null;
    }
    this.render();
  }

  setSample(sample: MetricsSample): void {
    this.lastSample = sample;
    this.render();
  }

  private render(): void {
    const cpu =
      this.lastSample?.cpuPercent != null
        ? `${this.lastSample.cpuPercent.toFixed(1)}%`
        : '--';
    const rssMiB =
      this.lastSample?.rssBytes != null
        ? `${(this.lastSample.rssBytes / (1024 * 1024)).toFixed(0)}MiB`
        : '--';
    switch (this.status) {
      case 'attached':
        this.item.text = `$(shield) shield ${cpu} ${rssMiB}`;
        this.item.backgroundColor = undefined;
        this.item.tooltip = `Shield node attached (source: ${this.lastSample?.source ?? 'n/a'}) - click to open DevTools`;
        break;
      case 'degraded':
        this.item.text = `$(shield) shield degraded`;
        this.item.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.warningBackground',
        );
        this.item.tooltip = 'Shield node degraded (a data source is unreachable)';
        break;
      case 'detaching':
        this.item.text = `$(shield) shield detaching...`;
        this.item.backgroundColor = undefined;
        this.item.tooltip = 'Shield node detaching';
        break;
      default:
        this.item.text = `$(shield) shield`;
        this.item.backgroundColor = undefined;
        this.item.tooltip = 'Shield node detached - click to open DevTools and attach';
        break;
    }
  }

  dispose(): void {
    this.item.dispose();
  }
}
