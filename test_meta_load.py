"""Load Qwen3 TTS via meta device to avoid OOM."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
import torch.nn as nn
from safetensors import safe_open
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration

t0 = time.time()

# 1. Config
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print(f"[{time.time()-t0:.1f}s] config", flush=True)

# 2. Create model on meta device
with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
print(f"[{time.time()-t0:.1f}s] meta model created", flush=True)

# 3. Load and assign weights one by one
state_dict = {}
with safe_open("I:/model/qwen3-tts-0.6b/model.safetensors", framework="pt", device="cpu") as f:
    keys = f.keys()
    print(f"[{time.time()-t0:.1f}s] {len(keys)} tensors in safetensors", flush=True)
    for k in keys:
        state_dict[k] = f.get_tensor(k)

print(f"[{time.time()-t0:.1f}s] all tensors loaded to CPU ({sum(t.numel()*t.element_size() for t in state_dict.values())/1024**3:.2f} GB)", flush=True)

# 4. Assign to model
model.load_state_dict(state_dict, strict=False, assign=True)
print(f"[{time.time()-t0:.1f}s] state dict assigned", flush=True)

del state_dict; gc.collect()

# 5. Convert to half and move to CUDA
model = model.to(dtype=torch.float16)
print(f"[{time.time()-t0:.1f}s] converted to half", flush=True)

# Move to CUDA
model = model.to("cuda")
print(f"[{time.time()-t0:.1f}s] model on CUDA, VRAM={torch.cuda.memory_allocated()/1024**3:.2f} GB", flush=True)
