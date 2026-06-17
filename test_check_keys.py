"""Check param keys match between model and safetensors."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
from safetensors import safe_open
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration

config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")

# Create model just to inspect keys (not on meta, use tiny)
print("Creating model to inspect keys...", flush=True)
t0 = time.time()
with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
print(f"Meta model created: {time.time()-t0:.1f}s", flush=True)

model_keys = set(n for n, p in model.named_parameters())
model_buffers = set(n for n, b in model.named_buffers())
print(f"Model: {len(model_keys)} params, {len(model_buffers)} buffers", flush=True)

# Check safetensors keys
sf_keys = set()
with safe_open("I:/model/qwen3-tts-0.6b/model.safetensors", framework="pt", device="cpu") as f:
    sf_keys = set(f.keys())
print(f"Safetensors: {len(sf_keys)} tensors", flush=True)

# Find mismatches
in_model_not_sf = model_keys - sf_keys
in_sf_not_model = sf_keys - model_keys

if in_model_not_sf:
    print(f"Model only keys ({len(in_model_not_sf)}):")
    for k in sorted(in_model_not_sf)[:10]:
        print(f"  {k}")
if in_sf_not_model:
    print(f"Safetensors only keys ({len(in_sf_not_model)}):")
    for k in sorted(in_sf_not_model)[:10]:
        print(f"  {k}")
if not in_model_not_sf and not in_sf_not_model:
    print("All keys match!", flush=True)

# Check shapes
print("\nChecking shapes...", flush=True)
with safe_open("I:/model/qwen3-tts-0.6b/model.safetensors", framework="pt", device="cpu") as f:
    for n, p in model.named_parameters():
        if p.shape != f.get_slice(n).get_shape():
            print(f"  SHAPE MISMATCH: {n}: model={p.shape} sf={f.get_slice(n).get_shape()}", flush=True)

print("Done", flush=True)
