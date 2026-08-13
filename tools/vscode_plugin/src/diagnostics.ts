import * as vscode from "vscode";
import { spawn } from "child_process";
import * as path from "path";
import { resolveHaoExecutable, stdlibEnv } from "./haoPath";

const DIAG_RE =
  /^(.+?):(\d+):(\d+):\s*(错误|警告|error|warning):\s*(.+)$/i;

export function parseHaoDiagnostics(
  output: string,
  fallbackUri?: vscode.Uri
): vscode.Diagnostic[] {
  const out: vscode.Diagnostic[] = [];
  for (const line of output.split(/\r?\n/)) {
    const m = DIAG_RE.exec(line.trim());
    if (!m) continue;
    const file = m[1];
    const lineNo = Math.max(0, parseInt(m[2], 10) - 1);
    const col = Math.max(0, parseInt(m[3], 10) - 1);
    const sev =
      /警告|warning/i.test(m[4])
        ? vscode.DiagnosticSeverity.Warning
        : vscode.DiagnosticSeverity.Error;
    const msg = m[5].trim();
    const range = new vscode.Range(lineNo, col, lineNo, Math.max(col + 1, col));
    const d = new vscode.Diagnostic(range, msg, sev);
    d.source = "hao";
    // 仅收集；按 URI 分组在调用方
    (d as vscode.Diagnostic & { _haoFile?: string })._haoFile = file;
    void fallbackUri;
    out.push(d);
  }
  return out;
}

export function registerDiagnostics(
  context: vscode.ExtensionContext
): vscode.DiagnosticCollection {
  const collection = vscode.languages.createDiagnosticCollection("haolang");
  context.subscriptions.push(collection);

  let timer: NodeJS.Timeout | undefined;
  const schedule = (doc: vscode.TextDocument) => {
    if (doc.languageId !== "haolang" && !doc.fileName.endsWith(".hao")) {
      return;
    }
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      void refreshDiagnostics(doc, collection);
    }, 400);
  };

  context.subscriptions.push(
    vscode.workspace.onDidSaveTextDocument(schedule),
    vscode.workspace.onDidOpenTextDocument(schedule),
    vscode.window.onDidChangeActiveTextEditor((e) => {
      if (e?.document) schedule(e.document);
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("haolang.diagnose", async () => {
      const doc = vscode.window.activeTextEditor?.document;
      if (!doc) {
        vscode.window.showWarningMessage("没有活动的 .hao 文件");
        return;
      }
      await refreshDiagnostics(doc, collection);
    })
  );

  const active = vscode.window.activeTextEditor?.document;
  if (active) schedule(active);

  return collection;
}

async function refreshDiagnostics(
  doc: vscode.TextDocument,
  collection: vscode.DiagnosticCollection
): Promise<void> {
  const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
  const hao = await resolveHaoExecutable(folder);
  if (!hao) {
    collection.set(doc.uri, [
      new vscode.Diagnostic(
        new vscode.Range(0, 0, 0, 1),
        "未找到 hao 可执行文件（设置 haolang.executablePath）",
        vscode.DiagnosticSeverity.Warning
      ),
    ]);
    return;
  }

  const cwd = folder?.uri.fsPath || path.dirname(doc.uri.fsPath);
  const args = ["build", doc.uri.fsPath, "-o", path.join(cwd, ".hao_diag_tmp.exe")];
  const text = await runHaoCapture(hao, args, cwd);
  const parsed = parseHaoDiagnostics(text, doc.uri);
  const byUri = new Map<string, vscode.Diagnostic[]>();
  for (const d of parsed) {
    const f = (d as vscode.Diagnostic & { _haoFile?: string })._haoFile || doc.uri.fsPath;
    const uri = path.isAbsolute(f)
      ? vscode.Uri.file(f)
      : vscode.Uri.file(path.resolve(cwd, f));
    const list = byUri.get(uri.toString()) || [];
    list.push(d);
    byUri.set(uri.toString(), list);
  }
  collection.delete(doc.uri);
  if (byUri.size === 0) {
    collection.set(doc.uri, []);
  } else {
    for (const [k, diags] of byUri) {
      collection.set(vscode.Uri.parse(k), diags);
    }
  }
}

function runHaoCapture(
  hao: string,
  args: string[],
  cwd: string
): Promise<string> {
  return new Promise((resolve) => {
    const child = spawn(hao, args, {
      cwd,
      env: stdlibEnv(),
      windowsHide: true,
    });
    let out = "";
    child.stdout.on("data", (b) => {
      out += b.toString();
    });
    child.stderr.on("data", (b) => {
      out += b.toString();
    });
    child.on("close", () => resolve(out));
    child.on("error", (e) => resolve(String(e)));
  });
}
