# Qwen3 TTS Experiment Log

## Goal
Run Qwen3-TTS-12Hz-0.6B-Base on GTX 1050 Ti (4GB VRAM, compute 6.1) with CUDA acceleration.

## Final Result: ❌ FAILED (Silence Output)
Model loads, generates without crash, but output audio is all -1.0 / NaN (silence/DC). Root cause of silence is undiagnosed — likely invalid generated tokens (code_predictor outputs garbage).

**Pipeline tested**: `test_fix_v10.py` — cleanest version.

---

## Timeline

### Attempt 1: Full float32 (`test_fix_v4`)
- Crash: CUDA OOM (3.41 GB spike)
- **Lesson**: float32 is too large for 4GB VRAM → use float16 or bfloat16

### Attempt 2: Direct CUDA loading (`v5-v6`)
- Crash: paging file full — `safe_open` mmap exhausts 8GB RAM
- **Lesson**: Cannot load model via HuggingFace `from_pretrained` after CUDA model is active; must load weights manually

### Attempt 3: Manual CUDA loading (`v7`)
- Approach: `meta → to_empty(device="cuda") → half()` — float32 spike OOM again
- **Lesson**: `.to_empty(device="cuda").half()` allocates float32 first, then halves — too late

### Attempt 4: Per-parameter CUDA half (`v8`)
- Approach: create individual CUDA float16 tensors, copy weights from CPU sd
- Success: model loads (1.71 GB VRAM), speech tokenizer loads (2.02 GB total)
- **Crash**: `TensorCompare.cu:110 Assertion 'input[0] != 0'` — NaN in logits → softmax overflow
- Debugged: 0 NaN/Inf in weights, 0 meta tensors remain
- **Lesson**: float16 computation overflows during attention softmax → NaN

### Attempt 5: bfloat16 (`v9-v10`) ✅ (no crash, wrong output)
- Approach: same per-parameter technique with `torch.bfloat16`
- Manual speech tokenizer loading (CPU creation → float buffers to CUDA, int buffers stay CPU)
- Manual safetensors parse (raw bytes → `torch.frombuffer`) avoids Windows mmap paging error
- Monkey-patch SafeCE to clamp embedding indices OOB
- **Model runs without crash**
- **Output**: audio all -1.0 / NaN — generated codes are invalid

---

## Key Technical Lessons

### bfloat16 vs float16
- Qwen3 TTS weights are natively bfloat16
- float16 exponent range (5 bits) overflows during softmax computation
- bfloat16 = same exponent range as f32 (8 bits) — no overflow
- `torch.bfloat16` works on GTX 1050 Ti (compute 6.1 + CUDA 11+)

### Memory Constraints (4GB VRAM, 8GB RAM)
```
Model weights (bf16):     1.71 GB
Speech tokenizer (bf16):  0.32 GB
Total VRAM:               2.03 GB
Peak CPU RAM:             2.4 GB (sd on CPU simultaneously)
```
- `safe_open` maps file into virtual memory → triggers Windows paging error
- **Fix**: manual tensor-by-tensor loading: read header JSON, seek to offsets, load raw bytes

### Speech Tokenizer (Mimi-based)
- Must be constructed on CPU (not meta device) — preserves scalar `register_buffer` values like `padding_total`, `stride`, `kernel_size`
- `padding_total` is `kernel_size - stride` as int64 buffer — needed as Python int by `nn.functional.pad()`
- Float buffers (codebook `embed`) must be moved to CUDA separately via `named_buffers()`
- Model class: `Qwen3TTSTokenizerV2Model` (12Hz variant)

### Safetensors Manual Parse
```python
st_f = open(path, "rb")
header_len = int.from_bytes(st_f.read(8), "little")
header = json.loads(st_f.read(header_len))
st_f.seek(info["data_offsets"][0], 0)
raw = st_f.read(info["data_offsets"][1] - info["data_offsets"][0])
t = torch.frombuffer(bytearray(raw), dtype=dtype_map[info["dtype"]]).reshape(shape)
```

### SafeCE Monkey-Patch
```python
class SafeCE(nn.Module):
    def __init__(self, e): self.e=e; self.vs=e.num_embeddings
    def forward(self, x):
        if x.numel() and x.max() >= self.vs: x = x.clamp(0, self.vs-1)
        return self.e(x)
```
Prevents OOB embedding lookup crash when generating tokens >= vocab_size (e.g., codec_embedding has 3072 tokens, code_predictor has 2048 per group).

---

## Files
- `test_fix_v10.py`: Clean pipeline (no crash, silence output)
- `test_fix_v9.py`: Working pipeline (same as v10, more verbose)
- `I:/model/qwen3-tts-0.6b/`: Full model directory

## Future Direction Not Recommended
- Root cause of silence is likely in code_predictor or speech tokenizer decode path
- Qwen3 TTS is a custom architecture requiring significant reverse engineering
- Recommend using pykokoro TTS instead (already working: `I:/model/.cache/pykokoro/`)
