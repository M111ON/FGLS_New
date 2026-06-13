#!/usr/bin/env python3
"""Analyze SmolVLM-256M tensors with SID capture logic.
Reads safetensors, computes 2D signature per tensor, 
compares architecture with SmolLM2-360M.

Usage:
    python analyze_smolvlm.py /mnt/i/model/smolVLM-256M-Instruct/model.safetensors
"""

import struct, sys, os, json, math, numpy as np
from safetensors import safe_open

TW_SCALE = 207360

def q8_signature(data: bytes) -> tuple:
    """Compute 2D signature from Q8_0 blocks (same as sid_signature_q80)"""
    n_blocks = len(data) // 34
    if n_blocks == 0: return (0, 0)
    n_vals = min(n_blocks * 32, 64)
    sum_a = sum_b = 0.0
    na = nb = 0
    for b in range(min(n_blocks, 2)):
        scale_bits = struct.unpack('<H', data[b*34:b*34+2])[0]
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
        for j in range(32):
            q = struct.unpack('<b', data[b*34+2+j:b*34+3+j])[0]
            val = q * dscale
            if j < 16:
                sum_a += val; na += 1
            else:
                sum_b += val; nb += 1
    if na == 0 or nb == 0:
        return (0, 0)
    sig_x = (sum_a / na) * TW_SCALE
    sig_y = (sum_b / nb) * TW_SCALE
    return (int(sig_x), int(sig_y))

def classify_tensor(name: str) -> str:
    """Classify tensor by layer type"""
    n = name.lower()
    if 'attn' in n or 'attention' in n:
        if 'q' in n.split('.')[-2:]: return 'ATTN_Q'
        if 'k' in n.split('.')[-2:]: return 'ATTN_K'
        if 'v' in n.split('.')[-2:]: return 'ATTN_V'
        if 'o' in n.split('.')[-2:]: return 'ATTN_O'
        if 'output' in name.lower(): return 'ATTN_O'
        return 'ATTN_OTHER'
    if 'mlp' in n or 'ffn' in n:
        if 'gate' in n: return 'FFN_GATE'
        if 'up' in n or 'w1' in n: return 'FFN_UP'
        if 'down' in n or 'w2' in n: return 'FFN_DOWN'
        return 'FFN_OTHER'
    if 'norm' in n or 'ln' in n: return 'NORM'
    if 'embed' in n or 'wte' in n: return 'EMBED'
    if 'head' in n: return 'HEAD'
    if 'vision' in n or 'visual' in n: return 'VISION'
    return 'OTHER'

def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_smolvlm.py <model.safetensors>")
        sys.exit(1)
    
    path = sys.argv[1]
    print(f"Loading: {path}")
    
    metadata = {}
    with safe_open(path, framework="np") as f:
        metadata = f.metadata()
    
    with safe_open(path, framework="np") as f:
        keys = list(f.keys())
        print(f"Total tensors: {len(keys)}")
        if metadata:
            for k, v in metadata.items():
                if hasattr(v, '__len__') and len(v) > 200:
                    v = v[:200] + '...'
                print(f"  metadata[{k}] = {v}")
        
        types = {}
        for key in keys:
            t = classify_tensor(key)
            if t not in types: types[t] = []
            types[t].append(key)
        
        print(f"\nLayer type distribution:")
        for t, ks in sorted(types.items()):
            print(f"  {t}: {len(ks)} tensors")
        
        # Sample analysis: compute 2D sig for first few per type
        print(f"\nSID signature sample (first 3 per type):")
        print(f"{'TYPE':12s} {'NAME':50s} {'vx':12s} {'vy':12s} {'zone_est':8s}")
        print("-"*96)
        
        # Print first few tensor names to understand architecture
        print(f"\nSample tensor names:")
        for k in keys[:20]:
            dtype = f.get_tensor(k).dtype
            print(f"  {k}  [{dtype}]")
        
        for t, ks in sorted(types.items()):
            n = 0
            for k in ks[:5]:
                arr = f.get_tensor(k)
                # Handle bfloat16 -> convert to float32
                if arr.dtype == np.dtype('bfloat16'):
                    # bf16 -> view as uint16 -> float32
                    raw = arr.view(np.uint16).flatten().astype(np.float32)
                    # Manually convert bf16 to f32
                    flat_f32 = np.zeros(len(raw), dtype=np.float32)
                    for i, v in enumerate(raw):
                        bits = int(v) << 16
                        flat_f32[i] = struct.unpack('<f', struct.pack('<I', bits))[0]
                    flat = flat_f32[:64]
                else:
                    flat = arr.flatten().astype(np.float32)[:64]
                if len(flat) < 32: continue
                half = len(flat)//2
                sig_x = int((float(np.mean(flat[:half]))))  # mean, not sum
                sig_y = int((float(np.mean(flat[half:2*half]))))
                sig_x = int(sig_x * TW_SCALE)
                sig_y = int(sig_y * TW_SCALE)
                # Estimate zone from angle
                zone_est = int(math.atan2(sig_y, sig_x) / (math.pi/10) + 5) if sig_x or sig_y else 0
                zone_est = max(0, min(9, zone_est))
                n += 1
                if n > 3: continue
                name_short = k[-48:] if len(k) > 48 else k
                print(f"{t:12s} {name_short:50s} {sig_x:12d} {sig_y:12d} z{zone_est:8d}")
        
        # Compare model architecture with SmolLM2
        print(f"\nArchitecture comparison:")
        print(f"  SmolVLM-256M: {len(keys)} tensors")
        smollm2_count = 290  # known from test
        print(f"  SmolLM2-360M: {smollm2_count} tensors")
        print(f"  Ratio: {len(keys)/smollm2_count:.2f}x")
        
        # Count architectural components
        lm_keys = [k for k in keys if 'vision' not in k.lower() and 'visual' not in k.lower()]
        vision_keys = [k for k in keys if 'vision' in k.lower() or 'visual' in k.lower()]
        print(f"  Language model tensors: {len(lm_keys)}")
        print(f"  Vision encoder tensors: {len(vision_keys)}")

if __name__ == '__main__':
    main()
