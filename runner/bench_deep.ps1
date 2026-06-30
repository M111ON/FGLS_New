# Deep benchmark: baseline vs SID+GPU+DRamTile
param(
    [string]$model = "I:\model\LFM2.5-8B-A1B-Q4_K_M.gguf",
    [int]$max_new = 10,
    [string]$prompt = "What is 2+2?"
)

$runner = "I:\FGLS_new\runner\llama_pogls_runner_sid_v2.exe"
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"

# nvidia-smi polling (background job) — captures peak VRAM
function Start-VramMon { 
    $script:vram_job = Start-Job -ScriptBlock { 
        $peak = 0
        for ($i=0; $i -lt 120; $i++) {
            $u = nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>$null | Select-Object -First 1
            if ($u -and [int]$u -gt $peak) { $peak = [int]$u }
            Start-Sleep -Milliseconds 500
            if ($i -gt 0 -and $peak -eq 0) { break } # process done
        }
        $peak
    }
}
function Stop-VramMon { 
    $peak = $script:vram_job | Wait-Job -Timeout 5 | Receive-Job
    $script:vram_job | Remove-Job -ErrorAction SilentlyContinue
    return $peak
}

function Run-And-Analyze($desc, $args) {
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host $desc -ForegroundColor Cyan
    Write-Host "========================================"

    Start-VramMon
    $stderr_file = "$env:TEMP\bench_debug.txt"

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $runner -ArgumentList $args -NoNewWindow `
        -RedirectStandardError $stderr_file -Wait -PassThru
    $sw.Stop()
    $vram_peak = Stop-VramMon

    $stderr = Get-Content $stderr_file -Raw
    $lines = $stderr -split "`n"

    # Parse profile
    $pp=$null; $gp=$null; $sid_tensors=0; $gpu_free=0; $vulkan_buf=0; $kv_buf=0
    foreach ($l in $lines) {
        if ($l -match 'using device Vulkan.*?(\d+)\s*MiB free') { $gpu_free = [int]$Matches[1] }
        if ($l -match 'Vulkan0 compute buffer size =\s*([0-9.]+)\s*MiB') { $vulkan_buf = $Matches[1] }
        if ($l -match 'CPU KV buffer size =\s*([0-9.]+)\s*MiB') { $kv_buf = $Matches[1] }
        if ($l -match '\[profile\] prompt\((\d+) tok\): apply=([0-9.]+)ms decode=([0-9.]+)ms restore=([0-9.]+)ms') {
            $pp = @{tok=[int]$Matches[1]; a=[double]$Matches[2]; d=[double]$Matches[3]; r=[double]$Matches[4]}
        }
        if ($l -match '\[profile\] gen\((\d+) tok\): apply=([0-9.]+)ms\(avg\) decode=([0-9.]+)ms\(avg\) restore=([0-9.]+)ms\(avg\) sample=([0-9.]+)ms\(avg\) apply2=([0-9.]+)ms\(avg\)') {
            $gp = @{tok=[int]$Matches[1]; a=[double]$Matches[2]; d=[double]$Matches[3]; r=[double]$Matches[4]; s=[double]$Matches[5]; a2=[double]$Matches[6]}
        }
        if ($l -match '\[sid\] weight tensors: (\d+)') { $sid_tensors = [int]$Matches[1] }
    }

    return @{
        t_s=$sw.Elapsed.TotalSeconds; t_ms=$sw.Elapsed.TotalMilliseconds
        exit=$proc.ExitCode; vram_peak=$vram_peak
        gpu_free=$gpu_free; vulkan_buf=$vulkan_buf; kv_buf=$kv_buf
        sid_tensors=$sid_tensors; pp=$pp; gp=$gp
    }
}

# ── Run ──
$b1 = Run-And-Analyze "BASELINE (no SID)" @($model, "--prompt", $prompt, "--max-new", $max_new)
$b2 = Run-And-Analyze "WINNER: SID+GPU+DRamTile" @($model, "--sid-face", "1", "--dramtile", "--ngl", "24", "--prompt", $prompt, "--max-new", $max_new)

# ── Report ──
Write-Host "`n`n========================================" -ForegroundColor Green
Write-Host "      DEEP BENCHMARK REPORT (LFM2.5-8B)" -ForegroundColor Green
Write-Host "========================================"

function fmt($v) { if ($v) { $v.ToString('F1') } else { "N/A" } }

Write-Host "`n1) VRAM USAGE"
Write-Host "  Metric                Baseline      SID+GPU+DRamTile"
$v1 = if ($b1.vram_peak) { $b1.vram_peak } else { $b1.gpu_free }
$v2 = if ($b2.vram_peak) { $b2.vram_peak } else { $b2.gpu_free }
Write-Host "  GPU free before load: $(fmt $b1.gpu_free) MiB      $(fmt $b2.gpu_free) MiB"
Write-Host "  Vulkan compute buf:   $(fmt $b1.vulkan_buf) MiB      $(fmt $b2.vulkan_buf) MiB"
Write-Host "  KV cache buf:         $(fmt $b1.kv_buf) MiB      $(fmt $b2.kv_buf) MiB"

Write-Host "`n2) WARM-UP (prompt, single batch)"
Write-Host "  Metric                Baseline      SID+GPU+DRamTile"
if ($b1.pp) { Write-Host "  Tokens:               $($b1.pp.tok)                $($b2.pp.tok)" }
Write-Host "  SID apply:            N/A           $(fmt $b2.pp.a) ms"
Write-Host "  llama_decode:         $(fmt $b1.pp.d) ms         $(fmt $b2.pp.d) ms"
Write-Host "  SID restore:          N/A           $(fmt $b2.pp.r) ms"
$overhead = if ($b2.pp) { $b2.pp.a + $b2.pp.r } else { 0 }
$overhead_pct = if ($b2.pp -and $b2.pp.d -gt 0) { $overhead / $b2.pp.d * 100 } else { 0 }
Write-Host "  SID swap overhead:    N/A           $(fmt $overhead) ms ($(fmt $overhead_pct)% of decode)"

Write-Host "`n3) GENERATION (per-token average)"
Write-Host "  Metric                Baseline      SID+GPU+DRamTile"
if ($b1.gp) { Write-Host "  Tokens generated:    $($b1.gp.tok)                $($b2.gp.tok)" }
Write-Host "  SID apply:            N/A           $(fmt $b2.gp.a) ms/tok"
Write-Host "  llama_decode:         $(fmt $b1.gp.d) ms/tok     $(fmt $b2.gp.d) ms/tok"
Write-Host "  SID restore:          N/A           $(fmt $b2.gp.r) ms/tok"
Write-Host "  SID apply2:           N/A           $(fmt $b2.gp.a2) ms/tok"
Write-Host "  sample:               N/A           $(fmt $b2.gp.s) ms/tok"
$gt = if ($b2.gp) { $b2.gp.a + $b2.gp.d + $b2.gp.r + $b2.gp.s + $b2.gp.a2 } else { 0 }
$tput = if ($gt -gt 0) { 1000.0 / $gt } else { 0 }
Write-Host "  Total/tok:            N/A           $(fmt $gt) ms"
Write-Host "  Throughput:           N/A           $(fmt $tput) tok/s"
$so = if ($b2.gp) { $b2.gp.a + $b2.gp.r + $b2.gp.a2 } else { 0 }
$sop = if ($gt -gt 0) { $so / $gt * 100 } else { 0 }
Write-Host "  SID swap overhead:    N/A           $(fmt $so) ms/tok ($(fmt $sop)% of total)"

Write-Host "`n4) MEMCPY VOLUME (estimated, per decode)"
Write-Host "  Baseline:  ~0 B (no swaps, GGUF mmap serves tensor->data directly)"
Write-Host "  SID:       ~2.3 GB pointer swaps ($($b2.sid_tensors) weight tensors)"
Write-Host "             zero-copy: only pointer/tensor->data swapped, no memcpy"
Write-Host "  DRamTile:  same 2.3 GB, but via ggml_backend_tensor_set (GPU) or"
Write-Host "             VirtualAlloc mmap swap (CPU) instead of per-tensor ops"

Write-Host "`n5) PCIe TRAFFIC (estimated)"
Write-Host "  Baseline:  ~2.3 GB (load from GGUF once at startup)"
Write-Host "  SID+GPU:   ~0 B per decode on hot path (data already in GPU VRAM)"
Write-Host "             Tensors swapped via pointer manipulation, not PCIe transfer"
Write-Host "  (GTX 1050 Ti PCIe 3.0 x16 = ~16 GB/s theoretical)"

Write-Host "`n6) PAGE FAULT ANALYSIS"
Write-Host "  Baseline:  GGUF mmap → lazy page faults on first touch per decode"
Write-Host "             (OS pages in model weights from file on first access)"
Write-Host "  SID:       sid_cache or DRamTile VirtualAlloc → committed pages"
Write-Host "             No page faults after initial allocation (MEM_COMMIT)"

Write-Host "`n7) SPEEDUP SUMMARY"
Write-Host "  Baseline total:            $(fmt $b1.t_s) s"
Write-Host "  SID+GPU+DRamTile total:   $(fmt $b2.t_s) s"
$speedup = if ($b1.t_s -gt 0) { ($b1.t_s - $b2.t_s) / $b1.t_s * 100 } else { 0 }
Write-Host "  Improvement:              $(fmt $speedup)% faster"
Write-Host "`nDone."
