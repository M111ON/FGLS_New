# SID+DRamTile benchmark — Qwen3-4B
$model = "I:\model\Qwen3-4B-Q4_K_M.gguf"
$runner = "I:\FGLS_new\runner\llama_pogls_runner_sid_v2.exe"
$prompt = "What is 2+2?"
$max_new = 10

$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH

Write-Host "`n========================================"
Write-Host "BENCHMARK 1: Baseline (no SID)" -ForegroundColor Cyan
Write-Host "========================================"
$t1 = Measure-Command { & $runner $model --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t1.TotalSeconds.ToString('F2'))s"

Write-Host "`n========================================"
Write-Host "BENCHMARK 2: SID only (CPU)" -ForegroundColor Cyan
Write-Host "========================================"
$t2 = Measure-Command { & $runner $model --sid --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t2.TotalSeconds.ToString('F2'))s"

Write-Host "`n========================================"
Write-Host "BENCHMARK 3: SID + DRamTile (CPU)" -ForegroundColor Cyan
Write-Host "========================================"
$t3 = Measure-Command { & $runner $model --sid --dramtile --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t3.TotalSeconds.ToString('F2'))s"

Write-Host "`n========================================"
Write-Host "BENCHMARK 4: SID + GPU (ngl=24)" -ForegroundColor Cyan
Write-Host "========================================"
$t4 = Measure-Command { & $runner $model --sid --ngl 24 --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t4.TotalSeconds.ToString('F2'))s"

Write-Host "`n========================================"
Write-Host "BENCHMARK 5: SID + GPU + DRamTile" -ForegroundColor Cyan
Write-Host "========================================"
$t5 = Measure-Command { & $runner $model --sid --dramtile --ngl 24 --prompt $prompt --max-new $max_new 2>$null }
Write-Host "  Total: $($t5.TotalSeconds.ToString('F2'))s"

Write-Host "`n========================================"
Write-Host "SUMMARY" -ForegroundColor Green
Write-Host "========================================"
Write-Host "1. Baseline (no SID):              $($t1.TotalSeconds.ToString('F2'))s"
Write-Host "2. SID only (CPU):                 $($t2.TotalSeconds.ToString('F2'))s"
Write-Host "3. SID + DRamTile (CPU):           $($t3.TotalSeconds.ToString('F2'))s"
Write-Host "4. SID + GPU:                      $($t4.TotalSeconds.ToString('F2'))s"
Write-Host "5. SID + GPU + DRamTile:           $($t5.TotalSeconds.ToString('F2'))s"
if ($t2.TotalMilliseconds -gt 0) {
    Write-Host "  DRamTile overhead (CPU):       $(($t3.TotalMilliseconds - $t2.TotalMilliseconds).ToString('+0;-0;0'))ms ($(($t3.TotalMilliseconds/$t2.TotalMilliseconds*100-100).ToString('+0.0;-0.0;0'))%)"
}
if ($t4.TotalMilliseconds -gt 0) {
    Write-Host "  DRamTile overhead (GPU):       $(($t5.TotalMilliseconds - $t4.TotalMilliseconds).ToString('+0;-0;0'))ms ($(($t5.TotalMilliseconds/$t4.TotalMilliseconds*100-100).ToString('+0.0;-0.0;0'))%)"
}
Write-Host "`nDone."
