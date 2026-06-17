"""Debug Qwen3 TTS with clamping + verbose logging."""
import os, sys, time, torch, gc, numpy as np
sys.setrecursionlimit(10000)

os.environ["HF_HOME"] = "I:/model/.cache/hf"

from transformers import AutoConfig, AutoModel, AutoProcessor
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration, Qwen3TTSProcessor
from qwen_tts.inference.qwen3_tts_model import Qwen3TTSModel as Qwen3TTSWrapper
from qwen_tts.inference.qwen3_tts_tokenizer import Qwen3TTSTokenizer
from safetensors.torch import load_file

MODEL_DIR = "I:/model/qwen3-tts-0.6b"

AutoConfig.register("qwen3_tts", Qwen3TTSConfig)
AutoModel.register(Qwen3TTSConfig, Qwen3TTSForConditionalGeneration)
AutoProcessor.register(Qwen3TTSConfig, Qwen3TTSProcessor)

t0 = time.time()
config = AutoConfig.from_pretrained(MODEL_DIR)
model = Qwen3TTSForConditionalGeneration(config).half()
sd = load_file(f"{MODEL_DIR}/model.safetensors")
model.load_state_dict(sd, strict=False)
del sd; gc.collect()
model = model.to("cuda", dtype=torch.float16)
print(f"[{time.time()-t0:.1f}s] Model on CUDA")

gc.collect()
st = Qwen3TTSTokenizer.from_pretrained(
    f"{MODEL_DIR}/speech_tokenizer", device_map="cuda", dtype="float16"
)
model.load_speech_tokenizer(st)
print(f"[{time.time()-t0:.1f}s] Speech tokenizer loaded")

processor = AutoProcessor.from_pretrained(MODEL_DIR, fix_mistral_regex=True)
wrapper = Qwen3TTSWrapper(model=model, processor=processor)
print(f"[{time.time()-t0:.1f}s] Wrapper ready")

talker = model.talker
cp = talker.code_predictor
print(f"codec_emb: vocab={talker.model.codec_embedding.num_embeddings} shape={talker.model.codec_embedding.weight.shape}")
print(f"text_emb:  vocab={talker.model.text_embedding.num_embeddings} shape={talker.model.text_embedding.weight.shape}")
print(f"cp_emb:    {len(cp.model.codec_embedding)} x vocab={cp.model.codec_embedding[0].num_embeddings} shape={cp.model.codec_embedding[0].weight.shape}")

# Patch: talker codec_embedding
orig_emb = talker.model.codec_embedding
class SafeCodecEmb(torch.nn.Module):
    def __init__(self, emb):
        super().__init__(); self.emb = emb; self.vs = emb.num_embeddings
    @property
    def weight(self): return self.emb.weight
    @weight.setter
    def weight(self, v): self.emb.weight = v
    def forward(self, x):
        if x.numel() and x.max() >= self.vs:
            print(f"[BUG] codec_emb: x.max={x.max().item()} >= vs={self.vs} shape={list(x.shape)}", flush=True)
            x = x.clamp(0, self.vs - 1)
        return self.emb(x)
talker.model.codec_embedding = SafeCodecEmb(orig_emb)
talker.model.get_input_embeddings = lambda: talker.model.codec_embedding
talker.get_input_embeddings = lambda: talker.model.codec_embedding

# Patch: code_predictor embeddings
orig_cp_list = cp.model.codec_embedding
class SafeCPEmbList(torch.nn.Module):
    def __init__(self, orig):
        super().__init__()
        self._emb = orig
        self._vs = [e.num_embeddings for e in orig]
    def __getitem__(self, i):
        e = self._emb[i]; vs = self._vs[i]
        class Inner(torch.nn.Module):
            def __init__(self): super().__init__()
            def forward(self, x):
                if x.numel() and x.max() >= vs:
                    print(f"[BUG] cp_emb[{i}]: x.max={x.max().item()} >= vs={vs} shape={list(x.shape)}", flush=True)
                    return e(x.clamp(0, vs - 1))
                return e(x)
        return Inner()
    def __len__(self): return len(self._emb)
    def __iter__(self):
        for i in range(len(self._emb)): yield self[i]
cp.model.codec_embedding = SafeCPEmbList(orig_cp_list)
cp.model.get_input_embeddings = lambda: cp.model.codec_embedding
cp.get_input_embeddings = lambda: cp.model.codec_embedding

sr = 24000
dummy_audio = np.sin(2 * np.pi * 440 * np.arange(sr) / sr).astype(np.float32)

import traceback
print("\n=== Testing English ===", flush=True)
try:
    wavs, fs = wrapper.generate_voice_clone(
        text="Hello, this is a test.",
        language="english",
        ref_audio=(dummy_audio, sr),
        x_vector_only_mode=True,
        max_audio_tokens=256,
    )
    import soundfile as sf
    sf.write("build/test_qwen3_fixed.wav", wavs[0], fs)
    print(f"SUCCESS: {len(wavs[0])/fs:.1f}s audio", flush=True)
except Exception as e:
    print(f"ERROR: {e}", flush=True)
    traceback.print_exc()
    # After error, check what's in input_ids on CPU
    torch.cuda.synchronize()
    print("CUDA synchronized", flush=True)
