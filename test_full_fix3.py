"""Fix v3: load safetensors to CPU, then stream-copy to CUDA model."""
import os, sys, time, gc, numpy as np
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch, torch.nn as nn
from safetensors.torch import load_file
from transformers import AutoProcessor
from transformers.modeling_rope_utils import ROPE_INIT_FUNCTIONS
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration, Qwen3TTSProcessor
from qwen_tts.inference.qwen3_tts_model import Qwen3TTSModel as Qwen3TTSWrapper
from qwen_tts.inference.qwen3_tts_tokenizer import Qwen3TTSTokenizer

MODEL_DIR = "I:/model/qwen3-tts-0.6b"
t0 = time.time()

# 1. Config
config = Qwen3TTSConfig.from_pretrained(MODEL_DIR)
print(f"[{time.time()-t0:.1f}s] config", flush=True)

# 2. Meta model → CUDA empty → half
with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
model = model.to_empty(device="cuda")
model = model.half()
print(f"[{time.time()-t0:.1f}s] CUDA empty half", flush=True)

# 3. Fix rope
rope_fn = ROPE_INIT_FUNCTIONS['default']
for sub in [model.talker.model, model.talker.code_predictor.model]:
    if hasattr(sub, 'rotary_emb') and hasattr(sub.rotary_emb, 'inv_freq') and sub.rotary_emb.inv_freq.is_meta:
        inv_freq, attn_scaling = rope_fn(sub.rotary_emb.config, device='cuda')
        sub.rotary_emb.inv_freq = nn.Parameter(inv_freq.to(dtype=torch.float16), requires_grad=False)
print(f"[{time.time()-t0:.1f}s] rope fixed", flush=True)

# 4. Load safetensors to CPU
sd = load_file(f"{MODEL_DIR}/model.safetensors", device="cpu")
print(f"[{time.time()-t0:.1f}s] sd loaded CPU: {len(sd)} keys", flush=True)
print(f"  CPU mem: {sum(t.numel()*t.element_size() for t in sd.values())/1024**3:.2f} GB", flush=True)

# 5. Copy to CUDA model one-by-one
param_map = {n: p for n, p in model.named_parameters()}
buffer_map = {n: b for n, b in model.named_buffers()}
for i, (k, t) in enumerate(sd.items()):
    target = param_map.get(k, buffer_map.get(k))
    if target is not None:
        target.data.copy_(t.to(dtype=torch.float16, device="cuda"))
    if (i+1) % 100 == 0:
        gc.collect()
        print(f"[{time.time()-t0:.1f}s] copied {i+1}/{len(sd)}", flush=True)
del sd; gc.collect()
print(f"[{time.time()-t0:.1f}s] all weights copied", flush=True)

# 6. Speech tokenizer
st = Qwen3TTSTokenizer.from_pretrained(f"{MODEL_DIR}/speech_tokenizer", device_map="cuda", dtype="float16")
model.load_speech_tokenizer(st)
print(f"[{time.time()-t0:.1f}s] speech tokenizer", flush=True)

# 7. Processor + Wrapper
AutoProcessor.register(Qwen3TTSConfig, Qwen3TTSProcessor)
processor = AutoProcessor.from_pretrained(MODEL_DIR, fix_mistral_regex=True)
wrapper = Qwen3TTSWrapper(model=model, processor=processor)
print(f"[{time.time()-t0:.1f}s] wrapper ready — VRAM {torch.cuda.memory_allocated()/1024**3:.2f}GB", flush=True)

# 8. Monkey-patch: clamp input_ids
talker = model.talker; cp = talker.code_predictor

orig_ce = talker.model.codec_embedding
class SafeCE(nn.Module):
    def __init__(self, e): super().__init__(); self.e=e; self.vs=e.num_embeddings
    @property
    def weight(self): return self.e.weight
    def forward(self, x):
        if x.numel() and x.max() >= self.vs:
            print(f"[FIX] ce clamp max={x.max().item()} vs={self.vs}", flush=True); x=x.clamp(0,self.vs-1)
        return self.e(x)
talker.model.codec_embedding = SafeCE(orig_ce)
talker.model.get_input_embeddings = lambda: talker.model.codec_embedding
talker.get_input_embeddings = lambda: talker.model.codec_embedding

orig_ce2 = cp.model.codec_embedding
vs2 = [e.num_embeddings for e in orig_ce2]
class SafeCEL(nn.Module):
    def __init__(self): super().__init__(); self._e=orig_ce2; self._vs=vs2
    def __getitem__(self, i):
        e=self._e[i]; vs=self._vs[i]
        class I(nn.Module):
            def __init__(self): super().__init__()
            def forward(self, x):
                if x.numel() and x.max()>=vs:
                    print(f"[FIX] cp_emb[{i}] clamp max={x.max().item()} vs={vs}",flush=True); return e(x.clamp(0,vs-1))
                return e(x)
        return I()
    def __len__(self): return len(self._e)
    def __iter__(self):
        for i in range(len(self._e)): yield self[i]
cp.model.codec_embedding = SafeCEL()
cp.model.get_input_embeddings = lambda: cp.model.codec_embedding
cp.get_input_embeddings = lambda: cp.model.codec_embedding

# 9. Test
sr=24000; dummy_audio=np.sin(2*np.pi*440*np.arange(sr)/sr).astype(np.float32)
print("\n=== Testing TTS ===", flush=True)
try:
    wavs,fs = wrapper.generate_voice_clone(text="Hello, this is a test.", language="english",
        ref_audio=(dummy_audio,sr), x_vector_only_mode=True, max_audio_tokens=256)
    import soundfile
    os.makedirs("build", exist_ok=True)
    soundfile.write("build/test_qwen3_fixed.wav", wavs[0], fs)
    print(f"SUCCESS: {len(wavs[0])/fs:.1f}s audio", flush=True)
except Exception as e:
    print(f"ERROR: {e}", flush=True)
    import traceback; traceback.print_exc()
print(f"DONE: {time.time()-t0:.1f}s", flush=True)
