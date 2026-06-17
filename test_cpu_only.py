"""Test model creation + state dict loading - CPU only, no CUDA ops."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
os.environ["CUDA_VISIBLE_DEVICES"] = ""
import torch
torch.cuda.is_available = lambda: False
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration
from safetensors.torch import load_file

t0 = time.time()
print("creating config...", flush=True)
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print(f"config ok: {time.time()-t0:.1f}s", flush=True)

# Try without .half() first — float32
print("creating model (float32)...", flush=True)
model = Qwen3TTSForConditionalGeneration(config)
print(f"model created: {time.time()-t0:.1f}s", flush=True)

print("loading state dict...", flush=True)
try:
    sd = load_file("I:/model/qwen3-tts-0.6b/model.safetensors")
    print(f"sd loaded: {len(sd)} keys, {time.time()-t0:.1f}s", flush=True)
    result = model.load_state_dict(sd, strict=False)
    print(f"state_dict loaded: missing={len(result.missing_keys)} unexpected={len(result.unexpected_keys)}", flush=True)
    if result.missing_keys:
        print(f"  missing keys: {result.missing_keys[:5]}", flush=True)
    if result.unexpected_keys:
        print(f"  unexpected keys: {result.unexpected_keys[:5]}", flush=True)
except Exception as e:
    print(f"ERROR: {e}", flush=True)
    import traceback; traceback.print_exc()

del sd; gc.collect()
print(f"DONE: {time.time()-t0:.1f}s", flush=True)
