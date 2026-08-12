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
            var junk = new ArrayList<Object>();
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

# U10: class method body try/finally root smoke
$out10 = & $hao run (Join-Path $Root "test\gc_try_method_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc10 = ([regex]::Matches($out10, '(?m)^true\s*$')).Count
    if ($tc10 -ge 7) {
        Write-Host "OK   gc_try_method_finally_root_smoke true x7"
    } else {
        Write-Host "FAIL method/finally smoke true count=$tc10"
        Write-Host $out10
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_method_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out10
    $fail++
}

# U11: catch-only (no finally) root smoke
$out11 = & $hao run (Join-Path $Root "test\gc_try_catch_only_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc11 = ([regex]::Matches($out11, '(?m)^true\s*$')).Count
    if ($tc11 -ge 4) {
        Write-Host "OK   gc_try_catch_only_root_smoke true x4"
    } else {
        Write-Host "FAIL catch-only smoke true count=$tc11"
        Write-Host $out11
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_catch_only_root_smoke exit=$LASTEXITCODE"
    Write-Host $out11
    $fail++
}

# U12: constructor body try/finally root smoke
$out12 = & $hao run (Join-Path $Root "test\gc_try_ctor_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc12 = ([regex]::Matches($out12, '(?m)^true\s*$')).Count
    if ($tc12 -ge 6) {
        Write-Host "OK   gc_try_ctor_finally_root_smoke true x6"
    } else {
        Write-Host "FAIL ctor/finally smoke true count=$tc12"
        Write-Host $out12
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_ctor_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out12
    $fail++
}

# U13: static method body try/finally root smoke
$out13 = & $hao run (Join-Path $Root "test\gc_try_static_method_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc13 = ([regex]::Matches($out13, '(?m)^true\s*$')).Count
    if ($tc13 -ge 7) {
        Write-Host "OK   gc_try_static_method_finally_root_smoke true x7"
    } else {
        Write-Host "FAIL static-method/finally smoke true count=$tc13"
        Write-Host $out13
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_static_method_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out13
    $fail++
}

# U14: haoroutine body try/finally root smoke
$out14 = & $hao run (Join-Path $Root "test\gc_try_haoroutine_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc14 = ([regex]::Matches($out14, '(?m)^true\s*$')).Count
    if ($tc14 -ge 7) {
        Write-Host "OK   gc_try_haoroutine_finally_root_smoke true x7"
    } else {
        Write-Host "FAIL haoroutine/finally smoke true count=$tc14"
        Write-Host $out14
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_haoroutine_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out14
    $fail++
}

# U15: when x for x try/finally root smoke
$out15 = & $hao run (Join-Path $Root "test\gc_try_when_for_finally_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc15 = ([regex]::Matches($out15, '(?m)^true\s*$')).Count
    if ($tc15 -ge 10) {
        Write-Host "OK   gc_try_when_for_finally_root_smoke true x10"
    } else {
        Write-Host "FAIL when/for/finally smoke true count=$tc15"
        Write-Host $out15
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_when_for_finally_root_smoke exit=$LASTEXITCODE"
    Write-Host $out15
    $fail++
}

# U16: nested try inside catch body root smoke
$out16 = & $hao run (Join-Path $Root "test\gc_try_nested_catch_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc16 = ([regex]::Matches($out16, '(?m)^true\s*$')).Count
    if ($tc16 -ge 9) {
        Write-Host "OK   gc_try_nested_catch_root_smoke true x9"
    } else {
        Write-Host "FAIL nested-catch smoke true count=$tc16"
        Write-Host $out16
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_nested_catch_root_smoke exit=$LASTEXITCODE"
    Write-Host $out16
    $fail++
}

# U17: catch-body finally throw (reason=4 / catchDepth_) root smoke
$out17 = & $hao run (Join-Path $Root "test\gc_try_catch_finally_throw_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc17 = ([regex]::Matches($out17, '(?m)^true\s*$')).Count
    if ($tc17 -ge 7) {
        Write-Host "OK   gc_try_catch_finally_throw_root_smoke true x7"
    } else {
        Write-Host "FAIL catch-finally-throw smoke true count=$tc17"
        Write-Host $out17
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_catch_finally_throw_root_smoke exit=$LASTEXITCODE"
    Write-Host $out17
    $fail++
}

# U18: select inside catch body root smoke
$out18 = & $hao run (Join-Path $Root "test\gc_try_select_in_catch_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc18 = ([regex]::Matches($out18, '(?m)^true\s*$')).Count
    if ($tc18 -ge 8) {
        Write-Host "OK   gc_try_select_in_catch_root_smoke true x8"
    } else {
        Write-Host "FAIL select-in-catch smoke true count=$tc18"
        Write-Host $out18
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_select_in_catch_root_smoke exit=$LASTEXITCODE"
    Write-Host $out18
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
    var xs = new ArrayList<Object>()
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

# A8: HAO_IRGEN_TRACE=1 recycle_spill on for/continue emit
@'
func main() {
    for (i in [0, 1, 2]) {
        var junk = "x"
        if (i == 0) { continue }
        fmt.println(junk == "x")
    }
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "recycle_for.hao")
$prevTrace2 = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut2 = & $hao emit (Join-Path $td "recycle_for.hao") -o (Join-Path $td "recycle_for.ll") 2>&1 | Out-String
$traceEc2 = $LASTEXITCODE
if ($null -ne $prevTrace2) { $env:HAO_IRGEN_TRACE = $prevTrace2 } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc2 -eq 0 -and $traceOut2 -match 'hao:irgen:recycle_spill target=') {
    Write-Host "OK   HAO_IRGEN_TRACE recycle_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE recycle_spill"
    Write-Host $traceOut2
    $fail++
}

# A9: unpin_spill + leave_spill prefixes
if ($traceEc -eq 0 -and $traceOut -match 'hao:irgen:leave_spill base=') {
    Write-Host "OK   HAO_IRGEN_TRACE leave_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE leave_spill"
    Write-Host $traceOut
    $fail++
}
if ($traceEc2 -eq 0 -and $traceOut2 -match 'hao:irgen:unpin_spill sticky=') {
    Write-Host "OK   HAO_IRGEN_TRACE unpin_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE unpin_spill"
    Write-Host $traceOut2
    $fail++
}

# A10: pin_spill + enter_spill prefixes
if ($traceEc -eq 0 -and $traceOut -match 'hao:irgen:enter_spill depth=') {
    Write-Host "OK   HAO_IRGEN_TRACE enter_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE enter_spill"
    Write-Host $traceOut
    $fail++
}
if ($traceEc2 -eq 0 -and $traceOut2 -match 'hao:irgen:pin_spill next=') {
    Write-Host "OK   HAO_IRGEN_TRACE pin_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE pin_spill"
    Write-Host $traceOut2
    $fail++
}

# A11: sticky_floor + block_enter/leave prefixes
if ($traceEc -eq 0 -and $traceOut -match 'hao:irgen:sticky_floor sticky_n=') {
    Write-Host "OK   HAO_IRGEN_TRACE sticky_floor prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE sticky_floor"
    Write-Host $traceOut
    $fail++
}
$prevTrace3 = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut3 = & $hao emit (Join-Path $Root "test\gc_block_scope_root_smoke.hao") -o (Join-Path $td "block_trace.ll") 2>&1 | Out-String
$traceEc3 = $LASTEXITCODE
if ($null -ne $prevTrace3) { $env:HAO_IRGEN_TRACE = $prevTrace3 } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc3 -eq 0 -and $traceOut3 -match 'hao:irgen:block_enter depth=' -and $traceOut3 -match 'hao:irgen:block_leave depth=') {
    Write-Host "OK   HAO_IRGEN_TRACE block_enter/leave prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE block_enter/leave"
    Write-Host $traceOut3
    $fail++
}

# A12: grow_spill — force pool expand past prealloc (48); nest in if so vars are not loop-hoisted
$growSrc = @'
func main() {
    var i = 0
    while (i < 1) {
        if (true) {
            var s00 = "00"; var s01 = "01"; var s02 = "02"; var s03 = "03"; var s04 = "04"
            var s05 = "05"; var s06 = "06"; var s07 = "07"; var s08 = "08"; var s09 = "09"
            var s10 = "10"; var s11 = "11"; var s12 = "12"; var s13 = "13"; var s14 = "14"
            var s15 = "15"; var s16 = "16"; var s17 = "17"; var s18 = "18"; var s19 = "19"
            var s20 = "20"; var s21 = "21"; var s22 = "22"; var s23 = "23"; var s24 = "24"
            var s25 = "25"; var s26 = "26"; var s27 = "27"; var s28 = "28"; var s29 = "29"
            var s30 = "30"; var s31 = "31"; var s32 = "32"; var s33 = "33"; var s34 = "34"
            var s35 = "35"; var s36 = "36"; var s37 = "37"; var s38 = "38"; var s39 = "39"
            var s40 = "40"; var s41 = "41"; var s42 = "42"; var s43 = "43"; var s44 = "44"
            var s45 = "45"; var s46 = "46"; var s47 = "47"; var s48 = "48"; var s49 = "49"
            fmt.println(s00.length + s49.length > 0)
        }
        i = i + 1
    }
}
'@
$growSrc | Set-Content -Encoding utf8 (Join-Path $td "grow_spill.hao")
$prevTrace4 = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut4 = & $hao emit (Join-Path $td "grow_spill.hao") -o (Join-Path $td "grow_spill.ll") 2>&1 | Out-String
$traceEc4 = $LASTEXITCODE
if ($null -ne $prevTrace4) { $env:HAO_IRGEN_TRACE = $prevTrace4 } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc4 -eq 0 -and $traceOut4 -match 'hao:irgen:grow_spill next=') {
    Write-Host "OK   HAO_IRGEN_TRACE grow_spill prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE grow_spill"
    Write-Host $traceOut4
    $fail++
}

# A13: unwind / catch_enter / catch_leave / gc_unwind prefixes
$prevTrace5 = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut5 = & $hao emit (Join-Path $Root "test\gc_try_catch_finally_throw_root_smoke.hao") -o (Join-Path $td "unwind_trace.ll") 2>&1 | Out-String
$traceEc5 = $LASTEXITCODE
if ($null -ne $prevTrace5) { $env:HAO_IRGEN_TRACE = $prevTrace5 } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc5 -eq 0 -and
    $traceOut5 -match 'hao:irgen:unwind reason=' -and
    $traceOut5 -match 'hao:irgen:catch_enter depth=' -and
    $traceOut5 -match 'hao:irgen:catch_leave depth=' -and
    $traceOut5 -match 'hao:irgen:gc_unwind wm=') {
    Write-Host "OK   HAO_IRGEN_TRACE unwind/catch/gc_unwind prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE unwind/catch/gc_unwind"
    Write-Host $traceOut5
    $fail++
}

# A14: note_block + unwind_gc prefixes
$prevTrace6 = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut6 = & $hao emit (Join-Path $Root "test\gc_try_select_in_catch_root_smoke.hao") -o (Join-Path $td "note_unwind_gc.ll") 2>&1 | Out-String
$traceEc6 = $LASTEXITCODE
if ($null -ne $prevTrace6) { $env:HAO_IRGEN_TRACE = $prevTrace6 } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc6 -eq 0 -and
    $traceOut6 -match 'hao:irgen:note_block depth=' -and
    $traceOut6 -match 'hao:irgen:unwind_gc ptr=') {
    Write-Host "OK   HAO_IRGEN_TRACE note_block/unwind_gc prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE note_block/unwind_gc"
    Write-Host $traceOut6
    $fail++
}

# A15: leave_spill noop + acquire_spill pool=0
@'
class Box {
    public var s: String = "x"
}
func main() {
    var b = new Box()
    fmt.println(b.s == "x")
}
'@ | Set-Content -Encoding utf8 (Join-Path $td "nopool_acquire.hao")
$prevTrace7 = $env:HAO_IRGEN_TRACE
$env:HAO_IRGEN_TRACE = "1"
$traceOut7 = & $hao emit (Join-Path $td "nopool_acquire.hao") -o (Join-Path $td "nopool_acquire.ll") 2>&1 | Out-String
$traceEc7 = $LASTEXITCODE
if ($null -ne $prevTrace7) { $env:HAO_IRGEN_TRACE = $prevTrace7 } else { Remove-Item Env:HAO_IRGEN_TRACE -ErrorAction SilentlyContinue }
if ($traceEc7 -eq 0 -and $traceOut7 -match 'hao:irgen:acquire_spill pool=0 hint=') {
    Write-Host "OK   HAO_IRGEN_TRACE acquire_spill pool=0 prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE acquire_spill pool=0"
    Write-Host $traceOut7
    $fail++
}
if (($traceEc -eq 0 -and $traceOut -match 'hao:irgen:leave_spill noop=1') -or
    ($traceEc7 -eq 0 -and $traceOut7 -match 'hao:irgen:leave_spill noop=1')) {
    Write-Host "OK   HAO_IRGEN_TRACE leave_spill noop prefix"
} else {
    Write-Host "FAIL HAO_IRGEN_TRACE leave_spill noop"
    Write-Host $traceOut
    Write-Host $traceOut7
    $fail++
}

# U19: for-in inside catch body root smoke
$out19 = & $hao run (Join-Path $Root "test\gc_try_for_in_catch_root_smoke.hao") 2>&1 | Out-String
if ($LASTEXITCODE -eq 0) {
    $tc19 = ([regex]::Matches($out19, '(?m)^true\s*$')).Count
    if ($tc19 -ge 12) {
        Write-Host "OK   gc_try_for_in_catch_root_smoke true x12"
    } else {
        Write-Host "FAIL for-in-catch smoke true count=$tc19"
        Write-Host $out19
        $fail++
    }
} else {
    Write-Host "FAIL gc_try_for_in_catch_root_smoke exit=$LASTEXITCODE"
    Write-Host $out19
    $fail++
}

if ($fail -eq 0) {
    Write-Host "spill_ir_smoke: ALL PASS"
    exit 0
}
Write-Host "spill_ir_smoke: $fail FAIL(s)"
exit 1

