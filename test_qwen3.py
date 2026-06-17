"""Test Qwen3-TTS with local model weights (manual load for memory efficiency)."""
import sys, os, time, torch, gc, numpy as np
from pathlib import Path

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

# Init in half-precision to save CPU RAM
model = Qwen3TTSForConditionalGeneration(config).half()
print(f"Model init: {time.time()-t0:.1f}s", flush=True)

# Load weights
sd = load_file(f"{MODEL_DIR}/model.safetensors")
model.load_state_dict(sd, strict=False)
del sd; gc.collect()
print(f"Weights loaded: {time.time()-t0:.1f}s", flush=True)

# Move to GPU
model = model.to("cuda", dtype=torch.float16)
print(f"GPU: {time.time()-t0:.1f}s", flush=True)

# Speech tokenizer (loads directly on GPU)
gc.collect()
st = Qwen3TTSTokenizer.from_pretrained(
    f"{MODEL_DIR}/speech_tokenizer",
    device_map="cuda",
    dtype="float16",
)
model.load_speech_tokenizer(st)
print(f"Speech tokenizer: {time.time()-t0:.1f}s", flush=True)

# Processor
processor = AutoProcessor.from_pretrained(MODEL_DIR, fix_mistral_regex=True)
wrapper = Qwen3TTSWrapper(model=model, processor=processor)
print(f"Ready: {time.time()-t0:.1f}s", flush=True)

# Dummy reference audio (sine wave, 1 sec @ 24kHz)
sr = 24000
dummy_audio = np.sin(2 * np.pi * 440 * np.arange(sr) / sr).astype(np.float32)

def test(text, lang, fname, max_tokens=2048):
    tt = time.time()
    wavs, fs = wrapper.generate_voice_clone(
        text=text, language=lang,
        ref_audio=(dummy_audio, sr),
        x_vector_only_mode=True,
        max_audio_tokens=max_tokens,
    )
    dur = len(wavs[0]) / fs
    print(f"  [{lang}] {dur:.1f}s audio in {time.time()-tt:.1f}s", flush=True)
    import soundfile as sf
    sf.write(f"build/{fname}", wavs[0], fs)

test("Hello, this is a test of Qwen Three TTS system.", "english", "test_qwen3_en.wav", 2048)
test("สวัสดีครับ ทดสอบระบบเสียงภาษาไทย", "thai", "test_qwen3_th.wav", 4096)
test("你好，世界！这是一个测试。", "chinese", "test_qwen3_zh.wav", 2048)

print(f"\n✓ All tests complete ({time.time()-t0:.1f}s total)")
