import * as vscode from "vscode";
import { execFile } from "child_process";
import { resolveHaoExecutable } from "./haoPath";

export class HaoStatusBar {
  private item: vscode.StatusBarItem;
  private disposed = false;

  constructor() {
    this.item = vscode.window.createStatusBarItem(
      vscode.StatusBarAlignment.Left,
      100
    );
    this.item.command = "haolang.version";
    this.item.tooltip = "HaoLang：点击查看 hao version";
    this.item.text = "$(loading~spin) HaoLang";
    this.item.show();
  }

  async refresh(folder?: vscode.WorkspaceFolder): Promise<void> {
    if (this.disposed) {
      return;
    }
    const hao = await resolveHaoExecutable(folder);
    if (!hao) {
      this.item.text = "$(warning) HaoLang: 未找到 hao";
      this.item.backgroundColor = new vscode.ThemeColor(
        "statusBarItem.warningBackground"
      );
      return;
    }
    this.item.backgroundColor = undefined;
    execFile(
      hao,
      ["version"],
      { encoding: "utf8", windowsHide: true, timeout: 5000 },
      (err, stdout) => {
        if (this.disposed) {
          return;
        }
        if (err) {
          this.item.text = "$(error) HaoLang";
          return;
        }
        const ver = (stdout || "").trim().split(/\r?\n/)[0] || "hao";
        this.item.text = `$(check) ${ver}`;
      }
    );
  }

  dispose(): void {
    this.disposed = true;
    this.item.dispose();
  }
}
