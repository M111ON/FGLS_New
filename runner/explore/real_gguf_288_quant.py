"""
Test 288-bucket quantization on REAL GGUF weights (SmolLM2-360M Q8_0).
Compare: 256 (baseline), 288, 360 buckets.
Two modes: raw uint8 codes, and real float32 dequantized Q8_0 weights.
"""
import numpy as np, struct, os, math

GGUF = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
FILE_SIZE = os.path.getsize(GGUF)
print(f"GGUF: {FILE_SIZE / 1024 / 1024:.1f} MB")

# ═══════════════════════════════════════════════════════
# Find actual data section start (where Q8_0 blocks begin)
# ═══════════════════════════════════════════════════════
def find_data_offset(path, scan_bytes=10*1024*1024):
    """Scan for 32-byte aligned position with max consecutive valid Q8_0 blocks."""
    def is_valid_f16(u16_val):
        b = u16_val.to_bytes(2, byteorder='little')
        f16 = float(np.frombuffer(b, dtype=np.float16)[0])
        return not np.isnan(f16) and not np.isinf(f16) and 0.0001 < abs(f16) < 1000
    
    with open(path, 'rb') as f:
        chunk = f.read(scan_bytes)
    
    best, best_n = 0, 0
    for a in range(0, len(chunk) - 34*500, 32):
        n = 0
        for i in range(500):
            b = a + i * 34
            if b + 2 > len(chunk): break
            su = struct.unpack_from('<H', chunk, b)[0]
            if is_valid_f16(su): n += 1
            else: break
        if n > best_n:
            best_n, best = n, a
    return best

DATA_OFFSET = find_data_offset(GGUF)
print(f"Data section starts at: {DATA_OFFSET} bytes ({DATA_OFFSET/(1024*1024):.2f} MB)")
print(f"  ({DATA_OFFSET // 32} × 32-byte alignment)\n")

# ═══════════════════════════════════════════════════════
# Q8_0 block reader
# ═══════════════════════════════════════════════════════
BLOCK_SIZE = 34  # 2 bytes scale (f16) + 32 bytes int8

def read_q80_blocks(path, data_offset, n_blocks):
    """Read n_blocks Q8_0 blocks starting at data_offset. Returns scales, weights."""
    raw = np.frombuffer(
        open(path, 'rb').read() if False else b'', dtype=np.uint8
    )  # placeholder
    with open(path, 'rb') as f:
        f.seek(data_offset)
        raw = f.read(n_blocks * BLOCK_SIZE)
    
    scales = np.zeros(n_blocks, dtype=np.float32)
    weights = np.zeros((n_blocks, 32), dtype=np.int8)
    
    for i in range(n_blocks):
        base = i * BLOCK_SIZE
        su = struct.unpack_from('<H', raw, base)[0]
        sb = su.to_bytes(2, byteorder='little')
        scales[i] = float(np.frombuffer(sb, dtype=np.float16)[0])
        weights[i] = np.frombuffer(raw[base+2:base+34], dtype=np.int8)
    
    return scales, weights

def dequantize_q80(scales, weights):
    """Q8_0 dequant: float32 = scale * int8 / 128.0"""
    return (scales[:, None] * weights.astype(np.float32) / 128.0).flatten()

def quantize_float_to_buckets(f32, n_buckets):
    """Symmetric N-bucket quantization on float32."""
    amax = max(np.abs(f32).max(), 1e-10)
    norm = (f32 + amax) / (2 * amax)
    buckets = np.clip(np.round(norm * (n_buckets - 1)), 0, n_buckets - 1)
    return buckets / (n_buckets - 1) * (2 * amax) - amax

def mse(a, b):
    return float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))

def max_err(a, b):
    return float(np.max(np.abs(a.astype(np.float64) - b.astype(np.float64))))

def psnr(a, b, peak=None):
    if peak is None:
        peak = max(np.abs(a).max(), np.abs(b).max(), 1e-10)
    m = mse(a, b)
    return 999.0 if m < 1e-20 else float(10 * math.log10(peak ** 2 / m))

bits_per = {256: 8.0, 288: math.log2(288), 360: math.log2(360)}

# ═══════════════════════════════════════════════════════
# PART 1: Float32 Q8_0 dequant → N-bucket quantization
# ═══════════════════════════════════════════════════════
print("=" * 90)
print("PART 1: Real Q8_0 float32 dequantized weights → N-bucket quantization")
print("=" * 90)

# Read blocks from data section, at various sub-offsets
# Each block is 34 bytes, so sub-offsets are in units of 34
offsets_blocks = [0, 5000, 25000, 50000, 150000]
offset_labels = ["start", "+170KB", "+850KB", "+1.7MB", "+5.1MB"]
n_blocks = 500  # 16000 float32 values per sample
bucket_counts = [256, 288, 360]

results = []

print(f"\n{'Sample':>12} | {'Buckets':>7} | {'MSE':>12} | {'MaxErr':>10} | {'PSNR(dB)':>9} | {'bits/e':>6} | {'vs Q8_0':>7}")
print("-" * 80)

for idx, (blks_off, label) in enumerate(zip(offsets_blocks, offset_labels)):
    pos = DATA_OFFSET + blks_off * BLOCK_SIZE
    if pos + n_blocks * BLOCK_SIZE > FILE_SIZE:
        print(f"  {label:>10} — SKIPPED (beyond file)")
        continue
    
    scales, weights = read_q80_blocks(GGUF, pos, n_blocks)
    f32 = dequantize_q80(scales, weights)
    peak = max(np.abs(f32).max(), 1e-10)
    
    for nb in bucket_counts:
        rec = quantize_float_to_buckets(f32, nb)
        m = mse(f32, rec)
        me = max_err(f32, rec)
        p = psnr(f32, rec, peak)
        bpe = bits_per[nb]
        ratio = bpe / 8.0
        print(f"  {label:>10} | {nb:>7} | {m:>12.6e} | {me:>10.6f} | {p:>9.2f} | {bpe:>6.3f} | {ratio:>6.3f}x")
        results.append((label, nb, m, me, p, bpe, ratio))

# ═══════════════════════════════════════════════════════
# PART 2: Lossy compression — fewer bits budget
# ═══════════════════════════════════════════════════════
print("\n" + "=" * 90)
print("PART 2: Compression at fixed bit budgets (2^N buckets → N bits/elem)")
print("=" * 90)
print(f"\n{'bits/e':>7} | {'Buckets':>7} | {'avg MSE':>12} | {'avg MaxErr':>10} | {'avg PSNR':>9}")
print("-" * 55)

for budget in [5, 6, 7, 8, 9, 10]:
    nb = 2 ** budget
    mses, mes, ps = [], [], []
    
    for blks_off in offsets_blocks:
        pos = DATA_OFFSET + blks_off * BLOCK_SIZE
        if pos + n_blocks * BLOCK_SIZE > FILE_SIZE:
            continue
        sc, wt = read_q80_blocks(GGUF, pos, n_blocks)
        f32 = dequantize_q80(sc, wt)
        peak = max(np.abs(f32).max(), 1e-10)
        rec = quantize_float_to_buckets(f32, nb)
        mses.append(mse(f32, rec))
        mes.append(max_err(f32, rec))
        ps.append(psnr(f32, rec, peak))
    
    if mses:
        print(f"{budget:>7} | {nb:>7} | {np.mean(mses):>12.6e} | {np.mean(mes):>10.6f} | {np.mean(ps):>8.2f}dB")

# ═══════════════════════════════════════════════════════
# PART 3: Distribution analysis
# ═══════════════════════════════════════════════════════
print("\n" + "=" * 90)
print("PART 3: Q8_0 weight distribution (sample from data start)")
print("=" * 90)

sc, wt = read_q80_blocks(GGUF, DATA_OFFSET, n_blocks)
f32 = dequantize_q80(sc, wt)
peak = max(np.abs(f32).max(), 1e-10)

print(f"  n_values:  {len(f32):,}")
print(f"  range:     [{f32.min():.6f}, {f32.max():.6f}]")
print(f"  |peak|:    {peak:.6f}")
print(f"  mean:      {f32.mean():.6f}")
print(f"  std:       {f32.std():.6f}")
print(f"  |mean|/std:{abs(f32.mean())/max(f32.std(),1e-10):.3f}")
print()

for nb in bucket_counts:
    rec = quantize_float_to_buckets(f32, nb)
    err = f32 - rec
    print(f"  {nb:>4} buckets: MSE={mse(f32,rec):.6e}  MaxErr={max_err(f32,rec):.6f}  "
          f"PSNR={psnr(f32,rec,peak):.2f}dB  |err|_mean={np.abs(err).mean():.6f}")

# ═══════════════════════════════════════════════════════
# FINAL SUMMARY
# ═══════════════════════════════════════════════════════
print("\n" + "=" * 90)
print("FINAL SUMMARY: 288-bucket quantization on real GGUF (SmolLM2-360M Q8_0)")
print("=" * 90)
print()
print("Averaged across 5 offset samples:")
print()
print(f"  {'Method':>30} | {'bits/e':>6} | {'MSE':>12} | {'MaxErr':>10} | {'PSNR':>9} | {'vs Q8_0':>7}")
print(f"  {'-'*30}-+-{'-'*6}-+-{'-'*12}-+-{'-'*10}-+-{'-'*9}-+-{'-'*7}")

for nb in bucket_counts:
    subset = [r for r in results if r[1] == nb]
    am = np.mean([r[2] for r in subset])
    ae = np.mean([r[3] for r in subset])
    ap = np.mean([r[4] for r in subset])
    bpe = bits_per[nb]
    ratio = bpe / 8.0
    tag = f"{nb}-bucket quant"
    if nb == 256: tag += " (baseline)"
    print(f"  {tag:>30} | {bpe:>6.3f} | {am:>12.6e} | {ae:>10.6f} | {ap:>8.2f}dB | {ratio:>6.3f}x")

print()
print("KEY FINDINGS:")
print(f"  • Q8_0 baseline: 256 buckets = 8.0 bits/elem")
print(f"  • 288 buckets:   {bits_per[288]:.4f} bits/elem (+{(bits_per[288]/8.0-1)*100:.1f}% storage)")
print(f"  • 360 buckets:   {bits_per[360]:.4f} bits/elem (+{(bits_per[360]/8.0-1)*100:.1f}% storage)")
print()

# Compute quality difference
subset_256 = [r for r in results if r[1] == 256]
subset_288 = [r for r in results if r[1] == 288]
subset_360 = [r for r in results if r[1] == 360]

mse_256 = np.mean([r[2] for r in subset_256])
mse_288 = np.mean([r[2] for r in subset_288])
mse_360 = np.mean([r[2] for r in subset_360])
psnr_256 = np.mean([r[4] for r in subset_256])
psnr_288 = np.mean([r[4] for r in subset_288])
psnr_360 = np.mean([r[4] for r in subset_360])

print(f"  • 288 vs 256: MSE improvement = {(1 - mse_288/mse_256)*100:.2f}%, PSNR gain = +{psnr_288 - psnr_256:.2f} dB")
print(f"  • 360 vs 256: MSE improvement = {(1 - mse_360/mse_256)*100:.2f}%, PSNR gain = +{psnr_360 - psnr_256:.2f} dB")
print(f"  • 288 buckets = 12.5% more codebook entries than 256, for +2.1% storage")
print(f"  • For Q8_0 weight storage, 288 buckets offers a fine-grained tradeoff")
print(f"    between compression ratio and reconstruction quality")
