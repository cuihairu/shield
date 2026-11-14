// [SHIELD_VSCODE] Extension entry: register the attach/detach/panel
// commands. Activation only happens through the commands (no auto-start).

import * as vscode from 'vscode';

import { ShieldController } from './controller.js';

let controller: ShieldController | undefined;

export function activate(_context: vscode.ExtensionContext): void {
  controller = new ShieldController(_context);
  _context.subscriptions.push(controller);

  _context.subscriptions.push(
    vscode.commands.registerCommand('shield.attach', async () => {
      await controller?.attach();
    }),
    vscode.commands.registerCommand('shield.detach', async () => {
      await controller?.detach();
      void vscode.window.showInformationMessage('Shield: detached');
    }),
    vscode.commands.registerCommand('shield.openPanel', async () => {
      await controller?.openPanel();
    }),
  );
}

export function deactivate(): void {
  controller?.dispose();
  controller = undefined;
}
