"""Stream-load Qwen3 TTS weights one tensor at a time."""
import os, sys, time, gc
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
from safetensors import safe_open
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration

t0 = time.time()
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print(f"[{time.time()-t0:.1f}s] config", flush=True)

# Create model on meta device
with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
print(f"[{time.time()-t0:.1f}s] meta model", flush=True)

# Get param name -> param mapping
param_map = {name: p for name, p in model.named_parameters()}
buffer_map = {name: b for name, b in model.named_buffers()}
print(f"[{time.time()-t0:.1f}s] {len(param_map)} params, {len(buffer_map)} buffers", flush=True)

# Stream-load and assign one by one
loaded = 0
with safe_open("I:/model/qwen3-tts-0.6b/model.safetensors", framework="pt", device="cpu") as f:
    keys = f.keys()
    for k in keys:
        tensor = f.get_tensor(k)
        if k in param_map:
            param_map[k].data.copy_(tensor)
        elif k in buffer_map:
            buffer_map[k].data.copy_(tensor)
        else:
            print(f"  WARNING: {k} not found in model", flush=True)
        del tensor
        loaded += 1
        if loaded % 50 == 0:
            gc.collect()
            print(f"[{time.time()-t0:.1f}s] loaded {loaded}/{len(keys)}", flush=True)

print(f"[{time.time()-t0:.1f}s] all {loaded} tensors assigned", flush=True)

# Convert to half
model = model.to(dtype=torch.float16)
print(f"[{time.time()-t0:.1f}s] half", flush=True)

# Fix rope buffers
for name, buf in model.named_buffers():
    if buf.is_meta:
        print(f"  meta buffer: {name}", flush=True)
from transformers.modeling_rope_utils import ROPE_INIT_FUNCTIONS
rope_fn = ROPE_INIT_FUNCTIONS['default']
for prefix, submodel in [('talker.model', model.talker.model), 
                          ('talker.code_predictor.model', model.talker.code_predictor.model)]:
    if hasattr(submodel, 'rotary_emb'):
        inv_freq, attn_scaling = rope_fn(submodel.rotary_emb.config, device='cpu')
        submodel.rotary_emb.inv_freq = nn.Parameter(inv_freq, requires_grad=False)
print(f"[{time.time()-t0:.1f}s] rope fixed", flush=True)

# Move to CUDA
model = model.to("cuda")
print(f"[{time.time()-t0:.1f}s] CUDA, VRAM={torch.cuda.memory_allocated()/1024**3:.2f} GB", flush=True)
