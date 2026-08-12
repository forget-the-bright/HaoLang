import * as vscode from "vscode";
import { resolveHaoExecutable, stdlibEnv } from "./haoPath";

let sharedTerminal: vscode.Terminal | undefined;

function getTerminal(): vscode.Terminal {
  if (!sharedTerminal || sharedTerminal.exitStatus !== undefined) {
    sharedTerminal = vscode.window.createTerminal({
      name: "HaoLang",
      env: stdlibEnv(),
    });
  }
  return sharedTerminal;
}

async function ensureHao(
  folder?: vscode.WorkspaceFolder
): Promise<string | undefined> {
  const hao = await resolveHaoExecutable(folder);
  if (!hao) {
    const pick = await vscode.window.showErrorMessage(
      "未找到 hao 可执行文件。请安装工具链或在设置中配置 haolang.executablePath。",
      "打开设置"
    );
    if (pick === "打开设置") {
      await vscode.commands.executeCommand(
        "workbench.action.openSettings",
        "haolang.executablePath"
      );
    }
    return undefined;
  }
  return hao;
}

function quoteArg(a: string): string {
  if (!/[ \t"]/u.test(a)) {
    return a;
  }
  return `"${a.replace(/"/g, '\\"')}"`;
}

async function runInTerminal(
  args: string[],
  cwd?: string
): Promise<void> {
  const folder =
    vscode.workspace.workspaceFolders?.[0] ??
    (vscode.window.activeTextEditor
      ? vscode.workspace.getWorkspaceFolder(
          vscode.window.activeTextEditor.document.uri
        )
      : undefined);
  const hao = await ensureHao(folder ?? undefined);
  if (!hao) {
    return;
  }
  const term = getTerminal();
  if (cwd) {
    term.sendText(`cd ${quoteArg(cwd)}`);
  }
  const line = [quoteArg(hao), ...args.map(quoteArg)].join(" ");
  term.show(true);
  term.sendText(line);
}

function activeHaoPath(): string | undefined {
  const ed = vscode.window.activeTextEditor;
  if (!ed || ed.document.languageId !== "haolang") {
    return undefined;
  }
  return ed.document.uri.fsPath;
}

function workspaceRoot(): string | undefined {
  const ed = vscode.window.activeTextEditor;
  if (ed) {
    const f = vscode.workspace.getWorkspaceFolder(ed.document.uri);
    if (f) {
      return f.uri.fsPath;
    }
  }
  return vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
}

export function registerCommands(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.commands.registerCommand("haolang.run", async () => {
      const file = activeHaoPath();
      if (!file) {
        vscode.window.showWarningMessage("请先打开一个 .hao 文件");
        return;
      }
      await runInTerminal(["run", file], workspaceRoot());
    }),
    vscode.commands.registerCommand("haolang.build", async () => {
      const file = activeHaoPath();
      const root = workspaceRoot();
      if (file) {
        await runInTerminal(["build", file], root);
      } else if (root) {
        await runInTerminal(["build", "."], root);
      } else {
        vscode.window.showWarningMessage("无工作区或 .hao 文件可构建");
      }
    }),
    vscode.commands.registerCommand("haolang.test", async () => {
      const root = workspaceRoot();
      const file = activeHaoPath();
      if (file) {
        await runInTerminal(["test", file], root);
      } else if (root) {
        await runInTerminal(["test", "."], root);
      } else {
        vscode.window.showWarningMessage("无工作区可测试");
      }
    }),
    vscode.commands.registerCommand("haolang.fmt", async () => {
      const ed = vscode.window.activeTextEditor;
      if (!ed || ed.document.languageId !== "haolang") {
        vscode.window.showWarningMessage("请先打开一个 .hao 文件");
        return;
      }
      if (ed.document.isDirty) {
        await ed.document.save();
      }
      await runInTerminal(["fmt", "-w", ed.document.uri.fsPath], workspaceRoot());
      // 终端写回后刷新编辑器
      setTimeout(() => {
        void vscode.commands.executeCommand("workbench.action.files.revert");
      }, 400);
    }),
    vscode.commands.registerCommand("haolang.clean", async () => {
      await runInTerminal(["clean"], workspaceRoot());
    }),
    vscode.commands.registerCommand("haolang.modTidy", async () => {
      await runInTerminal(["mod", "tidy"], workspaceRoot());
    }),
    vscode.commands.registerCommand("haolang.version", async () => {
      await runInTerminal(["version"], workspaceRoot());
    })
  );
}

export async function createHaoTaskProvider(): Promise<vscode.Disposable> {
  return vscode.tasks.registerTaskProvider("hao", {
    provideTasks: async () => {
      const folder = vscode.workspace.workspaceFolders?.[0];
      if (!folder) {
        return [];
      }
      const hao = await resolveHaoExecutable(folder);
      if (!hao) {
        return [];
      }
      const mk = (name: string, args: string[]): vscode.Task => {
        const def: vscode.TaskDefinition = { type: "hao", command: args[0], args: args.slice(1) };
        const task = new vscode.Task(
          def,
          folder,
          name,
          "hao",
          new vscode.ProcessExecution(hao, args, {
            cwd: folder.uri.fsPath,
            env: Object.fromEntries(
              Object.entries(stdlibEnv()).filter(
                (e): e is [string, string] => typeof e[1] === "string"
              )
            ),
          }),
          ["$hao"]
        );
        task.group = args[0] === "build" ? vscode.TaskGroup.Build : undefined;
        return task;
      };
      return [
        mk("hao: build", ["build", "."]),
        mk("hao: test", ["test", "."]),
        mk("hao: clean", ["clean"]),
        mk("hao: mod tidy", ["mod", "tidy"]),
        mk("hao: version", ["version"]),
      ];
    },
    resolveTask: async (task) => task,
  });
}
