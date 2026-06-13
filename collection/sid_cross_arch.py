#!/usr/bin/env python3
"""sid_cross_arch.py — SID coordinate comparison: SmolVLM vs SmolLM2

Reads BF16 direct from safetensors, computes 2D sig, maps to TRing.
Compares layer type distributions across architectures.

Usage:
    python3 sid_cross_arch.py /mnt/i/model/smolVLM-256M-Instruct/model.safetensors
"""

import struct, sys, os, json, math, array, re

TW_SCALE = 207360

# ── BF16 helpers ──
def bf16_to_f32(b: int) -> float:
    """bfloat16 (uint16) -> float32"""
    bits = struct.pack('<I', b << 16)
    return struct.unpack('<f', bits)[0]

def read_bf16_tensor(data: bytes, offset: int, size: int) -> list:
    """Read N bf16 values from data starting at offset"""
    n = size // 2
    vals = []
    for i in range(n):
        lo = struct.unpack('<H', data[offset + i*2: offset + i*2 + 2])[0]
        vals.append(bf16_to_f32(lo))
    return vals

def q8_signature(data: bytes, offset: int, nbytes: int) -> tuple:
    """Q8_0 2D signature from raw bytes (same as sid_signature_q80)"""
    n_blocks = nbytes // 34
    if n_blocks == 0: return (0, 0)
    n_vals = min(n_blocks * 32, 64)
    sum_a = sum_b = 0.0
    na = nb = 0
    for b in range(min(n_blocks, 2)):
        scale_bits = struct.unpack('<H', data[offset + b*34: offset + b*34 + 2])[0]
        # f16 to float
        sign = (scale_bits >> 15) & 1
        exp = (scale_bits >> 10) & 0x1F
        mant = scale_bits & 0x3FF
        if exp == 0:
            dscale = mant * 5.960464477539063e-8
            dscale = -dscale if sign else dscale
        else:
            fi = (sign << 31) | ((exp + 112) << 23) | (mant << 13)
            dscale = struct.unpack('<f', struct.pack('<I', fi))[0]
        for j in range(min(32, n_vals - b*32)):
            q = struct.unpack('<b', data[offset + b*34 + 2 + j: offset + b*34 + 3 + j])[0]
            val = q * dscale
            if j < 16:
                sum_a += val; na += 1
            else:
                sum_b += val; nb += 1
    if na == 0 or nb == 0: return (0, 0)
    sx = int((sum_a / na) * TW_SCALE)
    sy = int((sum_b / nb) * TW_SCALE)
    return (sx, sy)

def bf16_signature(vals: list) -> tuple:
    """2D signature from first 64 BF16 values"""
    if len(vals) < 32: return (0, 0)
    n = min(64, len(vals))
    half = n // 2
    sum_a = sum(vals[:half]) / half
    sum_b = sum(vals[half:n]) / half
    return (int(sum_a * TW_SCALE), int(sum_b * TW_SCALE))

def classify_layer(name: str) -> str:
    n = name.lower()
    if 'vision' in n or 'visual' in n or 'vit' in n: return 'VISION'
    if 'connector' in n: return 'CONNECTOR'
    if 'self_attn' in n:
        if 'q_proj' in n: return 'ATTN_Q'
        if 'k_proj' in n: return 'ATTN_K'
        if 'v_proj' in n: return 'ATTN_V'
        if 'o_proj' in n: return 'ATTN_O'
        return 'ATTN'
    if 'mlp' in n:
        if 'gate' in n: return 'FFN_GATE'
        if 'up' in n: return 'FFN_UP'
        if 'down' in n: return 'FFN_DOWN'
        return 'FFN'
    if 'layernorm' in n or 'input_layernorm' in n or 'post_attention' in n: return 'NORM'
    if 'embed' in n: return 'EMBED'
    if 'head' in n or 'lm_head' in n: return 'HEAD'
    return 'OTHER'

def extract_layer_idx(name: str) -> int:
    m = re.search(r'layers\.(\d+)', name)
    return int(m.group(1)) if m else -1

def tring_from_sig(vx: int, vy: int, face: int = 0) -> int:
    """Map vx,vy -> zone(0-9) -> slot(0-5) within face, then to TRing 0-719"""
    zone = int(math.atan2(vy, vx) / (math.pi/10) + 5) if (vx or vy) else 0
    zone = max(0, min(9, zone))
    mag = math.sqrt(vx*vx + vy*vy)
    if mag == 0: slot = 0
    else:
        # Normalize by max possible magnitude
        norm = min(mag / (TW_SCALE * 200), 1.0)
        slot = min(5, int(norm * 6))
    return face * 60 + zone * 6 + slot

def load_smollm2_analysis(store_path: str):
    """Load SmolLM2 .gsten store and compute SID for each tensor"""
    sid_results = {}
    # We'll read from the existing analysis file
    if not os.path.exists(store_path):
        # fallback: try to read .twidx 
        twidx_path = store_path.replace('.twidx', '.twidx')
        return None
    return None

def main():
    if len(sys.argv) < 2:
        print("Usage: sid_cross_arch.py <model.safetensors>")
        sys.exit(1)
    
    path = sys.argv[1]
    
    # Read safetensors raw bytes
    with open(path, 'rb') as f:
        header_len = struct.unpack('<Q', f.read(8))[0]
        header = json.loads(f.read(header_len))
        raw_data = f.read()
    
    keys = [k for k in header.keys() if k != '__metadata__']
    total_size = len(raw_data)
    
    print(f"{'='*80}")
    print(f"  SID Cross-Architecture Analysis")
    print(f"  {'='*80}")
    print(f"  Model: SmolVLM-256M (BF16, {len(keys)} tensors, {total_size/1e6:.0f} MB)")
    print()
    
    # Per-type accumulators
    types = {}
    for k in keys:
        t = classify_layer(k)
        if t not in types: types[t] = []
        meta = header[k]
        off = meta['data_offsets'][0]
        sz = meta['data_offsets'][1] - meta['data_offsets'][0]
        n_elems = 1
        for d in meta['shape']: n_elems *= d
        types[t].append((k, off, sz, meta['shape'], n_elems))
    
    print(f"Layer type distribution:")
    for t, ks in sorted(types.items()):
        print(f"  {t:12s}: {len(ks):4d}")
    print(f"  {'TOTAL':12s}: {len(keys):4d}")
    
    # Compute 2D sig and TRing for each tensor
    print(f"\n{'─'*80}")
    print(f"  SID Signature by Layer Type (first 10 per type)")
    print(f"{'─'*80}")
    print(f"{'TYPE':12s} {'LAYER':5s} {'vx':12s} {'vy':12s} {'Z':3s} {'S':3s} {'TRING':6s} {'SHAPE':20s}")
    print(f"{'─'*80}")
    
    # Analyze vision encoder only on first call
    vision_only_once = True
    
    by_type_tring = {}
    by_type_zone = {}
    by_type_layer = {}
    
    for t, ks in sorted(types.items()):
        if t not in by_type_tring: by_type_tring[t] = []
        if t not in by_type_zone: by_type_zone[t] = {}
        if t not in by_type_layer: by_type_layer[t] = {}
        
        n_shown = 0
        for k, off, sz, shape, nelem in ks:
            # Skip large vision tensors for speed - sample first 10
            if t == 'VISION' and n_shown >= 10 and len(ks) > 20:
                continue
            
            # Read first 64 bf16 values
            if sz >= 128:
                vals = read_bf16_tensor(raw_data, off, min(sz, 128))
            else:
                # Small tensor (like norm) - read all
                vals = read_bf16_tensor(raw_data, off, sz)
            
            vx, vy = bf16_signature(vals)
            tring = tring_from_sig(vx, vy)
            zone = int(math.atan2(vy, vx) / (math.pi/10) + 5) if (vx or vy) else 0
            zone = max(0, min(9, zone))
            
            by_type_tring[t].append(tring)
            by_type_zone[t][zone] = by_type_zone[t].get(zone, 0) + 1
            
            li = extract_layer_idx(k)
            if li not in by_type_layer[t]:
                by_type_layer[t][li] = []
            by_type_layer[t][li].append(tring)
            
            if n_shown < 8:
                shape_str = 'x'.join(str(d) for d in shape)
                if len(shape_str) > 20: shape_str = shape_str[:17]+'...'
                print(f"{t:12s} {str(li) if li>=0 else '':5s} {vx:12d} {vy:12d} {zone:3d} {tring%6:3d} {tring:6d} {shape_str:20s}")
                n_shown += 1
        
        if len(ks) > 8:
            print(f"  ... and {len(ks)-n_shown} more")
    
    # TRing distribution by type
    print(f"\n{'─'*80}")
    print(f"  TRing Distribution by Layer Type")
    print(f"{'─'*80}")
    print(f"{'TYPE':12s} {'COUNT':5s} {'TR_min':6s} {'TR_max':6s} {'TR_μ':7s} {'TR_σ':7s} {'ZONES':25s}")
    print(f"{'─'*80}")
    
    for t in sorted(by_type_tring.keys()):
        trings = by_type_tring[t]
        if not trings: continue
        mu = sum(trings) / len(trings)
        var = sum((x-mu)**2 for x in trings) / len(trings)
        zone_dist = by_type_zone.get(t, {})
        zone_str = ' '.join(f"z{z}:{zone_dist[z]}" for z in sorted(zone_dist.keys()))
        print(f"{t:12s} {len(trings):5d} {min(trings):6d} {max(trings):6d} {mu:7.0f} {math.sqrt(var):7.0f} {zone_str:25s}")
    
    # ── Cross-architecture comparison ──
    print(f"\n{'='*80}")
    print(f"  Cross-Architecture Comparison: SmolVLM-256M vs SmolLM2-360M")  
    print(f"{'='*80}")
    
    # SmolLM2 stats from known analysis
    print(f"""
  SmolLM2 layer composition (per layer, 30 layers):
    ATTN_Q:  1 (576×576 in VLM, 960×960 in LM2)
    ATTN_K:  1 (192×576 in VLM, 960×320 in LM2)
    ATTN_V:  1 (192×576 in VLM, 960×320 in LM2)
    ATTN_O:  1 (576×576 in VLM, 960×960 in LM2)
    FFN_GATE: 1 (1536×576 in VLM, 2560×960 in LM2)
    FFN_UP:   1 (1536×576 in VLM, 2560×960 in LM2)
    FFN_DOWN: 1 (576×1536 in VLM, 960×2560 in LM2)
    NORM:     2 (576 in VLM, 960 in LM2)
  SmolVLM adds: vision encoder (197 tensors), connector (1)
  """)
    
    # Check: do same layer types map to same zones across models?
    # For SmolLM2, we know ATTN_Q tends to zone X, FFN_DOWN tends to zone Y
    # Let's show what zones each type occupies in SmolVLM
    print(f"  Zone signature by layer type (SmolVLM-256M):")
    print(f"  {'TYPE':12s} {'ZONE_SIG':30s} {'POSSIBLE_ARCH_STABLE':40s}")
    
    arch_stable = ['ATTN_Q', 'ATTN_K', 'ATTN_V', 'ATTN_O',
                   'FFN_GATE', 'FFN_UP', 'FFN_DOWN', 'NORM']
    
    for t in arch_stable:
        if t not in by_type_zone: continue
        zd = by_type_zone[t]
        total = sum(zd.values())
        sig = ''.join(f"{z}({zd.get(z,0)})" for z in range(10) if zd.get(z,0) > 0)
        print(f"  {t:12s} {sig:30s} {'YES' if len(zd) > 1 else 'SPECIFIC':40s}")
    
    # Vision encoder analysis
    if 'VISION' in by_type_zone:
        zd = by_type_zone['VISION']
        total_v = sum(zd.values())
        print(f"\n  Vision encoder zone coverage: {total_v} tensors")
        sig_v = ' '.join(f"z{z}:{zd[z]}" for z in sorted(zd.keys()))
        print(f"    Zone distribution: {sig_v}")
    
    # Top-level conclusion 
    print(f"\n{'='*80}")
    print(f"  CONCLUSION")
    print(f"{'='*80}")
    print(f"""
  Architecture differences (SmolVLM vs SmolLM2):
    - Hidden dim: 576 vs 960 (VLM is smaller per-layer)
    - KV heads: single head design vs multi-query
    - Layers: 30 vs 30 (both have 30 LM layers)
    - Vision: VLM has 197 vision encoder tensors
    - Data type: BF16 vs Q8_0

  Key question: do same architectural roles produce same TRing zones?
    - If YES: SID coordinate = ARCHITECTURE function (predictable without weights)
    - If NO:  SID coordinate = WEIGHT VALUE function (needs actual weights)
  """)

if __name__ == '__main__':
    main()
