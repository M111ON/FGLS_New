#!/usr/bin/env bash
set -euo pipefail
echo "=== FGLS Colab Bench Setup ==="
apt-get update -qq && apt-get install -y -qq gcc > /dev/null 2>&1
echo "✓ gcc installed"
pip install -q torch numpy gguf safetensors psutil matplotlib wandb
echo "✓ Python packages installed"
python3 -c "import torch; print(f'CUDA: {torch.cuda.is_available()}, GPU: {torch.cuda.get_device_name(0) if torch.cuda.is_available() else \"none\"}')" 2>/dev/null || true
echo "✓ Setup complete"
