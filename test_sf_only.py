"""Test just safetensors loading."""
import os, sys
os.environ["HF_HOME"] = "I:/model/.cache/hf"
import torch
import safetensors
print("safetensors", safetensors.__version__)
from safetensors.torch import load_file
sd = load_file("I:/model/qwen3-tts-0.6b/model.safetensors")
print("loaded", len(sd), "tensors")
total = sum(t.numel() * t.element_size() for t in sd.values())
print(f"total size: {total/1024**3:.2f} GB")
