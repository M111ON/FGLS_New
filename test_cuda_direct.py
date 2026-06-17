"""Load safetensors directly to CUDA to avoid CPU memory spike."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration
from safetensors.torch import load_file

t0 = time.time()
print("creating config...", flush=True)
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print(f"config ok: {time.time()-t0:.1f}s", flush=True)

# Create model in float16 on CPU
print("creating model (half)...", flush=True)
model = Qwen3TTSForConditionalGeneration(config).half()
print(f"model created: {time.time()-t0:.1f}s", flush=True)

# Check memory
import psutil
mem = psutil.Process().memory_info()
print(f"CPU RSS: {mem.rss/1024**3:.2f} GB", flush=True)
print(f"CPU VMS: {mem.vms/1024**3:.2f} GB", flush=True)

# Load safetensors directly to CUDA
print("loading sd to cuda...", flush=True)
sd = load_file("I:/model/qwen3-tts-0.6b/model.safetensors", device="cuda")
print(f"sd loaded to cuda: {len(sd)} keys, {time.time()-t0:.1f}s", flush=True)

mem2 = psutil.Process().memory_info()
print(f"CPU RSS after sd: {mem2.rss/1024**3:.2f} GB", flush=True)
cuda_used = torch.cuda.memory_allocated() / 1024**3
print(f"CUDA used: {cuda_used:.2f} GB", flush=True)

# Move model to CUDA first, then load state dict
print("moving model to cuda...", flush=True)
model = model.to("cuda", dtype=torch.float16)
print(f"model on cuda: {time.time()-t0:.1f}s", flush=True)

print("loading state_dict from cuda sd...", flush=True)
result = model.load_state_dict(sd, strict=False, assign=True)
print(f"state_dict loaded: missing={len(result.missing_keys)} unexpected={len(result.unexpected_keys)}", flush=True)
if result.missing_keys:
    print(f"  missing: {result.missing_keys[:10]}", flush=True)

del sd; gc.collect()
torch.cuda.empty_cache()
print(f"CUDA used after cleanup: {torch.cuda.memory_allocated()/1024**3:.2f} GB", flush=True)
print(f"DONE: {time.time()-t0:.1f}s", flush=True)
