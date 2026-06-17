"""Diagnose exact crash point during weight load."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch, torch.nn as nn
from safetensors import safe_open
from transformers.modeling_rope_utils import ROPE_INIT_FUNCTIONS
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration

MODEL_DIR = "I:/model/qwen3-tts-0.6b"
t0 = time.time()

config = Qwen3TTSConfig.from_pretrained(MODEL_DIR)
print(f"[{time.time()-t0:.1f}s] config", flush=True)

with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
model = model.to_empty(device="cuda")
model = model.half()
print(f"[{time.time()-t0:.1f}s] CUDA half done", flush=True)

rope_fn = ROPE_INIT_FUNCTIONS['default']
for sub in [model.talker.model, model.talker.code_predictor.model]:
    if hasattr(sub, 'rotary_emb') and hasattr(sub.rotary_emb, 'inv_freq') and sub.rotary_emb.inv_freq.is_meta:
        inv_freq, attn_scaling = rope_fn(sub.rotary_emb.config, device='cuda')
        sub.rotary_emb.inv_freq = nn.Parameter(inv_freq.to(dtype=torch.float16), requires_grad=False)
print(f"[{time.time()-t0:.1f}s] rope fixed", flush=True)

# VRAM check
print(f"VRAM before load: {torch.cuda.memory_allocated()/1024**3:.2f} GB", flush=True)

# Stream load with timing
param_map = {n: p for n, p in model.named_parameters()}
buffer_map = {n: b for n, b in model.named_buffers()}

with safe_open(f"{MODEL_DIR}/model.safetensors", framework="pt", device="cpu") as f:
    keys = f.keys()
    print(f"[{time.time()-t0:.1f}s] file opened, {len(keys)} tensors", flush=True)
    for i, k in enumerate(keys):
        if i > 0 and i % 10 == 0:
            print(f"  [{i}] {k}", flush=True)
        try:
            t = f.get_tensor(k)
            target = param_map.get(k, buffer_map.get(k))
            if target is not None:
                tt = t.to(dtype=torch.float16, device="cuda")
                target.data.copy_(tt)
                del tt
            del t
        except Exception as e:
            print(f"  CRASH at [{i}] {k}: {e}", flush=True)
            raise
        
        if (i+1) % 50 == 0:
            gc.collect()
            vr = torch.cuda.memory_allocated()/1024**3
            print(f"[{time.time()-t0:.1f}s] {i+1}/{len(keys)} VRAM={vr:.2f}GB", flush=True)

print(f"[{time.time()-t0:.1f}s] ALL WEIGHTS OK", flush=True)
