import * as vscode from "vscode";
import { execFile } from "child_process";
import { promisify } from "util";
import { resolveHaoExecutable, stdlibEnv } from "./haoPath";

const execFileAsync = promisify(execFile);

export class HaoFormattingProvider
  implements vscode.DocumentFormattingEditProvider
{
  async provideDocumentFormattingEdits(
    document: vscode.TextDocument
  ): Promise<vscode.TextEdit[]> {
    if (document.languageId !== "haolang") {
      return [];
    }
    const folder = vscode.workspace.getWorkspaceFolder(document.uri);
    const hao = await resolveHaoExecutable(folder);
    if (!hao) {
      vscode.window.showErrorMessage("格式化失败：未找到 hao");
      return [];
    }
    const original = document.getText();
    try {
      const { stdout } = await execFileAsync(hao, ["fmt", document.uri.fsPath], {
        encoding: "utf8",
        env: stdlibEnv(),
        maxBuffer: 16 * 1024 * 1024,
        windowsHide: true,
      });
      // hao fmt 无 -w 时：若 stdout 含全文则用；否则读文件后对比
      let formatted = stdout;
      if (!formatted || formatted.length === 0) {
        // 回退：-w 写临时不可用；调用 fmt 再读盘不合适。尝试无参数输出。
        return [];
      }
      // 某些版本可能把诊断打到 stderr、正文到 stdout；若 stdout 不像源码，放弃
      if (formatted === original) {
        return [];
      }
      // 若 stdout 只是日志，检测是否仍以 package/import/注释开头等
      const fullRange = new vscode.Range(
        document.positionAt(0),
        document.positionAt(original.length)
      );
      return [vscode.TextEdit.replace(fullRange, formatted)];
    } catch (e: unknown) {
      const msg = e instanceof Error ? e.message : String(e);
      vscode.window.showErrorMessage(`hao fmt 失败: ${msg}`);
      return [];
    }
  }
}

export function registerFormatting(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider(
      { language: "haolang", scheme: "file" },
      new HaoFormattingProvider()
    )
  );

  context.subscriptions.push(
    vscode.workspace.onWillSaveTextDocument(async (e) => {
      if (e.document.languageId !== "haolang") {
        return;
      }
      const onSave = vscode.workspace
        .getConfiguration("haolang")
        .get<boolean>("format.enableOnSave");
      if (!onSave) {
        return;
      }
      e.waitUntil(
        (async () => {
          const folder = vscode.workspace.getWorkspaceFolder(e.document.uri);
          const hao = await resolveHaoExecutable(folder);
          if (!hao) {
            return [] as vscode.TextEdit[];
          }
          try {
            await execFileAsync(hao, ["fmt", "-w", e.document.uri.fsPath], {
              encoding: "utf8",
              env: stdlibEnv(),
              windowsHide: true,
            });
          } catch (err: unknown) {
            const msg = err instanceof Error ? err.message : String(err);
            vscode.window.showErrorMessage(`保存时 fmt 失败: ${msg}`);
          }
          return [] as vscode.TextEdit[];
        })()
      );
    })
  );
}
