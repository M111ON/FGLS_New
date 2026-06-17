"""Debug CUDA memory usage step by step."""
import os, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration

def vram():
    return torch.cuda.memory_allocated() / 1024**3

t0 = time.time()
print(f"[{time.time()-t0:.1f}] VRAM={vram():.2f}GB torch.cuda.is_available={torch.cuda.is_available()}", flush=True)

# Trigger CUDA init
_ = torch.zeros(1, device='cuda')
print(f"[{time.time()-t0:.1f}] after CUDA init VRAM={vram():.2f}GB", flush=True)
del _; gc.collect()
torch.cuda.empty_cache()
print(f"[{time.time()-t0:.1f}] after cleanup VRAM={vram():.2f}GB", flush=True)

print("loading config...", flush=True)
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print(f"[{time.time()-t0:.1f}] config VRAM={vram():.2f}GB", flush=True)

print("creating model on meta...", flush=True)
with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
print(f"[{time.time()-t0:.1f}] meta model VRAM={vram():.2f}GB", flush=True)

print("to_empty(cuda)...", flush=True)
try:
    model = model.to_empty(device="cuda")
    print(f"[{time.time()-t0:.1f}] to_empty VRAM={vram():.2f}GB", flush=True)
except Exception as e:
    print(f"to_empty failed: {e}", flush=True)
