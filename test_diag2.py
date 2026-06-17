"""Diagnose exact crash point — step by step."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch, torch.nn as nn
from safetensors import safe_open
from transformers.modeling_rope_utils import ROPE_INIT_FUNCTIONS
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration

MODEL_DIR = "I:/model/qwen3-tts-0.6b"
t0 = time.time()

print(f"[{time.time()-t0:.1f}s] START", flush=True)

config = Qwen3TTSConfig.from_pretrained(MODEL_DIR)
print(f"[{time.time()-t0:.1f}s] config", flush=True)

with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
model = model.to_empty(device="cuda")
model = model.half()
print(f"[{time.time()-t0:.1f}s] CUDA half VRAM={torch.cuda.memory_allocated()/1024**3:.2f}GB", flush=True)

rope_fn = ROPE_INIT_FUNCTIONS['default']
for sub in [model.talker.model, model.talker.code_predictor.model]:
    if hasattr(sub, 'rotary_emb') and hasattr(sub.rotary_emb, 'inv_freq') and sub.rotary_emb.inv_freq.is_meta:
        inv_freq, attn_scaling = rope_fn(sub.rotary_emb.config, device='cuda')
        sub.rotary_emb.inv_freq = nn.Parameter(inv_freq.to(dtype=torch.float16), requires_grad=False)
print(f"[{time.time()-t0:.1f}s] rope fixed", flush=True)

print(f"[{time.time()-t0:.1f}s] opening file...", flush=True)
sf = safe_open(f"{MODEL_DIR}/model.safetensors", framework="pt", device="cpu")
keys = sf.keys()
print(f"[{time.time()-t0:.1f}s] file opened, {len(keys)} tensors", flush=True)

print(f"[{time.time()-t0:.1f}s] getting tensor[0]...", flush=True)
t0_t = sf.get_tensor(keys[0])
print(f"[{time.time()-t0:.1f}s] tensor[0] shape={t0_t.shape} dtype={t0_t.dtype} device={t0_t.device}", flush=True)
del t0_t; gc.collect()
print(f"[{time.time()-t0:.1f}s] first tensor OK", flush=True)
