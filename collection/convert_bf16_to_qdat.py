#!/usr/bin/env python3
"""convert_bf16_to_qdat.py — Convert BF16 safetensors to Q8_0 .qdat format

Usage:
    python3 convert_bf16_to_qdat.py model.safetensors output_dir/
"""

import struct, sys, os, json, math, array

GGML_TYPE_Q8_0 = 8

def bf16_to_f32(b: int) -> float:
    bits = struct.pack('<I', b << 16)
    return struct.unpack('<f', bits)[0]

def quantize_q8_0(data_f32):
    """Quantize float32 values to Q8_0 blocks (32 values per block)
    
    Returns: bytes of Q8_0 data
    """
    n = len(data_f32)
    n_blocks = (n + 31) // 32
    result = bytearray()
    
    for b in range(n_blocks):
        start = b * 32
        end = min(start + 32, n)
        block_vals = data_f32[start:end]
        
        # Pad last block with zeros if needed
        if len(block_vals) < 32:
            block_vals = block_vals + [0.0] * (32 - len(block_vals))
        
        # Find max absolute value
        amax = max(abs(v) for v in block_vals)
        if amax == 0.0:
            scale = 0.0
            quants = [0] * 32
        else:
            scale = amax / 127.0
            quants = [max(-128, min(127, int(round(v / scale)))) for v in block_vals]
        
        # Write f16 scale
        f32_bytes = struct.pack('<f', scale)
        f32_int = struct.unpack('<I', f32_bytes)[0]
        sign = (f32_int >> 31) & 1
        exp = ((f32_int >> 23) & 0xFF) - 112
        mant = (f32_int >> 13) & 0x3FF  # top 10 bits of mantissa
        if exp <= 0:
            # Subnormal / zero
            scale_bits = 0
            if scale != 0.0:
                # Minimal subnormal
                scale_bits = 1 if sign else 0
        elif exp >= 31:
            # Inf/NaN
            scale_bits = 0x7C00 if sign == 0 else 0xFC00
        else:
            scale_bits = (sign << 15) | (exp << 10) | mant
        
        result += struct.pack('<H', scale_bits)
        result += bytes(q & 0xFF for q in quants)
    
    return bytes(result)

def tensor_name_to_filename(name: str) -> str:
    """Convert safetensors tensor name to .qdat filename"""
    # model.text_model.layers.0.self_attn.q_proj.weight -> blk.0.attn_q.weight
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
    if 'lm_head' in name or 'head' in name:
        return "output_norm.weight.qdat"  # SmolLM2 naming uses output_norm for head weight
    if 'connector' in name:
        return "connector.weight.qdat"
    if 'vision' in name:
        # Flatten vision name to safe filename
        safe = name.replace('/', '.').replace('model.', '')
        return f"vision.{safe}.qdat"
    
    # Fallback: flatten
    safe = name.replace('/', '.')
    return f"{safe}.qdat"

def qtype_filename_from_qdat(qdat_name: str) -> str:
    return qdat_name.replace('.qdat', '.qtype')

def main():
    if len(sys.argv) < 3:
        print("Usage: convert_bf16_to_qdat.py <model.safetensors> <output_dir>")
        sys.exit(1)
    
    safetensors_path = sys.argv[1]
    output_dir = sys.argv[2]
    os.makedirs(output_dir, exist_ok=True)
    
    with open(safetensors_path, 'rb') as f:
        header_len = struct.unpack('<Q', f.read(8))[0]
        header = json.loads(f.read(header_len))
        raw_data = f.read()
    
    keys = [k for k in header.keys() if k != '__metadata__']
    print(f"Converting {len(keys)} tensors from {safetensors_path}")
    print(f"Output: {output_dir}/")
    
    converted = 0
    skipped = 0
    for k in keys:
        meta = header[k]
        off = meta['data_offsets'][0]
        sz = meta['data_offsets'][1] - meta['data_offsets'][0]
        shape = meta['shape']
        
        # Read BF16 values
        n_bf16 = sz // 2
        f32_vals = []
        for i in range(n_bf16):
            b = struct.unpack('<H', raw_data[off + i*2: off + i*2 + 2])[0]
            f32_vals.append(bf16_to_f32(b))
        
        # Quantize to Q8_0
        q8_data = quantize_q8_0(f32_vals)
        
        # Determine output filename
        qdat_name = tensor_name_to_filename(k)
        qdat_path = os.path.join(output_dir, qdat_name)
        qtype_path = os.path.join(output_dir, qtype_filename_from_qdat(qdat_name))
        
        # Write .qdat
        with open(qdat_path, 'wb') as f:
            f.write(q8_data)
        
        # Write .qtype
        with open(qtype_path, 'wb') as f:
            f.write(bytes([GGML_TYPE_Q8_0]))
        
        converted += 1
        if converted <= 5 or converted % 50 == 0:
            print(f"  [{converted}/{len(keys)}] {qdat_name} ({'x'.join(str(d) for d in shape)}) -> {len(q8_data)} bytes")
    
    print(f"\nDone: {converted} tensors converted to {output_dir}/")
    print(f"Skipped: {skipped}")

if __name__ == '__main__':
    main()
