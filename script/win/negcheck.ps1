# 反向编译拒绝：期望 hao emit 失败（退出码非 0）
# 用法：powershell -ExecutionPolicy Bypass -File script\win\negcheck.ps1
# 产物目录：target\test\negcheck（规则 7）；跑完清空。
$ErrorActionPreference = "Continue"
Set-Location ((Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path)
$hao = ".\output\hao.exe"
if (-not (Test-Path $hao)) { throw "缺少 output/hao.exe" }

$cases = @(
    @{ name = "Int?+"; code = "package main;`nfunc main() { var x: Int? = 1; fmt.println(x + 1); }" },
    @{ name = "Bool?!"; code = "package main;`nfunc main() { var b: Bool? = true; fmt.println(!b); }" },
    @{ name = "Int?+="; code = "package main;`nfunc main() { var x: Int? = 1; x += 1; }" },
    @{ name = "Bool?&&"; code = "package main;`nfunc main() { var b: Bool? = true; if (b && true) {} }" },
    @{ name = "if Bool?"; code = "package main;`nfunc main() { var b: Bool? = true; if (b) {} }" },
    @{ name = "idx Int?"; code = "package main;`nfunc main() { var a: [Int] = [1]; var i: Int? = 0; fmt.println(a[i]); }" },
    @{ name = "Int? ?? String"; code = "package main;`nfunc main() { var x: Int? = null; fmt.println(x ?? `"x`"); }" },
    @{ name = "tmpl Int?"; code = "package main;`nfunc main() { var x: Int? = 1; val s = `$`"n=`$`{x}`"; fmt.println(s); }" },
    @{ name = "str+="; code = "package main;`nfunc main() { var s = `"a`"; var x: Int? = 1; s += x; }" },
    @{ name = "Int? to Long?"; code = "package main;`nfunc main() { var a: Int? = 1; var b: Long? = a; fmt.println(b!!); }" },
    @{ name = "spread non-array"; code = "package main;`nfunc main() { val x = 1; val a = [...x]; fmt.println(a.length); }" },
    @{ name = "new array arity"; code = "package main;`nfunc main() { val a = new [Int](); fmt.println(a.length); }" },
    @{ name = "spread Array?"; code = "package main;`nfunc main() { var a: [Int]? = [1]; val b = [...a]; fmt.println(b.length); }" },
    @{ name = "when expr no else"; code = "package main;`nfunc main() { val x = when (1) { 2 -> 3 }; fmt.println(x); }" },
    @{ name = "Func?"; code = "package main;`nfunc main() { var f: Func<Int,Int>? = null; fmt.println(f != null); }" },
    @{ name = "Int? == Long?"; code = "package main;`nfunc main() { var a: Int? = 1; var b: Long? = 1; fmt.println(a == b); }" },
    @{ name = "Bool==Int"; code = "package main;`nfunc main() { fmt.println(true == 1); }" },
    @{ name = "throw Exception?"; code = "package main;`nimport exception;`nfunc main() { var e: exception.Exception? = null; throw e; }" },
    @{ name = "Object==Int"; code = "package main;`nfunc main() { fmt.println(new Object() == 0); }" },
    @{ name = "haoroutine params"; code = "package main;`nfunc main() { haoroutine { x -> fmt.println(x); }; }" },
    @{ name = "select empty"; code = "package main;`nfunc main() { select { } }" },
    @{ name = "select bad method"; code = "package main;`nimport channel;`nfunc main() { val ch = channel.make(1); select { case x = ch.close(): {} } }" },
    @{ name = "ULong+Long"; code = "package main;`nfunc main() { var u: ULong = 1; var s: Long = 1; fmt.println(u + s); }" },
    @{ name = "ULong/Long"; code = "package main;`nfunc main() { var u: ULong = 1; var s: Long = 1; fmt.println(u / s); }" },
    @{ name = "UIntPtr-Long"; code = "package main;`nfunc main() { var u: UIntPtr = 1; var s: Long = 1; fmt.println(u - s); }" },
    @{ name = "ULong&Long"; code = "package main;`nfunc main() { var u: ULong = 1; var s: Long = 1; fmt.println(u & s); }" }
)

$dir = "target\test\negcheck"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$fail = 0
$ok = 0
try {
    foreach ($c in $cases) {
        $safe = ($c.name -replace '[^\w]', '_')
        $f = Join-Path $dir "$safe.hao"
        Set-Content -Encoding utf8 -Path $f -Value $c.code
        $p = Start-Process -FilePath $hao -ArgumentList @("emit", $f) -Wait -PassThru -NoNewWindow -RedirectStandardError "$dir\$safe.err" -RedirectStandardOutput "$dir\$safe.out"
        if ($p.ExitCode -eq 0) {
            Write-Host "FAIL 应拒绝但通过: $($c.name)"
            $fail++
        } else {
            Write-Host "OK   编译拒绝: $($c.name)"
            $ok++
        }
    }

    # 跨包访问 internal 成员（多文件包）
    $xpkg = Join-Path $dir "internal_xpkg"
    New-Item -ItemType Directory -Force -Path (Join-Path $xpkg "hid") | Out-Null
    Set-Content -Encoding utf8 -Path (Join-Path $xpkg "hid\Box.hao") -Value @"
package hid;
class Box {
    internal var n: Int = 1;
    constructor() {}
    internal func peek(): Int { return this.n; }
}
"@
    Set-Content -Encoding utf8 -Path (Join-Path $xpkg "main.hao") -Value @"
package main;
import hid;
func main() {
    val b = new hid.Box();
    fmt.println(b.n);
}
"@
    $p = Start-Process -FilePath $hao -ArgumentList @("emit", $xpkg) -Wait -PassThru -NoNewWindow -RedirectStandardError "$dir\internal_xpkg.err" -RedirectStandardOutput "$dir\internal_xpkg.out"
    if ($p.ExitCode -eq 0) {
        Write-Host "FAIL 应拒绝但通过: internal xpkg field"
        $fail++
    } else {
        Write-Host "OK   编译拒绝: internal xpkg field"
        $ok++
    }

    Set-Content -Encoding utf8 -Path (Join-Path $xpkg "main.hao") -Value @"
package main;
import hid;
func main() {
    val b = new hid.Box();
    fmt.println(b.peek());
}
"@
    $p = Start-Process -FilePath $hao -ArgumentList @("emit", $xpkg) -Wait -PassThru -NoNewWindow -RedirectStandardError "$dir\internal_xpkg_m.err" -RedirectStandardOutput "$dir\internal_xpkg_m.out"
    if ($p.ExitCode -eq 0) {
        Write-Host "FAIL 应拒绝但通过: internal xpkg method"
        $fail++
    } else {
        Write-Host "OK   编译拒绝: internal xpkg method"
        $ok++
    }

    Write-Host "========================================"
    Write-Host "反向: $ok 拒绝 / $fail 漏拒"
    if ($fail -ne 0) { exit 1 }
    exit 0
}
finally {
    # 用完清空（规范：测试产物不长期留在 target）
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}
