import * as vscode from "vscode";
import * as fs from "fs";
import * as path from "path";
import { execFileSync } from "child_process";

function isExecutable(p: string): boolean {
  try {
    fs.accessSync(p, fs.constants.F_OK);
    return true;
  } catch {
    return false;
  }
}

function candidateNames(): string[] {
  return process.platform === "win32" ? ["hao.exe", "hao"] : ["hao"];
}

function walkUp(start: string, rel: string[]): string | undefined {
  let cur = path.resolve(start);
  for (let i = 0; i < 12; i++) {
    for (const parts of rel) {
      const c = path.join(cur, ...parts.split(/[/\\]/));
      if (isExecutable(c)) {
        return c;
      }
    }
    const parent = path.dirname(cur);
    if (parent === cur) {
      break;
    }
    cur = parent;
  }
  return undefined;
}

function expandConfiguredPath(
  raw: string,
  workspaceFolder?: vscode.WorkspaceFolder
): string {
  let p = raw.trim();
  const folder =
    workspaceFolder?.uri.fsPath ??
    vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (folder) {
    p = p.replace(/\$\{workspaceFolder\}/gi, folder);
    if (!path.isAbsolute(p)) {
      p = path.resolve(folder, p);
    }
  }
  return path.normalize(p);
}

/** 解析 hao 可执行路径：设置 → 工作区常见产物 → PATH。 */
export async function resolveHaoExecutable(
  workspaceFolder?: vscode.WorkspaceFolder
): Promise<string | undefined> {
  const cfg = vscode.workspace.getConfiguration("haolang");
  const configuredRaw = (cfg.get<string>("executablePath") || "").trim();
  if (configuredRaw) {
    const configured = expandConfiguredPath(configuredRaw, workspaceFolder);
    if (isExecutable(configured)) {
      return configured;
    }
    vscode.window.showWarningMessage(
      `haolang.executablePath 无效: ${configured}`
    );
  }

  const roots: string[] = [];
  if (workspaceFolder) {
    roots.push(workspaceFolder.uri.fsPath);
  }
  for (const f of vscode.workspace.workspaceFolders ?? []) {
    if (!roots.includes(f.uri.fsPath)) {
      roots.push(f.uri.fsPath);
    }
  }

  const relCandidates = [
    "output/hao.exe",
    "output/hao",
    "target/hao.exe",
    "target/hao",
    "bin/hao.exe",
    "bin/hao",
    "hao.exe",
    "hao",
  ];

  for (const root of roots) {
    for (const rel of relCandidates) {
      const full = path.join(root, ...rel.split("/"));
      if (isExecutable(full)) {
        return full;
      }
    }
    const found = walkUp(root, relCandidates);
    if (found) {
      return found;
    }
  }

  // PATH
  for (const name of candidateNames()) {
    try {
      if (process.platform === "win32") {
        const out = execFileSync("where.exe", [name], {
          encoding: "utf8",
          windowsHide: true,
        });
        const first = out
          .split(/\r?\n/)
          .map((s) => s.trim())
          .find((s) => s.length > 0);
        if (first && isExecutable(first)) {
          return first;
        }
      } else {
        const out = execFileSync("which", [name], { encoding: "utf8" });
        const first = out.trim();
        if (first && isExecutable(first)) {
          return first;
        }
      }
    } catch {
      // continue
    }
  }
  return undefined;
}

export function stdlibEnv(): NodeJS.ProcessEnv {
  const env = { ...process.env };
  const std = (
    vscode.workspace.getConfiguration("haolang").get<string>("stdlibPath") ||
    ""
  ).trim();
  if (std) {
    env.HAO_STDLIB = std;
  }
  return env;
}
