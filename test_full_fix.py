"""Full fix: stream-load Qwen3 TTS + monkey-patch vocab + test."""
import os, sys, time, gc, numpy as np
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
import torch.nn as nn
from safetensors import safe_open
from transformers import AutoProcessor
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration, Qwen3TTSProcessor
from qwen_tts.inference.qwen3_tts_model import Qwen3TTSModel as Qwen3TTSWrapper
from qwen_tts.inference.qwen3_tts_tokenizer import Qwen3TTSTokenizer
from transformers.modeling_rope_utils import ROPE_INIT_FUNCTIONS

MODEL_DIR = "I:/model/qwen3-tts-0.6b"
t0 = time.time()

# ── 1. Config ──
config = Qwen3TTSConfig.from_pretrained(MODEL_DIR)
print(f"[{time.time()-t0:.1f}s] config", flush=True)

# ── 2. Meta model ──
with torch.device("meta"):
    model = Qwen3TTSForConditionalGeneration(config)
print(f"[{time.time()-t0:.1f}s] meta model", flush=True)

# ── 3. Load weight one-by-one ──
param_map = {n: p for n, p in model.named_parameters()}
buffer_map = {n: b for n, b in model.named_buffers()}
with safe_open(f"{MODEL_DIR}/model.safetensors", framework="pt", device="cpu") as f:
    keys = f.keys()
    for i, k in enumerate(keys):
        t = f.get_tensor(k)
        if k in param_map:
            param_map[k].data.copy_(t)
        elif k in buffer_map:
            buffer_map[k].data.copy_(t)
        del t
        if (i+1) % 100 == 0:
            gc.collect()
            print(f"[{time.time()-t0:.1f}s] {i+1}/{len(keys)}", flush=True)

print(f"[{time.time()-t0:.1f}s] weights loaded", flush=True)
gc.collect()

# ── 4. half ──
model = model.to(dtype=torch.float16)
print(f"[{time.time()-t0:.1f}s] half", flush=True)

# ── 5. Fix rotary buffers ──
rope_fn = ROPE_INIT_FUNCTIONS['default']
for sub in [model.talker.model, model.talker.code_predictor.model]:
    if hasattr(sub, 'rotary_emb') and hasattr(sub.rotary_emb, 'inv_freq') and sub.rotary_emb.inv_freq.is_meta:
        inv_freq, attn_scaling = rope_fn(sub.rotary_emb.config, device='cpu')
        sub.rotary_emb.inv_freq = nn.Parameter(inv_freq, requires_grad=False)
print(f"[{time.time()-t0:.1f}s] rope fixed", flush=True)

# ── 6. CUDA ──
model = model.to("cuda")
print(f"[{time.time()-t0:.1f}s] CUDA, VRAM={torch.cuda.memory_allocated()/1024**3:.2f}GB", flush=True)

# ── 7. Speech tokenizer ──
gc.collect()
st = Qwen3TTSTokenizer.from_pretrained(f"{MODEL_DIR}/speech_tokenizer", device_map="cuda", dtype="float16")
model.load_speech_tokenizer(st)
print(f"[{time.time()-t0:.1f}s] speech tokenizer", flush=True)

# ── 8. Processor + Wrapper ──
AutoProcessor.register(Qwen3TTSConfig, Qwen3TTSProcessor)
processor = AutoProcessor.from_pretrained(MODEL_DIR, fix_mistral_regex=True)
wrapper = Qwen3TTSWrapper(model=model, processor=processor)
print(f"[{time.time()-t0:.1f}s] wrapper ready", flush=True)

# ── 9. Monkey-patch: clamp input_ids before embedding lookups ──
talker = model.talker
cp = talker.code_predictor

# Patch talker codec_embedding
orig_codec_emb = talker.model.codec_embedding
class SafeCodecEmb(nn.Module):
    def __init__(self, emb):
        super().__init__()
        self.emb = emb
        self.vs = emb.num_embeddings
    @property
    def weight(self): return self.emb.weight
    def forward(self, x):
        if x.numel() > 0 and x.max() >= self.vs:
            print(f"[FIX] codec_emb clamp: max={x.max().item()} vs={self.vs}", flush=True)
            x = x.clamp(0, self.vs - 1)
        return self.emb(x)
talker.model.codec_embedding = SafeCodecEmb(orig_codec_emb)
talker.model.get_input_embeddings = lambda: talker.model.codec_embedding
talker.get_input_embeddings = lambda: talker.model.codec_embedding

# Patch code_predictor embeddings
orig_cp_list = cp.model.codec_embedding
class SafeCPEmbList(nn.Module):
    def __init__(self, orig):
        super().__init__()
        self._emb = orig
        self._vs = [e.num_embeddings for e in orig]
    def __getitem__(self, i):
        e = self._emb[i]
        vs = self._vs[i]
        def make_forward(emb, vsize):
            def forward(x):
                if x.numel() > 0 and x.max() >= vsize:
                    print(f"[FIX] cp_emb[{i}] clamp: max={x.max().item()} vs={vsize}", flush=True)
                    return emb(x.clamp(0, vsize - 1))
                return emb(x)
            return forward
        clamp_mod = nn.Module()
        clamp_mod.forward = make_forward(e, vs)
        return clamp_mod
    def __len__(self): return len(self._emb)
    def __iter__(self):
        for i in range(len(self._emb)): yield self[i]
cp.model.codec_embedding = SafeCPEmbList(orig_cp_list)
cp.model.get_input_embeddings = lambda: cp.model.codec_embedding
cp.get_input_embeddings = lambda: cp.model.codec_embedding

# ── 10. Test ──
sr = 24000
dummy_audio = np.sin(2 * np.pi * 440 * np.arange(sr) / sr).astype(np.float32)
print("\n=== Testing TTS ===", flush=True)
try:
    wavs, fs = wrapper.generate_voice_clone(
        text="Hello, this is a test.",
        language="english",
        ref_audio=(dummy_audio, sr),
        x_vector_only_mode=True,
        max_audio_tokens=256,
    )
    import soundfile as sf
    os.makedirs("build", exist_ok=True)
    sf.write("build/test_qwen3_fixed.wav", wavs[0], fs)
    print(f"SUCCESS: {len(wavs[0])/fs:.1f}s audio → build/test_qwen3_fixed.wav", flush=True)
except Exception as e:
    print(f"ERROR: {e}", flush=True)
    import traceback; traceback.print_exc()
