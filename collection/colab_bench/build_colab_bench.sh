#!/usr/bin/env bash
# build_colab_bench.sh — Prepare colab_bench bundle for Colab upload
# Usage: bash build_colab_bench.sh
# Output: colab_bench.zip (upload to Colab, extract, run)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V2_DIR="$SCRIPT_DIR/../colab_bundle_v2"
OUT_ZIP="$SCRIPT_DIR/colab_bench.zip"

echo "=== FGLS Colab Bench Builder ==="

# 1. Copy required Python modules from colab_bundle_v2
echo "--- Copying pipeline modules ---"
for f in pipeline_merged.py bermuda_reshape_v2.py fibo_clock.py; do
    src="$V2_DIR/$f"
    if [ -f "$src" ]; then
        cp "$src" "$SCRIPT_DIR/"
        echo "  ✓ $f"
    else
        echo "  ⚠ $f not found in colab_bundle_v2"
    fi
done

# 2. Create requirements
cat > "$SCRIPT_DIR/requirements.txt" << 'EOF'
torch>=2.0
numpy
gguf
psutil
matplotlib
wandb
EOF
echo "  ✓ requirements.txt"

# 3. Create Colab setup script
cat > "$SCRIPT_DIR/setup_colab.sh" << 'SETUP'
#!/usr/bin/env bash
set -euo pipefail
echo "=== FGLS Colab Bench Setup ==="
apt-get update -qq && apt-get install -y -qq gcc > /dev/null 2>&1
echo "✓ gcc installed"
pip install -q torch numpy gguf psutil matplotlib wandb
echo "✓ Python packages installed"
python3 -c "import torch; print(f'CUDA: {torch.cuda.is_available()}, GPU: {torch.cuda.get_device_name(0) if torch.cuda.is_available() else \"none\"}')" 2>/dev/null || true
echo "✓ Setup complete"
SETUP
chmod +x "$SCRIPT_DIR/setup_colab.sh"
echo "  ✓ setup_colab.sh"

# 4. Create the Colab notebook via Python
echo "--- Creating Colab notebook ---"
python3 << 'PYEOF'
import json, os

cells = []

cells.append({
    "cell_type": "markdown",
    "metadata": {},
    "source": [
        "# FGLS Multi-Model Benchmark\n",
        "Benchmark geometric compression across multiple GGUF models on Colab T4.\n",
        "\n",
        "Press **Runtime \u2192 Run all** to start.\n",
        "\n",
        "## What this benchmarks\n",
        "- **fibo_addr** throughput (geometric address computation)\n",
        "- **Pipeline throughput** (strip \u2192 bond \u2192 bermuda \u2192 reattach) across 4 traverse modes\n",
        "- **Code ramp** (code-space divergence, target >2x for ORBITAL/CHIRAL)\n",
        "- **Memory** (RAM/VRAM usage)\n",
        "- **Gate convergence** (BermudaGate training on real weights)"
    ]
})

cells.append({
    "cell_type": "code",
    "metadata": {},
    "source": [
        "#@title 1. Setup Environment { display-mode: \"form\" }\n",
        "!bash setup_colab.sh"
    ],
    "outputs": [],
    "execution_count": None
})

cells.append({
    "cell_type": "code",
    "metadata": {},
    "source": [
        "#@title 2. Upload & Extract Bundle { display-mode: \"form\" }\n",
        "import os\n",
        "BENCH_DIR = \"/content/colab_bench\"\n",
        "if not os.path.exists(BENCH_DIR):\n",
        "    from google.colab import files\n",
        "    print(\"Upload colab_bench.zip:\")\n",
        "    uploaded = files.upload()\n",
        "    fname = list(uploaded.keys())[0]\n",
        "    !unzip -qo {fname} -d /content/\n",
        "else:\n",
        "    print(f\"\u2713 Bundle already at {BENCH_DIR}\")\n",
        "\n",
        "os.chdir(BENCH_DIR)\n",
        "import sys; sys.path.insert(0, BENCH_DIR)\n",
        "print(f\"\u2713 Working directory: {os.getcwd()}\")"
    ],
    "outputs": [],
    "execution_count": None
})

cells.append({
    "cell_type": "code",
    "metadata": {},
    "source": [
        "#@title 3. Quick Sanity Check { display-mode: \"form\" }\n",
        "import torch\n",
        "print(f\"PyTorch: {torch.__version__}\")\n",
        "print(f\"CUDA: {torch.cuda.is_available()}\")\n",
        "if torch.cuda.is_available():\n",
        "    print(f\"GPU: {torch.cuda.get_device_name(0)}\")\n",
        "    print(f\"VRAM: {torch.cuda.get_device_properties(0).total_mem / 1e9:.1f} GB\")\n",
        "\n",
        "from bench_runner import bench_fibo_addr\n",
        "ops = bench_fibo_addr(10000)\n",
        "print(f\"fibo_addr: {ops:,.0f} ops/s \u2713\")"
    ],
    "outputs": [],
    "execution_count": None
})

cells.append({
    "cell_type": "code",
    "metadata": {},
    "source": [
        "#@title 4. Run Benchmark { display-mode: \"form\" }\n",
        "#@markdown ### Model Selection\n",
        "models = \"qwen3-0.6b-q8\" #@param {type:\"string\"}\n",
        "#@markdown Comma-separated model IDs, or `all` for every model\n",
        "\n",
        "#@markdown ### Benchmark Config\n",
        "max_tokens = 576 #@param {type:\"integer\"}\n",
        "gate_epochs = 500 #@param {type:\"integer\"}\n",
        "shell_levels_str = \"0,1,2,3\" #@param {type:\"string\"}\n",
        "\n",
        "shell_levels = [int(x.strip()) for x in shell_levels_str.split(\",\")]\n",
        "model_list = None if models.strip().lower() == \"all\" else [m.strip() for m in models.split(\",\")]\n",
        "\n",
        "from bench_runner import bench_all_models, print_comparison_table, save_results_json, plot_comparison\n",
        "from pathlib import Path\n",
        "\n",
        "results = bench_all_models(\n",
        "    model_ids=model_list,\n",
        "    max_tokens=max_tokens,\n",
        "    shell_levels=shell_levels,\n",
        "    gate_epochs=gate_epochs,\n",
        ")\n",
        "\n",
        "print_comparison_table(results)\n",
        "save_results_json(results, Path(\"/content/bench_results.json\"))\n",
        "plot_comparison(results, Path(\"/content/bench_plots\"))"
    ],
    "outputs": [],
    "execution_count": None
})

cells.append({
    "cell_type": "code",
    "metadata": {},
    "source": [
        "#@title 5. (Optional) Log to Weights & Biases { display-mode: \"form\" }\n",
        "use_wandb = False #@param {type:\"boolean\"}\n",
        "wandb_project = \"fgls-bench\" #@param {type:\"string\"}\n",
        "\n",
        "if use_wandb:\n",
        "    from bench_runner import log_to_wandb\n",
        "    log_to_wandb(results, project=wandb_project)\n",
        "else:\n",
        "    print(\"W&B logging skipped.\")"
    ],
    "outputs": [],
    "execution_count": None
})

cells.append({
    "cell_type": "code",
    "metadata": {},
    "source": [
        "#@title 6. Download Results { display-mode: \"form\" }\n",
        "from google.colab import files\n",
        "import os, zipfile\n",
        "\n",
        "with zipfile.ZipFile(\"/content/bench_results.zip\", \"w\") as zf:\n",
        "    for f in [\"/content/bench_results.json\"]:\n",
        "        if os.path.exists(f):\n",
        "            zf.write(f, os.path.basename(f))\n",
        "    plots_dir = \"/content/bench_plots\"\n",
        "    if os.path.isdir(plots_dir):\n",
        "        for f in os.listdir(plots_dir):\n",
        "            zf.write(os.path.join(plots_dir, f), f\"plots/{f}\")\n",
        "\n",
        "files.download(\"/content/bench_results.zip\")\n",
        "print(\"\u2713 Results downloaded\")"
    ],
    "outputs": [],
    "execution_count": None
})

nb = {
    "nbformat": 4,
    "nbformat_minor": 0,
    "metadata": {
        "accelerator": "GPU",
        "colab": {"gpuType": "T4", "provenance": [], "include_colab_link": True},
        "kernelspec": {"display_name": "Python 3", "name": "python3"},
        "language_info": {"name": "python"}
    },
    "cells": cells
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)) if "__file__" in dir() else ".", "FGLS_Bench_Colab.ipynb")
# When run from bash, __file__ is not defined, so write to cwd
out = "FGLS_Bench_Colab.ipynb"
with open(out, "w") as f:
    json.dump(nb, f, indent=1)
print(f"  ✓ {out}")
PYEOF
echo "  ✓ Notebook created"

# 5. Package into zip
echo "--- Packaging colab_bench.zip ---"
cd "$SCRIPT_DIR"
zip -r "$OUT_ZIP" \
    bench_runner.py \
    model_registry.json \
    pipeline_merged.py \
    bermuda_reshape_v2.py \
    fibo_clock.py \
    setup_colab.sh \
    requirements.txt \
    FGLS_Bench_Colab.ipynb \
    2>/dev/null

SZ=$(wc -c < "$OUT_ZIP" 2>/dev/null || stat -f%z "$OUT_ZIP" 2>/dev/null)
echo "✓ Created $OUT_ZIP ($SZ bytes)"
echo ""
echo "=== Next steps ==="
echo "1. Open Colab: https://colab.research.google.com"
echo "2. Upload colab_bench.zip"
echo "3. Open FGLS_Bench_Colab.ipynb"
echo "4. Runtime → Run all"
