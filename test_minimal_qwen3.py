"""Minimal Qwen3 TTS test."""
import os, torch, gc, sys
os.environ["HF_HOME"] = "I:/model/.cache/hf"
from qwen_tts.core.models import Qwen3TTSConfig, Qwen3TTSForConditionalGeneration
from safetensors.torch import load_file

print("loading config", flush=True)
config = Qwen3TTSConfig.from_pretrained("I:/model/qwen3-tts-0.6b")
print("config ok", flush=True)

print("creating model...", flush=True)
model = Qwen3TTSForConditionalGeneration(config).half()
print("model created", flush=True)

print("loading sd...", flush=True)
sd = load_file("I:/model/qwen3-tts-0.6b/model.safetensors")
print(f"sd loaded: {len(sd)} keys", flush=True)

print("loading state_dict...", flush=True)
model.load_state_dict(sd, strict=False)
print("state_dict loaded", flush=True)

del sd; gc.collect()
model = model.to("cuda", dtype=torch.float16)
print("model moved to cuda", flush=True)

print("DONE", flush=True)
