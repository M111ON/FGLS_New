"""Test model creation + state dict loading."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration
from safetensors.torch import load_file

t0 = time.time()
print("creating config...")
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print(f"config ok: {time.time()-t0:.1f}s")

print("creating model (this takes ~50s)...", flush=True)
model = Qwen3TTSForConditionalGeneration(config).half()
print(f"model created: {time.time()-t0:.1f}s", flush=True)

print("loading state dict...", flush=True)
sd = load_file("I:/model/qwen3-tts-0.6b/model.safetensors")
print(f"sd loaded: {len(sd)} keys, {time.time()-t0:.1f}s", flush=True)

result = model.load_state_dict(sd, strict=False)
print(f"state_dict loaded: missing={len(result.missing_keys)} unexpected={len(result.unexpected_keys)}", flush=True)
if result.missing_keys:
    print(f"  missing keys: {result.missing_keys[:3]}...")
if result.unexpected_keys:
    print(f"  unexpected keys: {result.unexpected_keys[:3]}...")

del sd, result; gc.collect()
print(f"DONE: {time.time()-t0:.1f}s", flush=True)
