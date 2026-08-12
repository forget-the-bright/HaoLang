import * as vscode from "vscode";
import { registerCommands, createHaoTaskProvider } from "./commands";
import { registerFormatting } from "./formatting";
import { HaoStatusBar } from "./statusBar";

export async function activate(
  context: vscode.ExtensionContext
): Promise<void> {
  registerCommands(context);
  registerFormatting(context);
  context.subscriptions.push(await createHaoTaskProvider());

  const status = new HaoStatusBar();
  context.subscriptions.push(status);
  const folder = vscode.workspace.workspaceFolders?.[0];
  void status.refresh(folder);

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("haolang")) {
        void status.refresh(vscode.workspace.workspaceFolders?.[0]);
      }
    })
  );
}

export function deactivate(): void {
  // no-op
}
