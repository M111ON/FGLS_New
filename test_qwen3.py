"""Test Qwen3-TTS with local model weights."""
import sys, os, time, torch
from pathlib import Path

os.environ["HF_HOME"] = "I:/model/.cache/huggingface"

t0 = time.time()
from qwen_tts import Qwen3TTSModel

MODEL_DIR = "I:/model/qwen3-tts-0.6b"
print(f"Loading Qwen3-TTS 0.6B from {MODEL_DIR}...")
model = Qwen3TTSModel.from_pretrained(
    MODEL_DIR,
    device_map="cpu",
    dtype=torch.float32,
)
load_ms = int((time.time()-t0)*1000)
print(f"Loaded in {load_ms}ms")

# Test English
t1 = time.time()
result = model.synthesize(
    "Hello, this is a test of Qwen Three TTS system.",
    language="en",
    max_audio_tokens=2048,
)
synth_ms = int((time.time()-t1)*1000)
print(f"\nEnglish synthesized in {synth_ms}ms")
print(f"Audio shape: {result.audio.shape}")
print(f"Sample rate: {result.sample_rate}")

import soundfile as sf
sf.write("build/test_qwen3_en.wav", result.audio, result.sample_rate)
print("Saved: build/test_qwen3_en.wav")

# Test Thai
t2 = time.time()
result2 = model.synthesize(
    "สวัสดีครับ ทดสอบระบบเสียงภาษาไทย",
    language="th",
    max_audio_tokens=4096,
)
thai_ms = int((time.time()-t2)*1000)
dur = len(result2.audio) / result2.sample_rate
print(f"\nThai synthesized in {thai_ms}ms, duration {dur:.1f}s")
sf.write("build/test_qwen3_th.wav", result2.audio, result2.sample_rate)
print("Saved: build/test_qwen3_th.wav")

# Test Chinese
t3 = time.time()
result3 = model.synthesize(
    "你好，世界！这是一个测试。",
    language="zh",
    max_audio_tokens=2048,
)
cn_ms = int((time.time()-t3)*1000)
dur3 = len(result3.audio) / result3.sample_rate
print(f"\nChinese synthesized in {cn_ms}ms, duration {dur3:.1f}s")
sf.write("build/test_qwen3_zh.wav", result3.audio, result3.sample_rate)
print("Saved: build/test_qwen3_zh.wav")

print("\n✓ All tests complete")
