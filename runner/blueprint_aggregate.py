#!/usr/bin/env python3
"""Blueprint geometry aggregate — runs integrity test across models, aggregates per-block stats."""
import re, subprocess, os

def run(model, scan_mb):
    """Run blueprint_integrity_test and parse aggregate stats."""
    exe = r"I:/FGLS_new/runner/blueprint_integrity_test.exe"
    p = subprocess.run([exe, model, str(scan_mb)], capture_output=True, timeout=600)
    out = (p.stdout or b"").decode("utf-8", errors="replace") + (p.stderr or b"").decode("utf-8", errors="replace")
    out = out.replace("\r", "\n")  # progress bar overwrites use \r

    stats = {}
    # Line shapes:
    #   "  block_000: avgΔ=0.01% maxΔ=0.05% PSNR=76.2 dB exact=24/32"          (raw Q8 blocks)
    #   "  Q8_0: avgΔ=0.04% maxΔ=0.18% PSNR=57.0 dB exact=23/32 compress=50.0%" (re-quantized formats)
    pat = re.compile(
        r"^\s*(?P<fmt>Q\d+_\w+|block_\d+): avgΔ=([\d.]+)% maxΔ=([\d.]+)% "
        r"PSNR=([\d.]+) dB exact=(\d+)/(\d+)(?: compress=([\d.]+)%)?",
        re.M,
    )
    for m in pat.finditer(out):
        fmt = m.group(1)
        if fmt == "BLOCK_B" or not fmt:
            fmt = "Q8_raw"
        elif fmt.startswith("block_"):
            fmt = "Q8_raw"
        avg, mx, psnr = float(m.group(2)), float(m.group(3)), float(m.group(4))
        exc, tot = int(m.group(5)), int(m.group(6))
        s = stats.setdefault(fmt, {"avg": [], "max": [], "psnr": [], "exact": [], "total": [], "blocks": 0})
        s["avg"].append(avg); s["max"].append(mx); s["psnr"].append(psnr)
        s["exact"].append(exc); s["total"].append(tot); s["blocks"] += 1
    return stats

def summarize(model, scan_mb):
    print(f"\n═══ {os.path.basename(model)} (scan {scan_mb} MB) ═══")
    stats = run(model, scan_mb)
    if not stats:
        print("  (no quantized blocks parsed)")
        return
    for fmt in ("Q8_raw", "Q8_0", "Q5_0", "Q4_0", "Q2_K"):
        s = stats.get(fmt)
        if not s:
            continue
        n = s["blocks"]
        avgA = sum(s["avg"]) / n
        mxA = max(s["max"])
        psnrA = sum(s["psnr"]) / n
        exactA = sum(s["exact"]) / sum(s["total"]) * 100
        print(f"  {fmt:6s}: blocks={n:5d}  avgΔ={avgA:5.3f}%  maxΔ={mxA:5.2f}%  "
              f"PSNR={psnrA:7.1f} dB  exact={exactA:5.1f}%")

if __name__ == "__main__":
    models = [
        (r"I:/model/SmolLM2-360M-Instruct.Q8_0.gguf", 16),
        (r"I:/model/Qwen3-0.6B-Q8_0.gguf", 16),
        (r"I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf", 8),
        (r"I:/model/Kokoro_no_espeak_Q8.gguf", 8),
        (r"I:/model/qwen25_q8.gguf", 8),
    ]
    for m, mb in models:
        summarize(m, mb)
    print("\nDONE")