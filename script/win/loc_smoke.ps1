# HaoLang loc smoke (D0/L1/L2/U1)
# Usage from repo root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\loc_smoke.ps1
$ErrorActionPreference = "Continue"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { $hao = "hao" }

$td = Join-Path $Root "target\repro-loc"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$fail = 0
$log = Join-Path $Root "hao-crash.log"

function Reset-CrashLog {
    if (Test-Path $log) { Remove-Item $log -Force }
}

# --- D0: bad compile path:line:col ---
@'
func main() {
  var x: Int = "bad"
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "bad.hao")
$err1 = & $hao build (Join-Path $td "bad.hao") 2>&1 | Out-String
if ($err1 -match "bad\.hao:\d+:\d+:") {
    Write-Host "OK   bad compile has path:line:col"
} else {
    Write-Host "FAIL bad compile missing path:line:col"
    Write-Host $err1
    $fail++
}

# --- panic src= user file ---
@'
import fmt

func main() {
  var p: String? = null
  fmt.println(p!!)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "panic.hao")
Reset-CrashLog
& $hao run (Join-Path $td "panic.hao") 2>&1 | Out-Null
$okSrc = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "src=.*panic\.hao:\d+:\d+") { $okSrc = $true }
    if ($txt -match "src=.*native\.hao") { $okSrc = $false }
}
if ($okSrc) {
    Write-Host "OK   panic log has src=...panic.hao:"
} else {
    Write-Host "FAIL panic missing src= or drifted to native.hao"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- L2: cross-fn stack >=2 user frames ---
@'
import fmt

func boom() {
  var p: String? = null
  fmt.println(p!!)
}

func main() {
  boom()
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "stack.hao")
Reset-CrashLog
& $hao run (Join-Path $td "stack.hao") 2>&1 | Out-Null
$okStack = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    $n = ([regex]::Matches($txt, "#\d+ .*stack\.hao:\d+:")).Count
    $hasMethods = $txt -match "hao_stack:[\s\S]*boom\(" -and $txt -match "hao_stack:[\s\S]*main\("
    if ($txt -match "src=.*stack\.hao:\d+:" -and $n -ge 2 -and $hasMethods) { $okStack = $true }
    if ($txt -match "src=.*native\.hao") { $okStack = $false }
}
if ($okStack) {
    Write-Host "OK   stack has >=2 frames with method names"
} else {
    Write-Host "FAIL cross-fn stack frames"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- toolError hao:0:0 ---
$err3 = & $hao build (Join-Path $td "panic.hao") --target no-such -o (Join-Path $td "x.exe") 2>&1 | Out-String
if ($err3 -match "hao:0:0:") {
    Write-Host "OK   tool error hao:0:0"
} else {
    Write-Host "FAIL unknown target not Diagnostic toolError"
    Write-Host $err3
    $fail++
}

# --- D0/L1: call site src= must be user file (not native.hao) via List.add + !! ---
@'
import fmt
import collections.*

class Box {
    public var s: String = ""
    public constructor(s: String) { this.s = s; }
}

func main() {
    var junk = new ArrayList<Object>()
    junk.add(new Box("x"))
    var p: String? = null
    fmt.println(p!!)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "callsite.hao")
Reset-CrashLog
& $hao run (Join-Path $td "callsite.hao") 2>&1 | Out-Null
$okCall = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "src=.*callsite\.hao:\d+:\d+" -and $txt -notmatch "src=.*native\.hao") {
        $okCall = $true
    }
}
if ($okCall) {
    Write-Host "OK   callsite src= user file (not native.hao)"
} else {
    Write-Host "FAIL callsite src= drifted or missing"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- L2: if stack has stdlib path, it must carry [lib] ---
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    $libLines = [regex]::Matches($txt, "#\d+ .*stdlib[/\\][^\r\n]+")
    $badLib = $false
    foreach ($m in $libLines) {
        if ($m.Value -notmatch "\[lib\]") { $badLib = $true }
    }
    if ($badLib) {
        Write-Host "FAIL stdlib stack frame missing [lib]"
        Get-Content $log
        $fail++
    } else {
        Write-Host "OK   stack [lib] tag (or no lib frames)"
    }
}

# --- D0: AV trap -> access= + user stack ---
@'
extern func hao_debug_trap_av(): Unit = "hao_debug_trap_av";

func main() {
    hao_debug_trap_av();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "av.hao")
Reset-CrashLog
& $hao run (Join-Path $td "av.hao") 2>&1 | Out-Null
$okAv = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    $hasAccess = $txt -match "access=" -or $txt -match "av_addr="
    $hasUser = $txt -match "av\.hao:\d+"
    $hasTime = $txt -match "time=\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}"
    $hasWhere = $txt -match "where=.* at .*av\.hao:\d+:\d+"
    $hasHaoStack = $txt -match "hao_stack:[\s\S]*main\("
    if ($hasAccess -and $hasUser -and $hasTime -and $hasWhere -and $hasHaoStack) { $okAv = $true }
}
if ($okAv) {
    Write-Host "OK   AV crash has time=/where=/hao_stack=method + access="
} else {
    Write-Host "FAIL AV crash log incomplete"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- U1: uncaught throw loc ---
@'
import exception.*

func main() {
    throw new Exception("boom")
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "throw.hao")
Reset-CrashLog
& $hao run (Join-Path $td "throw.hao") 2>&1 | Out-Null
$okThrow = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "src=.*throw\.hao:\d+:\d+" -or $txt -match "hao_stack:[\s\S]*throw\.hao:\d+") {
        $okThrow = $true
    }
    if ($txt -match "src=.*native\.hao") { $okThrow = $false }
}
if ($okThrow) {
    Write-Host "OK   throw uncaught has user src=/stack="
} else {
    Write-Host "FAIL throw loc missing"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- T1: child-thread panic stack is TLS (no main-only mix required; must show worker file) ---
@'
import thread.*
import fmt

func worker() {
    var p: String? = null
    fmt.println(p!!)
}

func main() {
    var t = new Thread()
    t.start(worker)
    t.join()
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "tlsstack.hao")
Reset-CrashLog
& $hao run (Join-Path $td "tlsstack.hao") 2>&1 | Out-Null
$okTls = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    # Worker frame must appear; process exits in worker so stack is that thread's TLS
    if ($txt -match "tlsstack\.hao:\d+" -and $txt -match "hao_stack:") {
        $okTls = $true
    }
}
if ($okTls) {
    Write-Host "OK   thread panic stack has tlsstack.hao frame"
} else {
    Write-Host "FAIL thread TLS stack"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- V2: HAO_GC_VERIFY bad shadow root fatal with user loc ---
@'
import gc

extern func hao_debug_poison_shadow_root(): Unit = "hao_debug_poison_shadow_root";

func main() {
    hao_debug_poison_shadow_root();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_bad.hao")
Reset-CrashLog
$prevV = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_bad.hao") 2>&1 | Out-Null
if ($null -ne $prevV) { $env:HAO_GC_VERIFY = $prevV } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okVfy = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "shadow_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_bad\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_bad\.hao:\d+")) {
        $okVfy = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_bad\.hao") {
        $okVfy = $false
    }
}
if ($okVfy) {
    Write-Host "OK   VERIFY poison has gc_verify + shadow_i + verify_bad.hao loc"
} else {
    Write-Host "FAIL VERIFY poison loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- V3: VERIFY on alloc-triggered collect (not only GC.collect) ---
@'
import collections.*
import gc

extern func hao_debug_poison_shadow_root(): Unit = "hao_debug_poison_shadow_root";

class Box {
    public var s: String = ""
    public constructor(s: String) { this.s = s; }
}

func main() {
    hao_debug_poison_shadow_root();
    var xs = new ArrayList<Object>();
    var i = 0;
    while (i < 8000) {
        xs.add(new Box("pad-0123456789abcdef0123456789abcdef"));
        i += 1;
    }
    fmt.println(xs.size());
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_alloc.hao")
Reset-CrashLog
$prevVa = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_alloc.hao") 2>&1 | Out-Null
if ($null -ne $prevVa) { $env:HAO_GC_VERIFY = $prevVa } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okVa = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "shadow_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_alloc\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_alloc\.hao:\d+")) {
        $okVa = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_alloc\.hao") {
        $okVa = $false
    }
}
if ($okVa) {
    Write-Host "OK   VERIFY alloc-collect has gc_verify + shadow_i + verify_alloc.hao loc"
} else {
    Write-Host "FAIL VERIFY alloc-collect loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- D3: -g emit has llvm.dbg.declare + var name ---
@'
func add(a: Int, b: Int): Int {
    var sum = a + b
    return sum
}
func main() {
    var x = 1
    fmt.println(add(x, 2))
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_declare.hao")
$ll = Join-Path $td "dbg_declare.ll"
& $hao emit -g (Join-Path $td "dbg_declare.hao") -o $ll 2>&1 | Out-Null
$okDbg = $false
if (Test-Path $ll) {
    $irtxt = Get-Content $ll -Raw
    if ($irtxt -match 'llvm\.dbg\.declare' -and $irtxt -match 'DILocalVariable\(name: "sum"' -and
        $irtxt -match 'DILocalVariable\(name: "a"' -and $irtxt -match 'DILocalVariable\(name: "x"') {
        $okDbg = $true
    }
}
if ($okDbg) {
    Write-Host "OK   -g emit has dbg.declare + DILocalVariable names"
} else {
    Write-Host "FAIL -g dbg.declare missing"
    $fail++
}

# --- D4: catch/for declare + try setjmp/switch !dbg ---
@'
import collections.*
import exception.*

class Box {
    public var s: String = ""
    public constructor(s: String) { this.s = s; }
}

func main() {
    var xs = new ArrayList<Object>()
    xs.add(new Box("a"))
    xs.add(new Box("b"))
    for (it in xs) {
        try {
            if ((it as Box).s == "b") { throw new Exception("x") }
        } catch (e: Exception) {
            fmt.println(e != null)
        }
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_catch_for.hao")
$ll2 = Join-Path $td "dbg_catch_for.ll"
& $hao emit -g (Join-Path $td "dbg_catch_for.hao") -o $ll2 2>&1 | Out-Null
$okDbg2 = $false
if (Test-Path $ll2) {
    $irtxt2 = Get-Content $ll2 -Raw
    $hasCatch = $irtxt2 -match 'DILocalVariable\(name: "e"'
    $hasFor = $irtxt2 -match 'DILocalVariable\(name: "it"'
    $hasSj = $irtxt2 -match '_setjmp[^\n]*!dbg|setjmp[^\n]*!dbg'
    $hasSw = $irtxt2 -match 'switch i32[^\n]*!dbg'
    if ($hasCatch -and $hasFor -and ($hasSj -or $hasSw)) { $okDbg2 = $true }
}
if ($okDbg2) {
    Write-Host "OK   -g catch/for declare + Except !dbg"
} else {
    Write-Host "FAIL -g catch/for/Except dbg"
    $fail++
}

# --- V4: hao_gc_verify_skip_reenter symbol callable (count >= 0) ---
@'
extern func hao_gc_verify_skip_reenter(): Long = "hao_gc_verify_skip_reenter";
func main() {
    fmt.println(hao_gc_verify_skip_reenter() >= 0);
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_skip.hao")
$vsOut = & $hao run (Join-Path $td "verify_skip.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -and $vsOut -match '(?m)^true\s*$') {
    Write-Host "OK   hao_gc_verify_skip_reenter readable"
} else {
    Write-Host "FAIL verify_skip_reenter"
    Write-Host $vsOut
    $fail++
}

# --- L5: select switch has !dbg under -g ---
@'
import channel
func main() {
    var ch = channel.make(1)
    ch.sendStr("a")
    select {
        case s = ch.recvStr():
            fmt.println(s!! == "a")
        default:
            fmt.println(false)
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_select.hao")
$llSel = Join-Path $td "dbg_select.ll"
& $hao emit -g (Join-Path $td "dbg_select.hao") -o $llSel 2>&1 | Out-Null
$okSel = $false
if (Test-Path $llSel) {
    $stxt = Get-Content $llSel -Raw
    if ($stxt -match 'switch i32[^\n]*!dbg') { $okSel = $true }
}
if ($okSel) {
    Write-Host "OK   -g select switch has !dbg"
} else {
    Write-Host "FAIL -g select !dbg"
    $fail++
}

# --- V6: VERIFY poison scan pin has pin_i= ---
@'
import gc

extern func hao_debug_poison_scan_pin(): Unit = "hao_debug_poison_scan_pin";

func main() {
    hao_debug_poison_scan_pin();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_pin.hao")
Reset-CrashLog
$prevVp = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_pin.hao") 2>&1 | Out-Null
if ($null -ne $prevVp) { $env:HAO_GC_VERIFY = $prevVp } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okPin = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "pin_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_pin\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_pin\.hao:\d+")) {
        $okPin = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_pin\.hao") {
        $okPin = $false
    }
}
if ($okPin) {
    Write-Host "OK   VERIFY poison pin has pin_i + verify_pin.hao loc"
} else {
    Write-Host "FAIL VERIFY poison pin loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- D5: -g emit has llvm.dbg.value ---
@'
func main() {
    var x: Int = 42
    fmt.println(x == 42)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value.hao")
$llVal = Join-Path $td "dbg_value.ll"
& $hao emit -g (Join-Path $td "dbg_value.hao") -o $llVal 2>&1 | Out-Null
$okVal = $false
if (Test-Path $llVal) {
    $vtxt = Get-Content $llVal -Raw
    $nVal = ([regex]::Matches($vtxt, 'llvm\.dbg\.value')).Count
    if ($nVal -ge 1) { $okVal = $true }
}
if ($okVal) {
    Write-Host "OK   -g emit has llvm.dbg.value"
} else {
    Write-Host "FAIL -g dbg.value"
    $fail++
}

# --- U8: Unit-lambda x try ret void (no ret i32 in lambda) ---
@'
package main
func main() {
    val f: ()->Unit = {
        try {
            return
        } finally {
        }
    }
    f()
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "unit_lambda_try.hao")
$llU8 = Join-Path $td "unit_lambda_try.ll"
& $hao emit (Join-Path $td "unit_lambda_try.hao") -o $llU8 2>&1 | Out-Null
$okU8 = $false
if (Test-Path $llU8) {
    $utxt = Get-Content $llU8 -Raw
    $lm = [regex]::Match($utxt, 'define void @lambda\$\d+\(ptr %env\.arg\) \{([\s\S]*?)\n\}')
    if ($lm.Success) {
        $body = $lm.Groups[1].Value
        if ($body -notmatch '(?m)^\s*ret i32\b') { $okU8 = $true }
    }
}
$runU8 = & $hao run (Join-Path $td "unit_lambda_try.hao") 2>&1 | Out-String
if ($okU8 -and $LASTEXITCODE -eq 0) {
    Write-Host "OK   Unit-lambda x try ret void + link"
} else {
    Write-Host "FAIL Unit-lambda x try (emit/link)"
    Write-Host $runU8
    $fail++
}

# --- V7: VERIFY poison remset has remset_i= ---
@'
import gc

extern func hao_debug_poison_remset(): Unit = "hao_debug_poison_remset";

func main() {
    hao_debug_poison_remset();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_remset.hao")
Reset-CrashLog
$prevVr = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_remset.hao") 2>&1 | Out-Null
if ($null -ne $prevVr) { $env:HAO_GC_VERIFY = $prevVr } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okRem = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "remset_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_remset\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_remset\.hao:\d+")) {
        $okRem = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_remset\.hao") {
        $okRem = $false
    }
}
if ($okRem) {
    Write-Host "OK   VERIFY poison remset has remset_i + verify_remset.hao loc"
} else {
    Write-Host "FAIL VERIFY poison remset loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- D6: -g assign path has >=2 dbg.value ---
@'
func main() {
    var x: Int = 1
    x = 2
    fmt.println(x == 2)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_assign.hao")
$llAsg = Join-Path $td "dbg_value_assign.ll"
& $hao emit -g (Join-Path $td "dbg_value_assign.hao") -o $llAsg 2>&1 | Out-Null
$okAsg = $false
if (Test-Path $llAsg) {
    $atxt = Get-Content $llAsg -Raw
    $nAsg = ([regex]::Matches($atxt, 'llvm\.dbg\.value')).Count
    if ($nAsg -ge 2 -and $atxt -match 'DILocalVariable\(name: "x"') { $okAsg = $true }
}
if ($okAsg) {
    Write-Host "OK   -g assign has dbg.value >=2"
} else {
    Write-Host "FAIL -g assign dbg.value"
    $fail++
}

# --- V8: VERIFY poison refl_i64 has refl_i64_i= ---
@'
import gc

extern func hao_debug_poison_refl_i64(): Unit = "hao_debug_poison_refl_i64";

func main() {
    hao_debug_poison_refl_i64();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_refl.hao")
Reset-CrashLog
$prevVrfl = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_refl.hao") 2>&1 | Out-Null
if ($null -ne $prevVrfl) { $env:HAO_GC_VERIFY = $prevVrfl } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okRfl = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "refl_i64_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_refl\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_refl\.hao:\d+")) {
        $okRfl = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_refl\.hao") {
        $okRfl = $false
    }
}
if ($okRfl) {
    Write-Host "OK   VERIFY poison refl_i64 has refl_i64_i + verify_refl.hao loc"
} else {
    Write-Host "FAIL VERIFY poison refl_i64 loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- D7: -g compound assign has dbg.value ---
@'
func main() {
    var x: Int = 1
    x += 1
    fmt.println(x == 2)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_compound.hao")
$llCmp = Join-Path $td "dbg_value_compound.ll"
& $hao emit -g (Join-Path $td "dbg_value_compound.hao") -o $llCmp 2>&1 | Out-Null
$okCmp = $false
if (Test-Path $llCmp) {
    $ctxt = Get-Content $llCmp -Raw
    $nCmp = ([regex]::Matches($ctxt, 'llvm\.dbg\.value')).Count
    if ($nCmp -ge 2 -and $ctxt -match 'DILocalVariable\(name: "x"') { $okCmp = $true }
}
if ($okCmp) {
    Write-Host "OK   -g compound assign has dbg.value >=2"
} else {
    Write-Host "FAIL -g compound dbg.value"
    $fail++
}

# --- D8: -g field assign has dbg.value ---
@'
class Box {
    public var n: Int = 0
    public constructor() {}
}
func main() {
    var b = new Box()
    b.n = 7
    fmt.println(b.n == 7)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_field.hao")
$llFld = Join-Path $td "dbg_value_field.ll"
& $hao emit -g (Join-Path $td "dbg_value_field.hao") -o $llFld 2>&1 | Out-Null
$okFld = $false
if (Test-Path $llFld) {
    $ftxt = Get-Content $llFld -Raw
    if ($ftxt -match 'llvm\.dbg\.value' -and $ftxt -match 'DILocalVariable\(name: "n"') {
        $okFld = $true
    }
}
if ($okFld) {
    Write-Host "OK   -g field assign has dbg.value for n"
} else {
    Write-Host "FAIL -g field dbg.value"
    $fail++
}

# --- D9: -g array index assign has dbg.value ---
@'
func main() {
    var a: [Int] = [0, 0]
    a[0] = 9
    fmt.println(a[0] == 9)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_index.hao")
$llIdx = Join-Path $td "dbg_value_index.ll"
& $hao emit -g (Join-Path $td "dbg_value_index.hao") -o $llIdx 2>&1 | Out-Null
$okIdx = $false
if (Test-Path $llIdx) {
    $itxt = Get-Content $llIdx -Raw
    if ($itxt -match 'llvm\.dbg\.value' -and $itxt -match 'DILocalVariable\(name: "a"') {
        $okIdx = $true
    }
}
if ($okIdx) {
    Write-Host "OK   -g index assign has dbg.value for a"
} else {
    Write-Host "FAIL -g index dbg.value"
    $fail++
}

# --- V9: VERIFY poison gpr has gpr_i= ---
@'
import gc

extern func hao_debug_poison_gpr(): Unit = "hao_debug_poison_gpr";

func main() {
    hao_debug_poison_gpr();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_gpr.hao")
Reset-CrashLog
$prevVgpr = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_gpr.hao") 2>&1 | Out-Null
if ($null -ne $prevVgpr) { $env:HAO_GC_VERIFY = $prevVgpr } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okGpr = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "gpr_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_gpr\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_gpr\.hao:\d+")) {
        $okGpr = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_gpr\.hao") {
        $okGpr = $false
    }
}
if ($okGpr) {
    Write-Host "OK   VERIFY poison gpr has gpr_i + verify_gpr.hao loc"
} else {
    Write-Host "FAIL VERIFY poison gpr loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- D10: -g field compound assign has dbg.value ---
@'
class Box {
    public var n: Int = 0
    public constructor() {}
}
func main() {
    var b = new Box()
    b.n += 1
    fmt.println(b.n == 1)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_field_compound.hao")
$llFc = Join-Path $td "dbg_value_field_compound.ll"
& $hao emit -g (Join-Path $td "dbg_value_field_compound.hao") -o $llFc 2>&1 | Out-Null
$okFc = $false
if (Test-Path $llFc) {
    $fctxt = Get-Content $llFc -Raw
    $nFc = ([regex]::Matches($fctxt, 'llvm\.dbg\.value')).Count
    if ($nFc -ge 2 -and $fctxt -match 'DILocalVariable\(name: "n"') { $okFc = $true }
}
if ($okFc) {
    Write-Host "OK   -g field compound assign has dbg.value for n"
} else {
    Write-Host "FAIL -g field compound dbg.value"
    $fail++
}

# --- V10: VERIFY poison c_leaf has leaf_i= ---
@'
import gc

extern func hao_debug_poison_c_leaf(): Unit = "hao_debug_poison_c_leaf";

func main() {
    hao_debug_poison_c_leaf();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_leaf.hao")
Reset-CrashLog
$prevVleaf = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_leaf.hao") 2>&1 | Out-Null
if ($null -ne $prevVleaf) { $env:HAO_GC_VERIFY = $prevVleaf } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okLeaf = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "leaf_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_leaf\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_leaf\.hao:\d+")) {
        $okLeaf = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_leaf\.hao") {
        $okLeaf = $false
    }
}
if ($okLeaf) {
    Write-Host "OK   VERIFY poison c_leaf has leaf_i + verify_leaf.hao loc"
} else {
    Write-Host "FAIL VERIFY poison c_leaf loc"
    if (Test-Path $log) { Get-Content $log }
    $fail++
}

# --- D11: -g for-iter has dbg.value ---
@'
func main() {
    for (x in [1, 2]) {
        fmt.println(x > 0)
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_for.hao")
$llFor = Join-Path $td "dbg_value_for.ll"
& $hao emit -g (Join-Path $td "dbg_value_for.hao") -o $llFor 2>&1 | Out-Null
$okFor = $false
if (Test-Path $llFor) {
    $fortxt = Get-Content $llFor -Raw
    if ($fortxt -match 'llvm\.dbg\.value' -and $fortxt -match 'DILocalVariable\(name: "x"') {
        $okFor = $true
    }
}
if ($okFor) {
    Write-Host "OK   -g for-iter has dbg.value for x"
} else {
    Write-Host "FAIL -g for-iter dbg.value"
    $fail++
}

# --- D12: -g catch bind has dbg.value ---
@'
import exception.*
func main() {
    try {
        throw new Exception("x")
    } catch (e: Exception) {
        fmt.println(e.getMessage().length > 0)
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_catch.hao")
$llCatch = Join-Path $td "dbg_value_catch.ll"
& $hao emit -g (Join-Path $td "dbg_value_catch.hao") -o $llCatch 2>&1 | Out-Null
$okCatch = $false
if (Test-Path $llCatch) {
    $ctxt = Get-Content $llCatch -Raw
    if ($ctxt -match 'llvm\.dbg\.value' -and $ctxt -match 'DILocalVariable\(name: "e"') {
        $okCatch = $true
    }
}
if ($okCatch) {
    Write-Host "OK   -g catch bind has dbg.value for e"
} else {
    Write-Host "FAIL -g catch dbg.value"
    $fail++
}

# --- D13: -g lambda param has declare/value ---
@'
func main() {
    val f: Func<Int, Int> = { x -> x + 1 }
    fmt.println(f(1) == 2)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_lambda.hao")
$llLam = Join-Path $td "dbg_value_lambda.ll"
& $hao emit -g (Join-Path $td "dbg_value_lambda.hao") -o $llLam 2>&1 | Out-Null
$okLam = $false
if (Test-Path $llLam) {
    $ltxt = Get-Content $llLam -Raw
    if ($ltxt -match 'llvm\.dbg\.declare' -and $ltxt -match 'llvm\.dbg\.value' -and $ltxt -match 'DILocalVariable\(name: "x"') {
        $okLam = $true
    }
}
if ($okLam) {
    Write-Host "OK   -g lambda param has dbg.declare/value for x"
} else {
    Write-Host "FAIL -g lambda param dbg"
    $fail++
}

# --- D14: -g select recv bind has declare/value ---
@'
import channel;
func main() {
    var ch = channel.make(1)
    ch.sendInt(7)
    select {
        case v = ch.recvInt():
            fmt.println(v == 7)
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_select.hao")
$llSel = Join-Path $td "dbg_value_select.ll"
& $hao emit -g (Join-Path $td "dbg_value_select.hao") -o $llSel 2>&1 | Out-Null
$okSel = $false
if (Test-Path $llSel) {
    $stxt = Get-Content $llSel -Raw
    if ($stxt -match 'llvm\.dbg\.declare' -and $stxt -match 'llvm\.dbg\.value' -and $stxt -match 'DILocalVariable\(name: "v"') {
        $okSel = $true
    }
}
if ($okSel) {
    Write-Host "OK   -g select bind has dbg.declare/value for v"
} else {
    Write-Host "FAIL -g select bind dbg"
    $fail++
}

# --- D15: -g fn param + this have dbg.value ---
@'
class Box {
    public var n: Int = 0
    public constructor(n: Int) { this.n = n }
    public func get(): Int { return this.n }
}
func add1(x: Int): Int { return x + 1 }
func main() {
    var b = new Box(3)
    fmt.println(add1(1) == 2)
    fmt.println(b.get() == 3)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_fn_this.hao")
$llFn = Join-Path $td "dbg_value_fn_this.ll"
& $hao emit -g (Join-Path $td "dbg_value_fn_this.hao") -o $llFn 2>&1 | Out-Null
$okFn = $false
if (Test-Path $llFn) {
    $ftxt = Get-Content $llFn -Raw
    if ($ftxt -match 'llvm\.dbg\.value' -and
        $ftxt -match 'DILocalVariable\(name: "x"' -and
        $ftxt -match 'DILocalVariable\(name: "this"') {
        $okFn = $true
    }
}
if ($okFn) {
    Write-Host "OK   -g fn/this has dbg.value for x and this"
} else {
    Write-Host "FAIL -g fn/this dbg.value"
    $fail++
}

# --- V11: VERIFY poison ext root has ext_i= ---
@'
import gc

extern func hao_debug_poison_ext_root(): Unit = "hao_debug_poison_ext_root";

func main() {
    hao_debug_poison_ext_root();
    gc.GC.collect();
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_ext.hao")
Reset-CrashLog
$prevVext = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
& $hao run (Join-Path $td "verify_ext.hao") 2>&1 | Out-Null
if ($null -ne $prevVext) { $env:HAO_GC_VERIFY = $prevVext } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
$okExt = $false
if (Test-Path $log) {
    $txt = Get-Content $log -Raw
    if ($txt -match "kind=gc_verify" -and
        $txt -match "ext_i=" -and
        $txt -match "ptr=" -and
        ($txt -match "src=.*verify_ext\.hao:\d+" -or $txt -match "hao_stack:[\s\S]*verify_ext\.hao:\d+")) {
        $okExt = $true
    }
    if ($txt -match "src=.*native\.hao" -and $txt -notmatch "src=.*verify_ext\.hao") {
        $okExt = $false
    }
}
if ($okExt) {
    Write-Host "OK   VERIFY poison ext has ext_i + verify_ext.hao loc"
} else {
    Write-Host "FAIL VERIFY poison ext_i"
    if (Test-Path $log) { Get-Content $log -Raw | Write-Host }
    $fail++
}

# --- D16: -g lambda capture has declare/value ---
@'
func main() {
    var outer = 7
    val f: () -> Int = {
        return outer + 1
    }
    fmt.println(f() == 8)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_capture.hao")
$llCap = Join-Path $td "dbg_value_capture.ll"
& $hao emit -g (Join-Path $td "dbg_value_capture.hao") -o $llCap 2>&1 | Out-Null
$okCap = $false
if (Test-Path $llCap) {
    $ctxt = Get-Content $llCap -Raw
    if ($ctxt -match 'llvm\.dbg\.declare' -and $ctxt -match 'llvm\.dbg\.value' -and $ctxt -match 'DILocalVariable\(name: "outer"') {
        $okCap = $true
    }
}
if ($okCap) {
    Write-Host "OK   -g lambda capture has dbg.declare/value for outer"
} else {
    Write-Host "FAIL -g lambda capture dbg"
    $fail++
}

# --- D17: -g new field default has dbg.value ---
@'
class Box {
    public var s: String = "x"
}
func main() {
    var b = new Box()
    fmt.println(b.s == "x")
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "dbg_value_new_field.hao")
$llNew = Join-Path $td "dbg_value_new_field.ll"
& $hao emit -g (Join-Path $td "dbg_value_new_field.hao") -o $llNew 2>&1 | Out-Null
$okNew = $false
if (Test-Path $llNew) {
    $ntext = Get-Content $llNew -Raw
    if ($ntext -match 'llvm\.dbg\.value' -and $ntext -match 'DILocalVariable\(name: "s"') {
        $okNew = $true
    }
}
if ($okNew) {
    Write-Host "OK   -g new field default has dbg.value for s"
} else {
    Write-Host "FAIL -g new field default dbg"
    $fail++
}

if ($fail -eq 0) {
    Write-Host "loc_smoke: ALL PASS"
    exit 0
}
Write-Host "loc_smoke: $fail FAIL(s)"
exit 1

