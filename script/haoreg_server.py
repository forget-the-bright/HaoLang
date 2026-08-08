#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HaoLang 本地包仓库服务端（对标 Maven 本地仓 / 简易私服）

数据根布局（与 file registry / hao mod 一致）::

    <root>/
      example.com/demo/utilpkg/
        1.0.0/
          haopkg.json
          utilpkg/...

hao 客户端兼容路径（tidy 已用，不变）::

    GET /{module}/versions.json          → ["1.0.0","1.1.0"]
    GET /{module}/{version}.zip          → 该版本目录打包

发现 / 浏览 API（分层；默认只返回当前层）::

    GET /                                → HTML：第一层目录
    GET /browse[/{prefix}]               → HTML：下钻浏览
    GET /health                          → {"ok":true}
    GET /api/v1/packages                 → 树：当前根层 dirs + modules
    GET /api/v1/packages?flat=1          → 兼容：全量平铺包列表
    GET /api/v1/packages/{prefix}        → 前缀层 / 模块详情 / 版本详情
    GET /api/v1/tree[/{prefix}]          → 同分层浏览（别名）

用法::

    python script/haoreg_server.py --root repo/RegisterRepo --port 8765
    set HAO_REGISTRY=http://127.0.0.1:8765
    hao mod tidy test/modsmoke/depshttp

可选鉴权：--token secret 时要求 Authorization: Bearer secret（对应 HAO_TOKEN）。
"""

from __future__ import annotations

import argparse
import html
import io
import json
import os
import re
import sys
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import parse_qs, unquote, urlparse


SEMVER_DIR = re.compile(r"^v?\d+\.\d+\.\d+$")


def norm_module(path: str) -> str:
    return path.strip("/").replace("\\", "/")


def is_version_dir(name: str) -> bool:
    return bool(SEMVER_DIR.match(name))


def _semver_key(v: str) -> Tuple[int, int, int]:
    s = v[1:] if v[:1] in "vV" else v
    parts = s.split(".")
    try:
        return int(parts[0]), int(parts[1]), int(parts[2])
    except (ValueError, IndexError):
        return 0, 0, 0


def scan_packages(root: Path) -> Dict[str, List[str]]:
    """module → sorted version strings（全量；给 flat / versions.json 用）。"""
    pkgs: Dict[str, List[str]] = {}
    if not root.is_dir():
        return pkgs
    for dirpath, dirnames, _filenames in os.walk(root):
        versions = [d for d in dirnames if is_version_dir(d)]
        if not versions:
            continue
        rel = Path(dirpath).relative_to(root).as_posix()
        if rel == ".":
            continue
        pkgs[rel] = sorted(versions, key=_semver_key)
        dirnames[:] = [d for d in dirnames if not is_version_dir(d)]
    return dict(sorted(pkgs.items()))


def has_packages_under(dir_path: Path) -> bool:
    """目录下是否存在「某层带 semver 子目录」的模块。"""
    if not dir_path.is_dir():
        return False
    for dirpath, dirnames, _ in os.walk(dir_path):
        if any(is_version_dir(d) for d in dirnames):
            return True
        dirnames[:] = [d for d in dirnames if not is_version_dir(d)]
    return False


def list_level(root: Path, prefix: str = "") -> Dict[str, Any]:
    """
    列出 prefix 下**一层**：
    - dirs：还可继续下钻的路径段
    - modules：直接子目录即为完整 module（其下是版本目录）
    """
    prefix = norm_module(prefix)
    base = root / prefix if prefix else root
    out: Dict[str, Any] = {
        "mode": "tree",
        "path": prefix,
        "parent": "/".join(prefix.split("/")[:-1]) if prefix else None,
        "dirs": [],
        "modules": [],
    }
    if not base.is_dir():
        return out

    for child in sorted(base.iterdir(), key=lambda p: p.name.lower()):
        if not child.is_dir() or is_version_dir(child.name):
            continue
        child_prefix = f"{prefix}/{child.name}" if prefix else child.name
        sub_dirs = [p for p in child.iterdir() if p.is_dir()]
        vers = sorted(
            (p.name for p in sub_dirs if is_version_dir(p.name)),
            key=_semver_key,
        )
        if vers:
            latest = vers[-1]
            meta = read_haopkg(child / latest)
            out["modules"].append(
                {
                    "name": child.name,
                    "module": child_prefix,
                    "versions": vers,
                    "latest": latest,
                    "description": meta.get("description"),
                }
            )
        elif has_packages_under(child):
            out["dirs"].append({"name": child.name, "path": child_prefix})
    return out


def resolve_browse(root: Path, rest: str) -> Tuple[str, Any]:
    """
    解析 /api/v1/packages/{rest}：
    返回 (kind, payload)：
      tree | module | version | missing
    """
    rest = norm_module(rest)
    if not rest:
        return "tree", list_level(root, "")

    pkgs = scan_packages(root)
    if rest in pkgs:
        vers = pkgs[rest]
        latest = vers[-1] if vers else None
        return "module", {
            "mode": "module",
            "module": rest,
            "versions": vers,
            "latest": latest,
            "urls": {
                "versions_json": f"/{rest}/versions.json",
                "zip_template": f"/{rest}/{{version}}.zip",
            },
        }

    parts = rest.rsplit("/", 1)
    if len(parts) == 2 and is_version_dir(parts[1]):
        mod, ver = parts
        if mod in pkgs and ver in pkgs[mod]:
            ver_dir = root / mod / ver
            meta = read_haopkg(ver_dir)
            files = sorted(
                p.relative_to(ver_dir).as_posix()
                for p in ver_dir.rglob("*")
                if p.is_file()
            )
            return "version", {
                "mode": "version",
                "module": mod,
                "version": ver,
                "haopkg": meta,
                "files": files,
                "zip": f"/{mod}/{ver}.zip",
            }
        return "missing", {"error": "无此模块或版本", "module": mod, "version": ver}

    # 前缀下钻
    level = list_level(root, rest)
    if level["dirs"] or level["modules"]:
        return "tree", level
    return "missing", {"error": "无此路径或模块", "path": rest}


def read_haopkg(ver_dir: Path) -> Dict[str, Any]:
    p = ver_dir / "haopkg.json"
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def zip_version_dir(ver_dir: Path) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(ver_dir.rglob("*")):
            if f.is_file():
                zf.write(f, f.relative_to(ver_dir).as_posix())
    return buf.getvalue()


def _css() -> str:
    return """
body{font-family:system-ui,sans-serif;max-width:920px;margin:2rem auto;padding:0 1rem;line-height:1.5;color:#1a1a1a}
code,pre{background:#f4f4f4;padding:.15em .4em;border-radius:4px;font-size:.92em}
pre{padding:1rem;overflow:auto}
a{color:#0b57d0;text-decoration:none} a:hover{text-decoration:underline}
.crumb{margin:.5rem 0 1.2rem;color:#555}
.crumb a{color:#555}
.card{border:1px solid #e0e0e0;border-radius:8px;padding:.75rem 1rem;margin:.4rem 0}
.card h3{margin:0 0 .35rem;font-size:1.05rem}
.meta{color:#666;font-size:.9rem}
.tag{display:inline-block;background:#eef3ff;color:#1a3a8a;padding:.1em .45em;border-radius:4px;font-size:.8rem;margin-right:.35rem}
"""


def render_browse_html(root: Path, prefix: str, host: str) -> str:
    prefix = norm_module(prefix)
    kind, payload = resolve_browse(root, prefix)
    title_path = prefix or "（根）"
    crumbs = ['<a href="/browse">根</a>']
    if prefix:
        acc: List[str] = []
        for seg in prefix.split("/"):
            acc.append(seg)
            p = "/".join(acc)
            crumbs.append(f'<a href="/browse/{html.escape(p)}">{html.escape(seg)}</a>')

    body = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>HaoReg · {html.escape(title_path)}</title>
<style>{_css()}</style></head><body>
<h1>HaoLang 包仓库</h1>
<p class="meta">数据根：<code>{html.escape(str(root))}</code> · 分层浏览（默认一层）</p>
<p class="crumb">{" / ".join(crumbs)}</p>
"""

    if kind == "tree":
        dirs = payload.get("dirs") or []
        mods = payload.get("modules") or []
        if not dirs and not mods:
            body += "<p>（此层为空）</p>"
        if dirs:
            body += "<h2>目录</h2>"
            for d in dirs:
                p = d["path"]
                body += (
                    f'<div class="card"><h3><span class="tag">dir</span>'
                    f'<a href="/browse/{html.escape(p)}"><code>{html.escape(d["name"])}</code></a></h3>'
                    f'<div class="meta">路径 <code>{html.escape(p)}</code></div></div>\n'
                )
        if mods:
            body += "<h2>模块</h2>"
            for m in mods:
                desc = m.get("description") or ""
                body += (
                    f'<div class="card"><h3><span class="tag">module</span>'
                    f'<a href="/browse/{html.escape(m["module"])}"><code>{html.escape(m["name"])}</code></a></h3>'
                    f'<div class="meta">versions: {html.escape(", ".join(m["versions"]))} · latest '
                    f'<code>{html.escape(m["latest"] or "")}</code>'
                    f'{(" · " + html.escape(str(desc))) if desc else ""}</div></div>\n'
                )
    elif kind == "module":
        mod = payload["module"]
        body += f'<h2>模块 <code>{html.escape(mod)}</code></h2><ul>'
        for v in payload["versions"]:
            body += (
                f'<li><a href="/browse/{html.escape(mod)}/{html.escape(v)}">'
                f'<code>{html.escape(v)}</code></a> · '
                f'<a href="/{html.escape(mod)}/{html.escape(v)}.zip">zip</a></li>\n'
            )
        body += (
            f'</ul><p class="meta"><a href="/{html.escape(mod)}/versions.json">'
            f'versions.json</a> · API '
            f'<a href="/api/v1/packages/{html.escape(mod)}">/api/v1/packages/{html.escape(mod)}</a></p>'
        )
    elif kind == "version":
        mod, ver = payload["module"], payload["version"]
        body += (
            f'<h2><code>{html.escape(mod)}</code> @ <code>{html.escape(ver)}</code></h2>'
            f'<p><a href="/{html.escape(mod)}/{html.escape(ver)}.zip">下载 zip</a></p>'
            f'<h3>haopkg.json</h3><pre>{html.escape(json.dumps(payload.get("haopkg") or {}, ensure_ascii=False, indent=2))}</pre>'
            f'<h3>文件</h3><ul>'
        )
        for f in payload.get("files") or []:
            body += f"<li><code>{html.escape(f)}</code></li>\n"
        body += "</ul>"
    else:
        body += f'<p>未找到：<code>{html.escape(prefix)}</code></p>'

    body += f"""
<hr>
<details><summary>客户端 / API</summary>
<pre>GET /{{module}}/versions.json
GET /{{module}}/{{version}}.zip
GET /api/v1/packages              → 树（第一层）
GET /api/v1/packages?flat=1       → 全量平铺（兼容）
GET /api/v1/packages/{{prefix}}   → 下钻 / 模块 / 版本

set HAO_REGISTRY=http://{html.escape(host)}
hao mod tidy .</pre>
</details>
</body></html>"""
    return body


class HaoRegHandler(BaseHTTPRequestHandler):
    server_version = "HaoReg/0.48"

    root: Path = Path(".")
    token: Optional[str] = None

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _check_auth(self) -> bool:
        if not self.token:
            return True
        auth = self.headers.get("Authorization", "")
        if auth == "Bearer " + self.token:
            return True
        self._json(401, {"error": "需要 Authorization: Bearer <token>"})
        return False

    def _json(self, code: int, obj: Any) -> None:
        data = json.dumps(obj, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def _bytes(self, code: int, data: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def _text(self, code: int, text: str, content_type: str = "text/plain; charset=utf-8") -> None:
        self._bytes(code, text.encode("utf-8"), content_type)

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        if not self._check_auth():
            return
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        qs = parse_qs(parsed.query)
        if path != "/" and path.endswith("/"):
            path = path[:-1]

        if path in ("", "/"):
            self._text(
                200,
                render_browse_html(self.root, "", self.headers.get("Host", "127.0.0.1")),
                "text/html; charset=utf-8",
            )
            return
        if path == "/browse" or path.startswith("/browse/"):
            prefix = "" if path == "/browse" else path[len("/browse/") :]
            self._text(
                200,
                render_browse_html(self.root, prefix, self.headers.get("Host", "127.0.0.1")),
                "text/html; charset=utf-8",
            )
            return
        if path == "/health":
            pkgs = scan_packages(self.root)
            self._json(
                200,
                {"ok": True, "root": str(self.root), "modules": len(pkgs), "server": self.server_version},
            )
            return
        if path == "/api/v1/tree":
            self._json(200, list_level(self.root, ""))
            return
        if path.startswith("/api/v1/tree/"):
            self._api_browse(path[len("/api/v1/tree/") :])
            return
        if path == "/api/v1/packages":
            if qs.get("flat", ["0"])[0] in ("1", "true", "yes"):
                self._api_list_packages_flat()
            else:
                self._json(200, list_level(self.root, ""))
            return
        if path.startswith("/api/v1/packages/"):
            self._api_browse(path[len("/api/v1/packages/") :])
            return

        # hao 客户端：/{module}/versions.json 或 /{module}/{version}.zip
        if path.endswith("/versions.json"):
            module = norm_module(path[: -len("/versions.json")])
            self._hao_versions(module)
            return
        if path.endswith(".zip"):
            body = path[: -len(".zip")]
            parts = norm_module(body).rsplit("/", 1)
            if len(parts) != 2 or not is_version_dir(parts[1]):
                self._json(400, {"error": "zip 路径须为 /{module}/{version}.zip"})
                return
            self._hao_zip(parts[0], parts[1])
            return

        self._json(404, {"error": "未找到", "path": path})

    def _api_list_packages_flat(self) -> None:
        pkgs = scan_packages(self.root)
        items = []
        for mod, vers in pkgs.items():
            latest = vers[-1] if vers else None
            meta = read_haopkg(self.root / mod / latest) if latest else {}
            items.append(
                {
                    "module": mod,
                    "versions": vers,
                    "latest": latest,
                    "description": meta.get("description"),
                }
            )
        self._json(200, {"mode": "flat", "count": len(items), "packages": items})

    def _api_browse(self, rest: str) -> None:
        kind, payload = resolve_browse(self.root, rest)
        if kind == "missing":
            self._json(404, payload)
            return
        # 兼容旧客户端：模块详情不强制带 mode（仍带上）
        self._json(200, payload)

    def _hao_versions(self, module: str) -> None:
        pkgs = scan_packages(self.root)
        if module not in pkgs:
            self._json(404, {"error": "无此模块", "module": module})
            return
        data = json.dumps(pkgs[module], ensure_ascii=False).encode("utf-8")
        self._bytes(200, data, "application/json; charset=utf-8")

    def _hao_zip(self, module: str, version: str) -> None:
        ver_dir = self.root / module / version
        if not ver_dir.is_dir():
            self._json(404, {"error": "无此版本", "module": module, "version": version})
            return
        try:
            raw = zip_version_dir(ver_dir)
        except OSError as e:
            self._json(500, {"error": f"打包失败: {e}"})
            return
        self._bytes(200, raw, "application/zip")


def main() -> int:
    ap = argparse.ArgumentParser(description="HaoLang 本地包仓库服务端（分层浏览）")
    ap.add_argument(
        "--root",
        default="repo/RegisterRepo",
        help="仓库数据根（默认 repo/RegisterRepo）",
    )
    ap.add_argument("--host", default="127.0.0.1", help="监听地址")
    ap.add_argument("--port", type=int, default=8765, help="端口")
    ap.add_argument(
        "--token",
        default=os.environ.get("HAOREG_TOKEN", ""),
        help="可选 Bearer token（也可用环境变量 HAOREG_TOKEN）",
    )
    args = ap.parse_args()
    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"错误: 数据根不存在: {root}", file=sys.stderr)
        return 1

    pkgs = scan_packages(root)
    level0 = list_level(root, "")
    token = args.token or None
    HaoRegHandler.root = root
    HaoRegHandler.token = token

    httpd = ThreadingHTTPServer((args.host, args.port), HaoRegHandler)
    base = f"http://{args.host}:{args.port}"
    print(f"HaoReg 已启动: {base}")
    print(f"  数据根: {root}")
    print(f"  模块数: {len(pkgs)} · 根层目录 {len(level0['dirs'])} · 根层模块 {len(level0['modules'])}")
    if token:
        print("  鉴权: 需要 Authorization: Bearer <token>")
    print(f"  浏览:  {base}/  或  {base}/browse")
    print(f"  树 API: {base}/api/v1/packages  （全量平铺加 ?flat=1）")
    print(f"  客户端: set HAO_REGISTRY={base}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
