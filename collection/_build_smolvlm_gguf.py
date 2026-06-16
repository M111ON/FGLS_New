#!/usr/bin/env python3
"""
Build smolVLM GGUF from BF16 safetensors data, transposing and quantizing to Q8_0.

Reads:
  - I:/model/smolVLM-256M-Instruct/model.safetensors (header + BF16 tensor data)

Writes:
  - I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf

Only text backbone tensors (0..29 LM layers) are included,
mapped to 'smollm2' architecture so llama.cpp can load the model.
Vision encoder tensors are EXCLUDED from the GGUF (llama.cpp b9528
does not support multi-modal). They can be analyzed separately.

Usage:
  python _build_smolvlm_gguf.py
"""

import json, os, struct, sys
import numpy as np

SAFETENSORS = "I:/model/smolVLM-256M-Instruct/model.safetensors"
OUTPUT = "I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf"
ARCH = "llama"
ALIGN = 32

# ── HuggingFace → GGUF tensor name mapping ──
HF2GGUF = {
    "model.text_model.layers.{}.self_attn.q_proj.weight":      "blk.{}.attn_q.weight",
    "model.text_model.layers.{}.self_attn.k_proj.weight":      "blk.{}.attn_k.weight",
    "model.text_model.layers.{}.self_attn.v_proj.weight":      "blk.{}.attn_v.weight",
    "model.text_model.layers.{}.self_attn.o_proj.weight":      "blk.{}.attn_output.weight",
    "model.text_model.layers.{}.mlp.gate_proj.weight":         "blk.{}.ffn_gate.weight",
    "model.text_model.layers.{}.mlp.up_proj.weight":           "blk.{}.ffn_up.weight",
    "model.text_model.layers.{}.mlp.down_proj.weight":         "blk.{}.ffn_down.weight",
    "model.text_model.layers.{}.input_layernorm.weight":       "blk.{}.attn_norm.weight",
    "model.text_model.layers.{}.post_attention_layernorm.weight": "blk.{}.ffn_norm.weight",
}

DIRECT_MAP = {
    "model.text_model.embed_tokens.weight":  "token_embd.weight",
    "lm_head.weight":                        "output.weight",
    "model.text_model.norm.weight": "output_norm.weight",
}

GGML_TYPE_F32 = 0
GGML_TYPE_Q8_0 = 8

SF_DATA_START = None  # set in main()

def read_bf16_tensor(sf, info):
    """Read BF16 tensor from safetensors → float32 numpy array."""
    shape = tuple(info["shape"])
    offsets = info["data_offsets"]
    sf.seek(SF_DATA_START + offsets[0])
    n_bf16 = int(np.prod(shape))
    raw = sf.read(n_bf16 * 2)
    u16 = np.frombuffer(raw, dtype=np.uint16).reshape(shape)
    # BF16 → F32: shift left 16 bits, reinterpret as float32
    f32 = (u16.astype(np.uint32) << 16).view(np.float32)
    return f32, shape

def quantize_q8_0(arr):
    """Quantize float32 numpy array to Q8_0 format. Returns bytes.
    arr is 1D; blocks of 32 along last dimension.
    """
    n = arr.size
    n_blocks = (n + 31) // 32
    padded = np.zeros(n_blocks * 32, dtype=np.float32)
    padded[:n] = arr.ravel()
    blocks = padded.reshape(-1, 32)
    amax = np.max(np.abs(blocks), axis=1)
    d = np.where(amax > 0, amax / 127.0, 1e-10)
    # Round-trip through float16 for GGML compatibility
    d_f16 = d.astype(np.float16)
    d_val = d_f16.astype(np.float32)
    qi = np.round(blocks / d_val[:, None]).astype(np.int8)
    # Interleave: [d_f16(2B) + qi(32B)] per block
    d_bytes = d_f16.tobytes()
    qi_bytes = qi.tobytes()
    out = bytearray()
    for bi in range(n_blocks):
        out.extend(d_bytes[bi*2:(bi+1)*2])
        out.extend(qi_bytes[bi*32:(bi+1)*32])
    return bytes(out)

def gguf_str(s):
    """Encode string for GGUF: [len: uint64][data: utf8]"""
    data = s.encode("utf-8")
    return struct.pack("<Q", len(data)) + data

def write_gguf(tensors, output_path, vocab_size):
    """Write GGUF v3 file.
    tensors = [(name, shape, type_code, data_bytes), ...]
    """
    n_tensors = len(tensors)

    # ── Calculate data offsets ──
    # Header region
    header_size = 4 + 4 + 8 + 8  # magic + version + n_tensors + n_metadata

    # Metadata (minimal)
    meta_entries = []  # [(key, value_type, encoded_value)]
    # architecture
    arch_bytes = gguf_str("general.architecture")
    arch_val = gguf_str(ARCH)
    meta_entries.append((arch_bytes, 8, arch_val))  # 8 = string type

    # hidden_size, n_layers, ffn_size, n_heads, n_kv_heads, rope
    hparams = [
        ("llama.context_length", 4, struct.pack("<I", 2048)),
        ("llama.embedding_length", 4, struct.pack("<I", 576)),
        ("llama.block_count", 4, struct.pack("<I", 30)),
        ("llama.feed_forward_length", 4, struct.pack("<I", 1536)),
        ("llama.attention.head_count", 4, struct.pack("<I", 9)),
        ("llama.attention.head_count_kv", 4, struct.pack("<I", 3)),
        ("llama.rope.dimension_count", 4, struct.pack("<I", 64)),
        ("llama.attention.layer_norm_rms_epsilon", 6, struct.pack("<f", 1e-5)),
        ("llama.rope.freq_base", 6, struct.pack("<f", 100000.0)),
    ]
    for key, vtype, vdata in hparams:
        k_bytes = gguf_str(key)
        meta_entries.append((k_bytes, vtype, vdata))

    # general.file_type (u32, 7 = Q8_0)
    meta_entries.append((gguf_str("general.file_type"), 4, struct.pack("<I", 7)))

    meta_entries.append((gguf_str("llama.vocab_size"), 4, struct.pack("<I", vocab_size)))

    # ── Tokenizer (copy from SmolLM2 GGUF) ──
    tokenizer_keys = {
        "tokenizer.ggml.model", "tokenizer.ggml.pre",
        "tokenizer.ggml.bos_token_id", "tokenizer.ggml.eos_token_id",
        "tokenizer.ggml.unknown_token_id", "tokenizer.ggml.padding_token_id",
        "tokenizer.ggml.add_space_prefix", "tokenizer.ggml.add_bos_token",
        "tokenizer.chat_template",
        "tokenizer.ggml.tokens",
        "tokenizer.ggml.token_type",
        "tokenizer.ggml.merges",
    }
    SMOLLM2_GGUF = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
    with open(SMOLLM2_GGUF, "rb") as f:
        f.read(4)  # magic
        ver = struct.unpack("<I", f.read(4))[0]
        nt = struct.unpack("<Q", f.read(8))[0]  # n_tensors
        nm = struct.unpack("<Q", f.read(8))[0]  # n_metadata
        for _ in range(nm):
            kl = struct.unpack("<Q", f.read(8))[0]
            key = f.read(kl).decode()
            vt = struct.unpack("<I", f.read(4))[0]
            if key in tokenizer_keys:
                if vt == 8:  # string
                    vl = struct.unpack("<Q", f.read(8))[0]
                    val = struct.pack("<Q", vl) + f.read(vl)  # GGUF: [uint64 len][data]
                    meta_entries.append((gguf_str(key), vt, val))
                elif vt in (4, 5):  # uint32/int32
                    val = f.read(4)
                    meta_entries.append((gguf_str(key), vt, val))
                elif vt == 6:  # float32
                    val = f.read(4)
                    meta_entries.append((gguf_str(key), vt, val))
                elif vt == 7:  # bool
                    val = f.read(1)
                    meta_entries.append((gguf_str(key), vt, val))
                elif vt == 9:  # array
                    at = struct.unpack("<I", f.read(4))[0]
                    al = struct.unpack("<Q", f.read(8))[0]
                    target_al = vocab_size if key in ("tokenizer.ggml.tokens", "tokenizer.ggml.token_type") else al
                    # read all elements into a list, then build array data
                    arr_data = struct.pack("<I", at) + struct.pack("<Q", target_al)
                    if at == 8:  # array of strings
                        elems = []
                        for _ in range(al):
                            sl = struct.unpack("<Q", f.read(8))[0]
                            elems.append(f.read(sl))
                        for e in elems:
                            arr_data += struct.pack("<Q", len(e)) + e
                        for j in range(target_al - al):
                            pad = f"<|pad_{j}|>".encode("utf-8")
                            arr_data += struct.pack("<Q", len(pad)) + pad
                    elif at in (4, 5):
                        raw = f.read(4 * al)
                        arr_data += raw
                        arr_data += b"\x00\x00\x00\x00" * (target_al - al)
                    else:
                        raise ValueError(f"unhandled array type {at}")
                    meta_entries.append((gguf_str(key), vt, arr_data))
                else:
                    raise ValueError(f"unhandled tokenizer type {vt}")
            else:
                # skip this metadata entry
                if vt == 8:
                    vl = struct.unpack("<Q", f.read(8))[0]
                    f.read(vl)
                elif vt in (4, 5, 6):
                    f.read(4)
                elif vt == 7:
                    f.read(1)
                elif vt == 9:
                    at = struct.unpack("<I", f.read(4))[0]
                    al = struct.unpack("<Q", f.read(8))[0]
                    if at == 8:
                        for _ in range(al):
                            sl = struct.unpack("<Q", f.read(8))[0]
                            f.read(sl)
                    elif at in (4, 5):
                        f.read(4 * al)
                    else:
                        raise ValueError(f"unhandled array type {at}")
                else:
                    raise ValueError(f"unhandled type {vt}")

    # metadata KV count
    n_metadata = len(meta_entries)

    # Tensor info region
    tensor_info_size = 0
    tensor_info_entries = []
    for name, shape, ttype, data in tensors:
        n_dims = len(shape)
        # llama.cpp b9528 reads dims as int64_t (8 bytes each), not uint32
        dims_bytes = b"".join(struct.pack("<q", d) for d in shape)
        ti = (gguf_str(name), n_dims, dims_bytes, ttype)
        tensor_info_size += len(ti[0]) + 4 + 8 * n_dims + 4 + 8
        tensor_info_entries.append(ti)

    # Data region starts after all headers
    header_end = (header_size +
                    sum(len(k) + 4 + len(v) for k, _, v in meta_entries) +
                    tensor_info_size)
    # Align to ALIGN boundary
    data_section_start = header_end
    if data_section_start % ALIGN:
        data_section_start += ALIGN - (data_section_start % ALIGN)

    # Assign RELATIVE offsets (llama.cpp b9528 uses 0-based from data section)
    rel_offsets = []
    cur = 0
    for name, shape, ttype, data in tensors:
        if cur % ALIGN:
            cur += ALIGN - (cur % ALIGN)
        rel_offsets.append(cur)
        cur += len(data)
    total_data_size = cur
    total_file_size = data_section_start + total_data_size

    # ── Write ──
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as f:
        # Magic + version
        f.write(b"GGUF")
        f.write(struct.pack("<I", 3))  # version 3
        f.write(struct.pack("<Q", n_tensors))
        f.write(struct.pack("<Q", n_metadata))

        # Metadata
        for k_bytes, vtype, vdata in meta_entries:
            f.write(k_bytes)
            f.write(struct.pack("<I", vtype))
            f.write(vdata)

        # Tensor info (with RELATIVE offsets)
        for i, (name, shape, ttype, data) in enumerate(tensors):
            f.write(gguf_str(name))
            f.write(struct.pack("<I", len(shape)))
            for d in shape:
                f.write(struct.pack("<q", d))  # int64_t per b9528
            f.write(struct.pack("<I", ttype))
            f.write(struct.pack("<Q", rel_offsets[i]))

        # Padding to data section start
        if data_section_start > f.tell():
            f.write(b"\x00" * (data_section_start - f.tell()))

        # Tensor data
        for i, (name, shape, ttype, data) in enumerate(tensors):
            expected = data_section_start + rel_offsets[i]
            if f.tell() != expected:
                f.write(b"\x00" * (expected - f.tell()))
            f.write(data)

    return total_file_size

def main():
    # Step 1: Read safetensors header
    with open(SAFETENSORS, "rb") as f:
        hlen = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(hlen))

    hf_names = sorted(k for k in header if k != "__metadata__")
    hf_shapes = {k: tuple(header[k]["shape"]) for k in hf_names}

    print(f"[gguf] safetensors: {len(hf_names)} tensors loaded", flush=True)

    # Step 2: Filter for text backbone tensors only
    text_tensors = []
    vision_tensors = []
    for name in hf_names:
        if "vision" in name:
            vision_tensors.append(name)
        else:
            text_tensors.append(name)

    print(f"[gguf] text tensors: {len(text_tensors)}, vision tensors: {len(vision_tensors)}", flush=True)

    # Step 3: Map text tensors and convert BF16 → Q8_0 with correct transposition
    mapped = []

    def ggml_shape(hf_shape):
        """Transpose 2D: HF (out_dim, in_dim) -> GGML (in_dim, out_dim)."""
        if len(hf_shape) == 2:
            return (hf_shape[1], hf_shape[0])
        return hf_shape

    global SF_DATA_START
    SF_DATA_START = 8 + hlen
    sf_file = open(SAFETENSORS, "rb")

    def load_and_quantize(hf_name, gguf_name):
        """Load BF16 from safetensors, transpose if 2D, quantize to Q8_0.
        1D tensors (norms) stored as F32."""
        hf_info = header[hf_name]
        f32, hf_shape = read_bf16_tensor(sf_file, hf_info)
        ggml_sh = ggml_shape(hf_shape)
        if len(hf_shape) == 2:
            f32 = f32.T  # transpose via numpy
            data = quantize_q8_0(f32)
            ttype = GGML_TYPE_Q8_0
        else:
            data = f32.tobytes()
            ttype = GGML_TYPE_F32
        mapped.append((gguf_name, ggml_sh, ttype, data))
        print(f"  {gguf_name} shape={ggml_sh} size={len(data)} type={'q8_0' if ttype == GGML_TYPE_Q8_0 else 'f32'}", flush=True)

    # Direct mappings (non-layer)
    for hf_name, gguf_name in DIRECT_MAP.items():
        if hf_name in hf_shapes:
            load_and_quantize(hf_name, gguf_name)

    # Per-layer mappings
    for layer_idx in range(30):
        for hf_tmpl, gguf_tmpl in HF2GGUF.items():
            hf_name = hf_tmpl.format(layer_idx)
            gguf_name = gguf_tmpl.format(layer_idx)
            if hf_name in hf_shapes:
                load_and_quantize(hf_name, gguf_name)
            else:
                print(f"  WARN: {hf_name} not in safetensors header", flush=True)

    sf_file.close()

    print(f"[gguf] mapped {len(mapped)} tensors to GGUF format", flush=True)

    # Step 4: Write GGUF
    vocab_size = hf_shapes["model.text_model.embed_tokens.weight"][0]
    total = write_gguf(mapped, OUTPUT, vocab_size)
    print(f"[gguf] written: {OUTPUT} ({total} bytes, {len(mapped)} tensors)", flush=True)

    # Step 5: Verify with gguf_idx_open (via C)
    print(f"[gguf] done. Use: llama_pogls_runner_sid_v2.exe {OUTPUT} --count-only --bond", flush=True)

if __name__ == "__main__":
    main()
