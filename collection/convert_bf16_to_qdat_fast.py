#!/usr/bin/env python3
"""convert_bf16_to_qdat_fast.py — Fast BF16 → Q8_0 conversion

Uses numpy for vectorized operations. ~2-3x faster than pure Python.
Usage:
    python3 convert_bf16_to_qdat_fast.py model.safetensors output_dir/ [--only-lm]
"""

import struct, sys, os, json, math, time
import numpy as np

GGML_TYPE_Q8_0 = 8

def bf16_to_f32_batch(bf16_bytes):
    """Convert BF16 bytes to float32 numpy array"""
    n = len(bf16_bytes) // 2
    # Read as uint16
    u16 = np.frombuffer(bf16_bytes, dtype=np.uint16).astype(np.uint32)
    # BF16 -> F32: shift left by 16 bits
    f32_bits = u16 << 16
    return f32_bits.view(np.float32)

def quantize_q8_0_np(f32_vals):
    """Quantize float32 array to Q8_0 format.
    Returns: bytes of Q8_0 data
    """
    n = len(f32_vals)
    n_blocks = (n + 31) // 32
    
    # Pad to multiple of 32
    if n % 32 != 0:
        f32_vals = np.pad(f32_vals, (0, 32 - n % 32), mode='constant')
    
    # Reshape to blocks
    blocks = f32_vals.reshape(-1, 32)
    
    # Per-block max abs
    amax = np.max(np.abs(blocks), axis=1)
    
    # Handle zero blocks
    scale = np.where(amax > 0, amax / 127.0, 0.0)
    
    # Quantize
    quants = np.round(blocks / scale[:, np.newaxis]).astype(np.int8)
    quants = np.clip(quants, -128, 127)
    
    # Convert f32 scale to f16
    # f32 bits -> extract sign, exp, mant -> pack as f16
    scale_f32_bits = scale.view(np.uint32)
    sign = (scale_f32_bits >> 31) & 1
    exp = ((scale_f32_bits >> 23) & 0xFF).astype(np.int32) - 112
    mant = (scale_f32_bits >> 13) & 0x3FF
    
    # Clamp exp to valid f16 range
    exp = np.clip(exp, 0, 31)
    scale_bits = (sign << 15) | (exp.astype(np.uint32) << 10) | mant.astype(np.uint32)
    
    # Pack to bytes: each block = 2 bytes scale + 32 bytes quants
    result = bytearray(n_blocks * 34)
    for b in range(n_blocks):
        struct.pack_into('<H', result, b * 34, int(scale_bits[b]))
        result[b*34 + 2:(b+1)*34] = (quants[b].astype(np.uint8) & 0xFF).tobytes()
    
    return bytes(result)

def tensor_name_to_filename(name):
    """Convert safetensors tensor name to .qdat filename.
    Uses FULL path as filename to avoid collisions."""
    # For LM tensors, use the clean naming like blk.X.* 
    # For vision tensors, prefix with vision.
    
    is_vision = 'vision' in name or 'visual' in name or 'vit' in name
    is_lm = 'text_model' in name or 'lm_head' in name or 'embed_tokens' in name
    
    if is_lm and not is_vision:
        parts = name.replace('model.text_model.', '').split('.')
        if 'layers' in parts:
            idx = parts[parts.index('layers') + 1]
            if 'self_attn' in parts:
                sub = parts[parts.index('self_attn') + 1]
                sub = sub.replace('_proj', '')
                if sub == 'q': sub = 'attn_q'
                elif sub == 'k': sub = 'attn_k'
                elif sub == 'v': sub = 'attn_v'
                elif sub == 'o': sub = 'attn_output'
                return f"blk.{idx}.{sub}.weight.qdat"
            if 'mlp' in parts:
                sub = parts[parts.index('mlp') + 1]
                sub = sub.replace('_proj', '')
                if sub == 'gate': sub = 'ffn_gate'
                elif sub == 'up': sub = 'ffn_up'
                elif sub == 'down': sub = 'ffn_down'
                return f"blk.{idx}.{sub}.weight.qdat"
            if 'input_layernorm' in parts:
                return f"blk.{idx}.attn_norm.weight.qdat"
            if 'post_attention_layernorm' in parts:
                return f"blk.{idx}.ffn_norm.weight.qdat"
        if 'embed_tokens' in name:
            return "token_embd.weight.qdat"
        if 'lm_head' in name:
            return "lm_head.weight.qdat"
    
    # Vision or other: use full path as filename
    safe = name.replace('/', '.').replace('model.', '')
    return f"{safe}.qdat"

def main():
    if len(sys.argv) < 3:
        print("Usage: convert_bf16_to_qdat_fast.py <model.safetensors> <output_dir> [--only-lm]")
        sys.exit(1)
    
    safetensors_path = sys.argv[1]
    output_dir = sys.argv[2]
    only_lm = '--only-lm' in sys.argv
    
    os.makedirs(output_dir, exist_ok=True)
    
    with open(safetensors_path, 'rb') as f:
        t0 = time.time()
        header_len = struct.unpack('<Q', f.read(8))[0]
        header = json.loads(f.read(header_len))
        raw_data = f.read()
        t_load = time.time() - t0
        print(f"Loaded safetensors: {len(raw_data)/1e6:.0f} MB in {t_load:.1f}s")
    
    keys = [k for k in header.keys() if k != '__metadata__']
    print(f"Total tensors: {len(keys)}")
    
    converted = 0
    skipped = 0
    t_total = time.time()
    
    for k in keys:
        if only_lm and ('vision' in k or 'visual' in k or 'vit' in k):
            skipped += 1
            continue
        
        meta = header[k]
        off = meta['data_offsets'][0]
        sz = meta['data_offsets'][1] - meta['data_offsets'][0]
        
        # Read BF16 bytes and convert to f32 in batch
        bf16_data = raw_data[off:off+sz]
        t1 = time.time()
        f32_vals = bf16_to_f32_batch(bf16_data)
        t2 = time.time()
        
        # Quantize to Q8_0
        q8_data = quantize_q8_0_np(f32_vals)
        t3 = time.time()
        
        # Determine output filename
        qdat_name = tensor_name_to_filename(k)
        qdat_path = os.path.join(output_dir, qdat_name)
        qtype_path = os.path.join(output_dir, qdat_name.replace('.qdat', '.qtype'))
        
        # Write .qdat
        with open(qdat_path, 'wb') as f:
            f.write(q8_data)
        
        # Write .qtype
        with open(qtype_path, 'wb') as f:
            f.write(bytes([GGML_TYPE_Q8_0]))
        
        converted += 1
        if converted <= 5 or converted % 25 == 0:
            pct = f32_vals.size / 1e6 if f32_vals.size > 1e6 else f32_vals.size / 1e3
            unit = 'M' if f32_vals.size > 1e6 else 'K'
            print(f"  [{converted}/{len(keys)}] {qdat_name} ({f32_vals.size:.0f}{unit} vals, "
                  f"conv:{t2-t1:.2f}s quant:{t3-t2:.2f}s)")
    
    t_elapsed = time.time() - t_total
    print(f"\nDone: {converted} converted, {skipped} skipped in {t_elapsed:.1f}s")
    print(f"Output: {output_dir}/")

if __name__ == '__main__':
    main()
