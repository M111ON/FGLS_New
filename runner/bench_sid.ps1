# SID+DRamTile benchmark
$model = "I:\model\LFM2.5-8B-A1B-Q4_K_M.gguf"
$runner = "I:\FGLS_new\runner\llama_pogls_runner_sid_v2.exe"
$prompt = "What is 2+2?"
$max_new = 10

# Create temp experiment dir with 5 CPLs
$exp_dir = "$env:TEMP\sid_bench_faces"
Remove-Item -Recurse -Force $exp_dir -ErrorAction SilentlyContinue | Out-Null
New-Item -ItemType Directory -Path $exp_dir -Force | Out-Null
Get-ChildItem "I:\FGLS_new\runner\cpl_profiles\*.cpl" | Select-Object -First 5 -ExpandProperty FullName | ForEach-Object { Copy-Item $_ $exp_dir\ }
Write-Host "=== CPL files for benchmark ==="
Get-ChildItem $exp_dir -Name

Write-Host "`n========================================"
Write-Host "BENCHMARK 1: Baseline (no SID, no DRamTile)" -ForegroundColor Cyan
Write-Host "========================================"
$t1 = Measure-Command { & $runner $model --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t1.TotalSeconds.ToString('F2'))s | CPU: $($t1.TotalMilliseconds.ToString('F0'))ms"

Write-Host "`n========================================"
Write-Host "BENCHMARK 2: SID face=1 (no DRamTile)" -ForegroundColor Cyan
Write-Host "========================================"
$t2 = Measure-Command { & $runner $model --sid-face 1 --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t2.TotalSeconds.ToString('F2'))s | CPU: $($t2.TotalMilliseconds.ToString('F0'))ms"

Write-Host "`n========================================"
Write-Host "BENCHMARK 3: SID face=1 + DRamTile" -ForegroundColor Cyan
Write-Host "========================================"
$t3 = Measure-Command { & $runner $model --sid-face 1 --dramtile --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t3.TotalSeconds.ToString('F2'))s | CPU: $($t3.TotalMilliseconds.ToString('F0'))ms"

Write-Host "`n========================================"
Write-Host "BENCHMARK 4: SID + GPU offload (ngl=24, no DRamTile)" -ForegroundColor Cyan
Write-Host "========================================"
$t4 = Measure-Command { & $runner $model --sid-face 1 --ngl 24 --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t4.TotalSeconds.ToString('F2'))s | CPU: $($t4.TotalMilliseconds.ToString('F0'))ms"

Write-Host "`n========================================"
Write-Host "BENCHMARK 5: SID + GPU offload + DRamTile" -ForegroundColor Cyan
Write-Host "========================================"
$t5 = Measure-Command { & $runner $model --sid-face 1 --dramtile --ngl 24 --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t5.TotalSeconds.ToString('F2'))s | CPU: $($t5.TotalMilliseconds.ToString('F0'))ms"

Write-Host "`n========================================"
Write-Host "SUMMARY" -ForegroundColor Green
Write-Host "========================================"
Write-Host "1. Baseline (no SID):                $($t1.TotalSeconds.ToString('F2'))s"
Write-Host "2. SID only:                         $($t2.TotalSeconds.ToString('F2'))s"
Write-Host "3. SID + DRamTile:                   $($t3.TotalSeconds.ToString('F2'))s"
Write-Host "4. SID + GPU:                        $($t4.TotalSeconds.ToString('F2'))s"
Write-Host "5. SID + GPU + DRamTile:             $($t5.TotalSeconds.ToString('F2'))s"
if ($t2.TotalMilliseconds -gt 0) {
    Write-Host "  DRamTile overhead (CPU):          $(($t3.TotalMilliseconds - $t2.TotalMilliseconds).ToString('F0'))ms ($(($t3.TotalMilliseconds/$t2.TotalMilliseconds*100-100).ToString('F1'))%)"
}
if ($t4.TotalMilliseconds -gt 0) {
    Write-Host "  DRamTile overhead (GPU):          $(($t5.TotalMilliseconds - $t4.TotalMilliseconds).ToString('F0'))ms ($(($t5.TotalMilliseconds/$t4.TotalMilliseconds*100-100).ToString('F1'))%)"
}

Write-Host "`n========================================"
Write-Host "BENCHMARK 6: 5-face experiment (no DRamTile)" -ForegroundColor Cyan
Write-Host "========================================"
$t6 = Measure-Command { & $runner $model --sid-face 1 --experiment $exp_dir 2>$null }
Write-Host "  Total: $($t6.TotalSeconds.ToString('F2'))s | CPU: $($t6.TotalMilliseconds.ToString('F0'))ms"

Write-Host "`n========================================"
Write-Host "BENCHMARK 7: 5-face experiment + DRamTile" -ForegroundColor Cyan
Write-Host "========================================"
$t7 = Measure-Command { & $runner $model --sid-face 1 --dramtile --experiment $exp_dir 2>$null }
Write-Host "  Total: $($t7.TotalSeconds.ToString('F2'))s | CPU: $($t7.TotalMilliseconds.ToString('F0'))ms"

if ($t6.TotalMilliseconds -gt 0) {
    Write-Host "  5-face DRamTile overhead: $(($t7.TotalMilliseconds - $t6.TotalMilliseconds).ToString('F0'))ms ($(($t7.TotalMilliseconds/$t6.TotalMilliseconds*100-100).ToString('F1'))%)"
}

Remove-Item -Recurse -Force $exp_dir -ErrorAction SilentlyContinue | Out-Null
Write-Host "`nDone."
