# HaoLang spill IR smoke (L3) — nested while must not null outer junk spill in inner cond
# Usage from repo root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\win\spill_ir_smoke.ps1
$ErrorActionPreference = "Continue"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$hao = Join-Path $Root "output\hao.exe"
if (-not (Test-Path $hao)) { $hao = "hao" }

$td = Join-Path $Root "target\repro-spill"
New-Item -ItemType Directory -Force -Path $td | Out-Null
$fail = 0

@'
package main;
import collections.*;
class Box {
    public var s: String = "";
    public constructor(s: String) { this.s = s; }
}
func main() {
    var round = 0;
    while (round < 2) {
        {
            var junk = new List<Object>();
            var i = 0;
            while (i < 1) {
                junk.add(new Box("x"));
                i += 1;
            }
        }
        round += 1;
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "nested.hao")

$llPath = Join-Path $td "nested.ll"
& $hao emit (Join-Path $td "nested.hao") -o $llPath 2>&1 | Out-Null
if (-not (Test-Path $llPath)) {
    Write-Host "FAIL emit nested.ll"
    $fail++
} else {
    $ll = Get-Content $llPath -Raw
    $mainM = [regex]::Match($ll, 'define i32 @main\(\) \{([\s\S]*?)\n\}')
    if (-not $mainM.Success) {
        Write-Host "FAIL no @main in nested.ll"
        $fail++
    } else {
        $main = $mainM.Groups[1].Value
        # CU anti-pattern: inner while.cond has store null to loop.spill (junk killed before body load)
        # Fixed shape: while.cond.* in @main has zero store-null to loop.spill
        $conds = [regex]::Matches($main, '(?m)^(while\.cond\.\d+):([\s\S]*?)(?=^(?:while\.|[a-zA-Z_%].*:))')
        $bad = $false
        foreach ($c in $conds) {
            $name = $c.Groups[1].Value
            $body = $c.Groups[2].Value
            $n = ([regex]::Matches($body, 'store ptr null, ptr %loop\.spill')).Count
            if ($n -gt 0) {
                Write-Host "FAIL $name in @main has $n store-null to loop.spill (CU anti-pattern)"
                $bad = $true
            }
        }
        if (-not $bad) {
            if ($conds.Count -lt 2) {
                Write-Host "FAIL expected >=2 while.cond in @main, got $($conds.Count)"
                $fail++
            } else {
                Write-Host "OK   @main while.cond has no loop.spill null stores ($($conds.Count) conds)"
            }
        } else {
            $fail++
        }
    }
}

$out = & $hao run (Join-Path $Root "test\gc_block_scope_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $trueCount = ([regex]::Matches($out, '(?m)^true\s*$')).Count
    if ($trueCount -ge 3) {
        Write-Host "OK   gc_block_scope_root_smoke true x3"
    } else {
        Write-Host "FAIL block smoke true count=$trueCount"
        Write-Host $out
        $fail++
    }
} else {
    Write-Host "FAIL gc_block_scope_root_smoke exit=$LASTEXITCODE"
    Write-Host $out
    $fail++
}

# U2: try/catch/finally root smoke
$out2 = & $hao run (Join-Path $Root "test\gc_try_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc = ([regex]::Matches($out2, '(?m)^true\s*$')).Count
    if ($tc -ge 5) {
        Write-Host "OK   gc_try_finally_root_smoke true x5"
    } else {
        Write-Host "FAIL try/finally smoke true count=$tc"
        Write-Host $out2
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out2
    $fail++
}

# U3: try x while x finally multilayer root smoke
$out3 = & $hao run (Join-Path $Root "test\gc_try_loop_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc3 = ([regex]::Matches($out3, '(?m)^true\s*$')).Count
    if ($tc3 -ge 10) {
        Write-Host "OK   gc_try_loop_finally_root_smoke true x10"
    } else {
        Write-Host "FAIL try/loop/finally smoke true count=$tc3"
        Write-Host $out3
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_loop_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out3
    $fail++
}

# U4: nested try x try x finally + for x try root smoke
$out4 = & $hao run (Join-Path $Root "test\gc_try_nested_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc4 = ([regex]::Matches($out4, '(?m)^true\s*$')).Count
    if ($tc4 -ge 14) {
        Write-Host "OK   gc_try_nested_finally_root_smoke true x14"
    } else {
        Write-Host "FAIL nested try/finally smoke true count=$tc4"
        Write-Host $out4
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_nested_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out4
    $fail++
}

# U5: return/throw-from-finally / select x try / when x try root smoke
$out5 = & $hao run (Join-Path $Root "test\gc_try_control_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc5 = ([regex]::Matches($out5, '(?m)^true\s*$')).Count
    if ($tc5 -ge 10) {
        Write-Host "OK   gc_try_control_root_smoke true x10"
    } else {
        Write-Host "FAIL control try root smoke true count=$tc5"
        Write-Host $out5
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_control_root_smoke exit=$LASTEXITCODE"
    Write-Host $out5
    $fail++
}

# U6a: break through try/finally root smoke
$out6a = & $hao run (Join-Path $Root "test\gc_try_break_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc6a = ([regex]::Matches($out6a, '(?m)^true\s*$')).Count
    if ($tc6a -ge 6) {
        Write-Host "OK   gc_try_break_finally_root_smoke true x6"
    } else {
        Write-Host "FAIL break/finally smoke true count=$tc6a"
        Write-Host $out6a
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_break_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out6a
    $fail++
}

# U6b: lambda body try/finally root smoke
$out6b = & $hao run (Join-Path $Root "test\gc_try_lambda_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc6b = ([regex]::Matches($out6b, '(?m)^true\s*$')).Count
    if ($tc6b -ge 7) {
        Write-Host "OK   gc_try_lambda_finally_root_smoke true x7"
    } else {
        Write-Host "FAIL lambda/finally smoke true count=$tc6b"
        Write-Host $out6b
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_lambda_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out6b
    $fail++
}

# U7a: thread x try/finally root smoke
$out7a = & $hao run (Join-Path $Root "test\gc_try_thread_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc7a = ([regex]::Matches($out7a, '(?m)^true\s*$')).Count
    if ($tc7a -ge 5) {
        Write-Host "OK   gc_try_thread_finally_root_smoke true x5"
    } else {
        Write-Host "FAIL thread/finally smoke true count=$tc7a"
        Write-Host $out7a
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_thread_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out7a
    $fail++
}

# U7b: rethrow + multilayer finally + for x break x finally
$out7b = & $hao run (Join-Path $Root "test\gc_try_rethrow_multilayer_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc7b = ([regex]::Matches($out7b, '(?m)^true\s*$')).Count
    if ($tc7b -ge 13) {
        Write-Host "OK   gc_try_rethrow_multilayer_root_smoke true x13"
    } else {
        Write-Host "FAIL rethrow/multilayer smoke true count=$tc7b"
        Write-Host $out7b
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_rethrow_multilayer_root_smoke exit=$LASTEXITCODE"
    Write-Host $out7b
    $fail++
}

# U8b: Unit-lambda body try/finally root smoke (no named worker wrap)
$out8b = & $hao run (Join-Path $Root "test\gc_try_unit_lambda_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc8b = ([regex]::Matches($out8b, '(?m)^true\s*$')).Count
    if ($tc8b -ge 5) {
        Write-Host "OK   gc_try_unit_lambda_finally_root_smoke true x5"
    } else {
        Write-Host "FAIL unit-lambda/finally smoke true count=$tc8b"
        Write-Host $out8b
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_unit_lambda_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out8b
    $fail++
}

# H1: HAO_GC_VERIFY=1 normal collect
$prevVerify = $env:HAO_GC_VERIFY
$env:HAO_GC_VERIFY = "1"
@'
import gc
import collections.*
class Box {
    public var s: String = ""
    public constructor(s: String) { this.s = s; }
}
func main() {
    var xs = new List<Object>()
    var i = 0
    while (i < 20) {
        xs.add(new Box(Integer.toStr(i)))
        i += 1
    }
    gc.GC.collect()
    fmt.println(xs.size() >= 20)
    fmt.println(gc.GC.markAbortCycles() == 0)
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "verify_ok.hao")
$vout = & $hao run (Join-Path $td "verify_ok.hao") 2>&1 | Out-String
$vec = $LASTEXITCODE
if ($null -ne $prevVerify) { $env:HAO_GC_VERIFY = $prevVerify } else { Remove-Item Env:HAO_GC_VERIFY -ErrorAction SilentlyContinue }
if ($vec -eq 0 -and ([regex]::Matches($vout, '(?m)^true\s*$')).Count -ge 2) {
    Write-Host "OK   HAO_GC_VERIFY=1 collect"
} else {
    Write-Host "FAIL HAO_GC_VERIFY collect exit=$vec"
    Write-Host $vout
    $fail++
}

# A5: HAO_IRGEN_STRICT=1 must keep existing smokes green
$prevStrict = $env:HAO_IRGEN_STRICT
$env:HAO_IRGEN_STRICT = "1"
$sout = & $hao run (Join-Path $Root "test\gc_block_scope_root_smoke.hao") 2>&1 | Out-String
$sec = $LASTEXITCODE
$sout2 = & $hao run (Join-Path $Root "test\gc_try_control_root_smoke.hao") 2>&1 | Out-String
$sec2 = $LASTEXITCODE
if ($null -ne $prevStrict) { $env:HAO_IRGEN_STRICT = $prevStrict } else { Remove-Item Env:HAO_IRGEN_STRICT -ErrorAction SilentlyContinue }
if ($sec -eq 0 -and $sec2 -eq 0) {
    Write-Host "OK   HAO_IRGEN_STRICT=1 block+control smokes"
} else {
    Write-Host "FAIL HAO_IRGEN_STRICT smokes exit=$sec/$sec2"
    Write-Host $sout
    Write-Host $sout2
    $fail++
}

# A6+A7: HAO_IRGEN_TRACE=1 clear_spill + acquire_spill prefixes on nested while emit
$prevTrace = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut = & $hao emit (Join-Path $td "nested.hao") -o (Join-Path $td "nested_trace.ll") 2>&1 | Out-String
$traceEc = $LASTEXITCODE
if ($null -ne $prevTrace) { $env:HAO_IRGEN_TRACE = $prevTrace } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc -eq 0 -and $traceOut -match 'hao:irgen:clear_spill base=') {
    Write-Host "OK   HAO_IRGEN_TRACE clear_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE clear_spill"
    Write-Host $traceOut
    $fail++
}
if ($traceEc -eq 0 -and $traceOut -match 'hao:irgen:acquire_spill next=') {
    Write-Host "OK   HAO_IRGEN_TRACE acquire_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE acquire_spill"
    Write-Host $traceOut
    $fail++
}

if ($fail -eq 0) {
    Write-Host "spill_ir_smoke: ALL PASS"
    exit 0
}
Write-Host "spill_ir_smoke: $fail FAIL(s)"
exit 1
