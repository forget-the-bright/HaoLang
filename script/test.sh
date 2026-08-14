#!/usr/bin/env bash
# ============================================================
#  HaoLang 全量测试运行器（多文件套件 + 语法解析）
# ------------------------------------------------------------
#  用法：
#      bash script/test.sh                 # 增量：源未变直接运行已编译 suite.exe
#      bash script/test.sh --rebuild-all   # 强制全量重编
#
#  测试产物规范（记忆文档规则 7）：
#      一律写入 target/test/；禁止往 target/ 根乱扔文件。
#      跑完清空临时子目录，仅保留可选增量缓存 suite.exe。
# ============================================================
set -u
cd "$(dirname "$0")/.." || exit 1

HAO=./output/hao.exe
TESTROOT=target/test
CACHE="$TESTROOT/cache"
OUTDIR="$TESTROOT/out"
FORCE=0
[[ "${1:-}" == "--rebuild-all" ]] && FORCE=1

mkdir -p "$CACHE"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

SUITE=suite

echo "== 编译多文件套件（增量）=="
exe="$CACHE/suite.exe"
rebuild=0
if [[ $FORCE -eq 1 || ! -f "$exe" ]]; then
  rebuild=1
else
  while IFS= read -r -d '' f; do
    if [[ "$f" -nt "$exe" ]]; then rebuild=1; break; fi
  done < <(find "test/$SUITE" -name '*.hao' -print0 2>/dev/null)
  for f in "$HAO" stdlib/libhaort.a VERSION; do
    if [[ -f "$f" && "$f" -nt "$exe" ]]; then rebuild=1; break; fi
  done
fi
if [[ $rebuild -eq 1 ]]; then
  if ! "$HAO" build "test/$SUITE" -o "$exe" >/dev/null 2>&1; then
    echo "BUILD_FAIL: suite"; exit 1
  fi
  echo "  rebuilt: suite"
fi

echo "== 运行套件 =="
"$exe" > "$OUTDIR/suite" 2>&1
code=$?
total=0
while IFS= read -r _; do total=$((total+1)); done < "$OUTDIR/suite"

echo "== 语法解析 =="
if "$HAO" parse test/syntax.hao >/dev/null 2>&1; then
  echo "  OK   syntax(parse)"
else
  echo "  FAIL syntax"; code=1
fi

echo "== haoproject / localReferences =="
modexe="$CACHE/modsmoke.exe"
if ! "$HAO" build test/modsmoke/app -o "$modexe" >/dev/null 2>&1; then
  echo "  FAIL modsmoke(build)"; code=1
else
  modout=$("$modexe" 2>&1) || true
  if [[ "$modout" == "modsmoke-ok" ]]; then
    echo "  OK   modsmoke(localReferences)"
  else
    echo "  FAIL modsmoke(run): got [$modout]"; code=1
  fi
fi

# 包管理规范：HAO_REGISTRY=HTTP 私服（RegisterRepo）；HAO_REPO=repo/LocalRepo
# 每用例前清空 LocalRepo，避免本地仓低版本挡住源仓更高版；测完必须关私服。
http_pid=""
haoreg_port=8765
stop_haoreg() {
  if [[ -n "${http_pid:-}" ]]; then
    kill "$http_pid" 2>/dev/null || true
    taskkill //F //PID "$http_pid" >/dev/null 2>&1 || true
    http_pid=""
  fi
  # Windows 上 bash $! 常对不上真 python PID，按端口再清一遍
  if command -v powershell.exe >/dev/null 2>&1 || command -v powershell >/dev/null 2>&1; then
    PS=powershell.exe
    command -v powershell.exe >/dev/null 2>&1 || PS=powershell
    "$PS" -NoProfile -Command \
      "Get-NetTCPConnection -LocalPort $haoreg_port -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id \$_.OwningProcess -Force -ErrorAction SilentlyContinue }" \
      >/dev/null 2>&1 || true
  fi
  sleep 0.3
  unset HAO_REGISTRY HAO_REPO 2>/dev/null || true
}
clear_local_repo() {
  rm -rf repo/LocalRepo
  mkdir -p repo/LocalRepo
}

if [[ -d test/modsmoke/depsapp || -d test/modsmoke/depsemver || -d test/modsmoke/depgraph || -d test/modsmoke/depshttp ]]; then
  echo "== haoproject / HTTP registry → HAO_REPO=repo/LocalRepo =="
  if ! command -v python >/dev/null 2>&1 && ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP 包管理（无 python，无法起 haoreg）"
  else
    PY=python
    command -v python >/dev/null 2>&1 || PY=python3
    port=$haoreg_port
    # 先清残留私服，再起
    stop_haoreg
    sleep 0.2
    clear_local_repo
    "$PY" script/haoreg_server.py --root repo/RegisterRepo --port "$port" \
      >"$OUTDIR/haoreg.log" 2>&1 &
    http_pid=$!
    trap 'stop_haoreg' EXIT
    sleep 1
    export HAO_REGISTRY="http://127.0.0.1:$port"
    export HAO_REPO="repo/LocalRepo"

    if ! "$PY" -c "
import json, urllib.request
base='http://127.0.0.1:$port'
t=json.load(urllib.request.urlopen(base+'/api/v1/packages', timeout=3))
assert t.get('mode')=='tree' and 'dirs' in t
f=json.load(urllib.request.urlopen(base+'/api/v1/packages?flat=1', timeout=3))
assert f.get('mode')=='flat' and f.get('count',0)>=1
d=json.load(urllib.request.urlopen(base+'/api/v1/packages/example.com', timeout=3))
assert d.get('mode')=='tree'
" >/dev/null 2>&1; then
      echo "  FAIL haoreg(分层 API)"; code=1
    else
      echo "  OK   haoreg(树/flat/下钻)"
    fi

    # --- depsapp：HTTP → LocalRepo ---
    if [[ -d test/modsmoke/depsapp ]]; then
      clear_local_repo
      rm -f test/modsmoke/depsapp/haoproject.lock.json
      if ! "$HAO" mod tidy test/modsmoke/depsapp >/dev/null 2>&1; then
        echo "  FAIL depsapp(mod tidy)"; code=1
      elif [[ ! -d repo/LocalRepo/example.com/demo/utilpkg/1.0.0 ]]; then
        echo "  FAIL depsapp(LocalRepo 布局)"; code=1
      else
        depexe="$CACHE/depsapp.exe"
        if ! "$HAO" build test/modsmoke/depsapp -o "$depexe" >/dev/null 2>&1; then
          echo "  FAIL depsapp(build)"; code=1
        else
          depout=$("$depexe" 2>&1) || true
          if [[ "$depout" == "deps-ok" ]]; then
            echo "  OK   depsapp(HTTP→LocalRepo)"
          else
            echo "  FAIL depsapp(run): got [$depout]"; code=1
          fi
        fi
        # 纯本地仓命中：保留 LocalRepo，关掉源
        unset HAO_REGISTRY
        rm -f test/modsmoke/depsapp/haoproject.lock.json
        if ! "$HAO" mod tidy test/modsmoke/depsapp >/dev/null 2>&1; then
          echo "  FAIL depsapp(本地仓命中)"; code=1
        else
          echo "  OK   depsapp(LocalRepo 命中无源)"
        fi
        export HAO_REGISTRY="http://127.0.0.1:$port"
      fi
      rm -f test/modsmoke/depsapp/haoproject.lock.json
    fi

    # --- semver ---
    if [[ -d test/modsmoke/depsemver ]]; then
      echo "== haoproject / semver (^) =="
      clear_local_repo
      rm -f test/modsmoke/depsemver/haoproject.lock.json
      if ! "$HAO" mod tidy test/modsmoke/depsemver >"$OUTDIR/semver_tidy.txt" 2>&1; then
        echo "  FAIL depsemver(mod tidy)"; code=1
      elif ! grep -q '"version": "1.1.0"' test/modsmoke/depsemver/haoproject.lock.json; then
        echo "  FAIL depsemver(lock 应为 1.1.0)"; code=1
      else
        sexe="$CACHE/depsemver.exe"
        if ! "$HAO" build test/modsmoke/depsemver -o "$sexe" >/dev/null 2>&1; then
          echo "  FAIL depsemver(build)"; code=1
        else
          sout=$("$sexe" 2>&1) || true
          if [[ "$sout" == "semver-1.1.0" ]]; then
            echo "  OK   depsemver(^1.0.0 → 1.1.0)"
          else
            echo "  FAIL depsemver(run): got [$sout]"; code=1
          fi
        fi
      fi
      mkdir -p "$OUTDIR/badver"
      printf '%s\n' '{' \
        '  "project": { "name": "bad", "module": "example/bad", "version": "0.1.0", "main": "main.hao" },' \
        '  "dependencies": { "example.com/demo/utilpkg": "*" },' \
        '  "registry": { "includeDefault": false }' \
        '}' > "$OUTDIR/badver/haoproject.json"
      printf 'package main;\nfunc main() {}\n' > "$OUTDIR/badver/main.hao"
      if "$HAO" mod tidy "$OUTDIR/badver" >/dev/null 2>&1; then
        echo "  FAIL semver(* 应拒绝)"; code=1
      else
        echo "  OK   semver(拒绝 *)"
      fi
      mkdir -p "$OUTDIR/nover"
      printf '%s\n' '{' \
        '  "project": { "name": "nover", "module": "example/nover", "version": "0.1.0", "main": "main.hao" },' \
        '  "dependencies": { "example.com/demo/utilpkg": "^3.0.0" },' \
        '  "registry": { "includeDefault": false }' \
        '}' > "$OUTDIR/nover/haoproject.json"
      printf 'package main;\nfunc main() {}\n' > "$OUTDIR/nover/main.hao"
      if "$HAO" mod tidy "$OUTDIR/nover" >/dev/null 2>&1; then
        echo "  FAIL semver(^3 应无匹配)"; code=1
      else
        echo "  OK   semver(无满足版本)"
      fi
      rm -f test/modsmoke/depsemver/haoproject.lock.json
    fi

    # --- 传递依赖 / exclude / conflict ---
    if [[ -d test/modsmoke/depgraph ]]; then
      echo "== haoproject / transitive deps =="
      clear_local_repo
      rm -f test/modsmoke/depgraph/haoproject.lock.json
      if ! "$HAO" mod tidy test/modsmoke/depgraph >"$OUTDIR/graph_tidy.txt" 2>&1; then
        echo "  FAIL depgraph(mod tidy)"; code=1
      elif ! grep -q 'example.com/demo/leafpkg' test/modsmoke/depgraph/haoproject.lock.json; then
        echo "  FAIL depgraph(lock 缺 leafpkg)"; code=1
      else
        gexe="$CACHE/depgraph.exe"
        if ! "$HAO" build test/modsmoke/depgraph -o "$gexe" >/dev/null 2>&1; then
          echo "  FAIL depgraph(build)"; code=1
        else
          gout=$("$gexe" 2>&1) || true
          if [[ "$gout" == "leaf-1.0.0" ]]; then
            echo "  OK   depgraph(mid→leaf)"
          else
            echo "  FAIL depgraph(run): got [$gout]"; code=1
          fi
        fi
        if "$HAO" mod why example.com/demo/leafpkg test/modsmoke/depgraph >"$OUTDIR/why.txt" 2>&1; then
          if grep -q 'required by' "$OUTDIR/why.txt"; then
            echo "  OK   mod why leafpkg"
          else
            echo "  FAIL mod why 无 required by"; code=1
          fi
        else
          echo "  FAIL mod why"; code=1
        fi
      fi
      clear_local_repo
      rm -f test/modsmoke/depexclude/haoproject.lock.json
      if ! "$HAO" mod tidy test/modsmoke/depexclude >"$OUTDIR/ex_tidy.txt" 2>&1; then
        echo "  FAIL depexclude(mod tidy)"; code=1
      elif grep -q 'example.com/demo/leafpkg' test/modsmoke/depexclude/haoproject.lock.json; then
        echo "  FAIL depexclude(lock 不应含 leaf)"; code=1
      else
        echo "  OK   depexclude(排除 leaf)"
      fi
      mkdir -p "$OUTDIR/conflict"
      printf '%s\n' '{' \
        '  "project": { "name": "c", "module": "example/c", "version": "0.1.0", "main": "main.hao" },' \
        '  "dependencies": {' \
        '    "example.com/demo/apkg": "1.0.0",' \
        '    "example.com/demo/bpkg": "1.0.0"' \
        '  },' \
        '  "registry": { "includeDefault": false }' \
        '}' > "$OUTDIR/conflict/haoproject.json"
      printf 'package main;\nfunc main() {}\n' > "$OUTDIR/conflict/main.hao"
      if "$HAO" mod tidy "$OUTDIR/conflict" >/dev/null 2>&1; then
        echo "  FAIL conflict(应硬失败)"; code=1
      else
        echo "  OK   conflict(硬失败)"
      fi
      rm -f test/modsmoke/depgraph/haoproject.lock.json
      rm -f test/modsmoke/depexclude/haoproject.lock.json
    fi

    # --- depshttp 再冒烟一次（同规范）---
    if [[ -d test/modsmoke/depshttp ]]; then
      clear_local_repo
      rm -f test/modsmoke/depshttp/haoproject.lock.json
      if ! "$HAO" mod tidy test/modsmoke/depshttp >"$OUTDIR/http_tidy.txt" 2>&1; then
        echo "  FAIL depshttp(mod tidy)"; code=1
        cat "$OUTDIR/http_tidy.txt" || true
      else
        hexe="$CACHE/depshttp.exe"
        if ! "$HAO" build test/modsmoke/depshttp -o "$hexe" >/dev/null 2>&1; then
          echo "  FAIL depshttp(build)"; code=1
        else
          hout=$("$hexe" 2>&1) || true
          if [[ "$hout" == "deps-ok" ]]; then
            echo "  OK   depshttp(HTTP→LocalRepo)"
          else
            echo "  FAIL depshttp(run): got [$hout]"; code=1
          fi
        fi
      fi
      rm -f test/modsmoke/depshttp/haoproject.lock.json
    fi

    stop_haoreg
    trap - EXIT
    echo "  OK   haoreg(已停止)"
  fi
fi

echo "== hao fmt / hao test =="
# fmt：行尾空白 + CRLF 脏文件应 --check 失败，-w 后再通过
printf 'package main;\nfunc main() { fmt.println(1); }   \r\n' > "$OUTDIR/fmt_dirty.hao"
if "$HAO" fmt --check "$OUTDIR/fmt_dirty.hao" >/dev/null 2>&1; then
  echo "  FAIL fmt(--check 应失败)"; code=1
else
  echo "  OK   fmt(--check dirty)"
fi
if ! "$HAO" fmt -w "$OUTDIR/fmt_dirty.hao" >/dev/null 2>&1; then
  echo "  FAIL fmt(-w)"; code=1
elif "$HAO" fmt --check "$OUTDIR/fmt_dirty.hao" >/dev/null 2>&1; then
  echo "  OK   fmt(-w + --check)"
else
  echo "  FAIL fmt(写回后仍需格式化)"; code=1
fi
# fmt：缩进重排（4 空格）+ golden
printf 'package main;\nfunc main() {\nfmt.println(1);\nif (true) {\nfmt.println(2);\n}\n}\n' > "$OUTDIR/fmt_indent.hao"
if "$HAO" fmt --check "$OUTDIR/fmt_indent.hao" >/dev/null 2>&1; then
  echo "  FAIL fmt(缩进脏应 --check 失败)"; code=1
elif ! "$HAO" fmt -w "$OUTDIR/fmt_indent.hao" >/dev/null 2>&1; then
  echo "  FAIL fmt(缩进 -w)"; code=1
elif ! cmp -s "$OUTDIR/fmt_indent.hao" test/fmtsmoke/indent_expect.hao; then
  echo "  FAIL fmt(缩进与 golden 不一致)"; code=1
elif "$HAO" fmt --check "$OUTDIR/fmt_indent.hao" >/dev/null 2>&1; then
  echo "  OK   fmt(缩进 + golden)"
else
  echo "  FAIL fmt(缩进写回后仍需格式化)"; code=1
fi
# fmt：坏语法拒绝写回
printf 'package main;\nfunc main() {{{{\n' > "$OUTDIR/fmt_bad.hao"
if "$HAO" fmt -w "$OUTDIR/fmt_bad.hao" >/dev/null 2>&1; then
  echo "  FAIL fmt(坏语法应拒绝)"; code=1
else
  echo "  OK   fmt(坏语法拒绝写回)"
fi
# fmt：目录递归（子目录 dirty）
mkdir -p "$OUTDIR/fmt_tree/sub"
printf 'package main;\nfunc a() {\n    fmt.println(1);\n}\n' > "$OUTDIR/fmt_tree/ok.hao"
printf 'package main;\nfunc b() {\nfmt.println(1);\n}\n' > "$OUTDIR/fmt_tree/sub/dirty.hao"
if "$HAO" fmt --check "$OUTDIR/fmt_tree" >/dev/null 2>&1; then
  echo "  FAIL fmt(递归 --check 应失败)"; code=1
elif ! "$HAO" fmt -w "$OUTDIR/fmt_tree" >/dev/null 2>&1; then
  echo "  FAIL fmt(递归 -w)"; code=1
elif "$HAO" fmt --check "$OUTDIR/fmt_tree" >/dev/null 2>&1; then
  echo "  OK   fmt(目录递归)"
else
  echo "  FAIL fmt(递归写回后仍需格式化)"; code=1
fi
# test：Go 式 *_test.hao + TestXxx（testdemo）；产物在 target/test/hao-test 跑完即删
if "$HAO" test test/modsmoke/testdemo >"$OUTDIR/hao_test.txt" 2>&1; then
  if grep -q $'ok' "$OUTDIR/hao_test.txt"; then
    echo "  OK   hao test testdemo"
  else
    echo "  FAIL hao test（无 ok 行）"; code=1
  fi
else
  echo "  FAIL hao test testdemo"; code=1
fi
# 普通 build 不得编入 *_test.hao：只跑业务 main，输出应为 3
if ! "$HAO" build test/modsmoke/testdemo -o "$OUTDIR/testdemo_prod.exe" >/dev/null 2>&1; then
  echo "  FAIL hao build testdemo（应忽略 *_test.hao）"; code=1
elif [[ "$("$OUTDIR/testdemo_prod.exe")" != "3" ]]; then
  echo "  FAIL testdemo 生产入口输出异常"; code=1
else
  echo "  OK   hao build 排除 *_test.hao"
fi

# 收工：清临时输出与一次性 exe，保留 suite 增量缓存（规则 7）
rm -rf "$OUTDIR" 2>/dev/null || true
rm -rf "$TESTROOT/hao-test" 2>/dev/null || true
rm -f "$CACHE/modsmoke.exe" "$CACHE/depsapp.exe" "$CACHE/depsemver.exe" "$CACHE/depgraph.exe" "$CACHE/depshttp.exe" 2>/dev/null || true
rm -f "$CACHE"/*.ll 2>/dev/null || true
rm -f target/*.ll target/*.exe 2>/dev/null || true

echo "========================================"
echo "套件总行数: $total  (基线 1050) | 退出码: $code"
[[ $code -eq 0 ]] || exit 1
# P3 双门禁：行数必须等于基线（曾出现增量缓存 suite 少行误报 1007）
if [[ "$total" -ne 1050 ]]; then
  echo "FAIL 套件行数 $total != 1050（请 --rebuild-all 或查丢打印）"
  exit 1
fi
# 基线：v0.78 废数组+=迁 ArrayList → 1050；v0.76 SB +3 → 1052；v0.60.2 → 1049

# C0：定位/spill 门禁（Win + powershell；与套件双门禁同级）
if command -v powershell >/dev/null 2>&1 || command -v powershell.exe >/dev/null 2>&1; then
  PS=powershell
  command -v powershell.exe >/dev/null 2>&1 && PS=powershell.exe
  echo "== loc_smoke / spill_ir_smoke =="
  if ! "$PS" -NoProfile -ExecutionPolicy Bypass -File script/win/loc_smoke.ps1; then
    echo "FAIL loc_smoke.ps1"
    exit 1
  fi
  if ! "$PS" -NoProfile -ExecutionPolicy Bypass -File script/win/spill_ir_smoke.ps1; then
    echo "FAIL spill_ir_smoke.ps1"
    exit 1
  fi
  if ! "$PS" -NoProfile -ExecutionPolicy Bypass -File script/win/stringbuilder_gate.ps1; then
    echo "FAIL stringbuilder_gate.ps1"
    exit 1
  fi
  if ! "$PS" -NoProfile -ExecutionPolicy Bypass -File script/win/sb_gc_gate.ps1; then
    echo "FAIL sb_gc_gate.ps1"
    exit 1
  fi
else
  echo "SKIP loc/spill/sb smoke (no powershell)"
fi
