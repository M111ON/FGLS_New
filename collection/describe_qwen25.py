"""Generate Qwen2.5-0.5B architecture reference JSON from cached GGUF."""
import gguf, json, os, re
from collections import Counter

def get_str(field):
    """Decode string from GGUF field. Last parts entry is byte array."""
    raw = field.parts[-1]
    if hasattr(raw, "tobytes"):
        return raw.tobytes().decode("utf-8", errors="replace")
    return str(raw)

def get_int(field):
    """Get integer from GGUF field."""
    return field.parts[-1].item()

CACHE = r"C:\Users\Administrator.AVENTADOR\.cache\huggingface\hub\models--Qwen--Qwen2.5-0.5B-Instruct-GGUF\snapshots\9217f5db79a29953eb74d5343926648285ec7e67\qwen2.5-0.5b-instruct-q8_0.gguf"

reader = gguf.GGUFReader(CACHE)
fields = reader.fields

arch = get_str(fields["general.architecture"])
n_layers = get_int(fields[f"{arch}.block_count"])
dim = get_int(fields[f"{arch}.embedding_length"])
ffn_dim = get_int(fields[f"{arch}.feed_forward_length"])
n_heads = get_int(fields[f"{arch}.attention.head_count"])
n_kv_heads = get_int(fields[f"{arch}.attention.head_count_kv"])
ctx_len = get_int(fields[f"{arch}.context_length"])
file_type = get_int(fields["general.file_type"])

# Tensor pattern per block
block_tensors = []
global_tensors = []
for t in reader.tensors:
    entry = {
        "name": t.name,
        "shape": [int(s) for s in t.shape],
        "dtype": str(t.tensor_type).split(".")[-1],
        "nbytes": int(t.n_bytes),
    }
    if t.name.startswith("blk."):
        block_tensors.append(entry)
    else:
        global_tensors.append(entry)

# Group by block
block_groups = {}
for t in block_tensors:
    m = re.match(r"blk\.(\d+)", t["name"])
    if m:
        idx = int(m.group(1))
        block_groups.setdefault(idx, []).append(t["name"])

total_bytes = sum(t.n_bytes for t in reader.tensors)

spec = {
    "model": "Qwen2.5-0.5B-Instruct",
    "file": "qwen2.5-0.5b-instruct-q8_0.gguf",
    "size_mb": round(total_bytes / 1024 / 1024, 1),
    "architecture": arch,
    "quantization": "Q8_0",
    "file_type": file_type,
    "n_layers": n_layers,
    "dim": dim,
    "ffn_dim": ffn_dim,
    "n_heads": n_heads,
    "n_kv_heads": n_kv_heads,
    "ctx_len": ctx_len,
    "n_tensors": len(reader.tensors),
    "global_tensors": [],
    "tensors_per_block": 0,
    "block_pattern": [],
}

if 0 in block_groups:
    spec["tensors_per_block"] = len(block_groups[0])
    first_block = [t for t in block_tensors if t["name"].startswith("blk.0.")]
    for t in first_block:
        spec["block_pattern"].append({
            "name": t["name"].replace("blk.0.", "blk.N."),
            "shape": t["shape"],
            "dtype": t["dtype"],
        })

for t in global_tensors:
    spec["global_tensors"].append({"name": t["name"], "shape": t["shape"], "dtype": t["dtype"]})

spec["_summary"] = (f"{n_layers} layers x {spec['tensors_per_block']} tensors "
                    f"= {n_layers * spec['tensors_per_block']} block tensors "
                    f"+ {len(global_tensors)} global = {len(reader.tensors)} total")

out = os.path.join(os.path.dirname(__file__) or ".", "qwen25_arch.json")
with open(out, "w") as f:
    json.dump(spec, f, indent=2)
print(json.dumps(spec, indent=2))
print(f"\nSaved to {out}")
