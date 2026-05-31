"""
std_abs_benchmark.py — Compute weight_std + max_abs per tensor/zone from .gsidx + .qdat
Output: per-tensor JSON + per-zone summary table + MCP-ready card metadata
"""
import os, sys, json, struct, math, argparse
from pathlib import Path
import numpy as np

p = argparse.ArgumentParser()
p.add_argument("--gsidx", required=True)
p.add_argument("--qdat-dir", required=True)
p.add_argument("--out", default="bench_results.json")
p.add_argument("--model", default="unknown")
args = p.parse_args()

with open(args.gsidx) as f:
    index = json.load(f)

results = []
zone_data = {}

for entry in index:
    name = entry["name"]
    safe = name.replace("/", "_")
    path = os.path.join(args.qdat_dir, f"{safe}.qdat")
    if not os.path.exists(path):
        print(f"  skip {name} — no .qdat")
        continue

    raw = np.fromfile(path, dtype=np.uint8)
    shape = entry["shape"]
    dtype_code = entry["dtype"]  # 0=F32, 1=F16, ...

    if dtype_code == 0:  # F32
        arr = raw.view(np.float32)
    elif dtype_code == 1:  # F16
        arr = raw.view(np.float16).astype(np.float32)
    else:
        raise ValueError(f"unsupported dtype {dtype_code} for {name}")

    std = float(np.std(arr))
    max_abs = float(np.max(np.abs(arr)))
    zone = entry.get("zone", 0)
    nbytes = entry.get("nbytes", len(raw))

    r = {
        "name": name, "zone": zone, "shape": shape,
        "std": round(std, 6), "max_abs": round(max_abs, 6),
        "nbytes": nbytes, "dtype": dtype_code,
    }
    results.append(r)
    zone_data.setdefault(zone, {"stds": [], "maxs": [], "names": []})
    zone_data[zone]["stds"].append(std)
    zone_data[zone]["maxs"].append(max_abs)
    zone_data[zone]["names"].append(name)

# Per-zone aggregation
print(f"\n{'='*70}")
print(f"std/max_abs benchmark — {args.model}")
print(f"{'='*70}")
print(f"{'Zone':>5} {'Tensors':>8} {'AvgStd':>10} {'MaxStd':>10} {'AvgMax':>10} {'MaxMax':>10} {'Precision':>10}")
print(f"{'-'*70}")
for z in sorted(zone_data):
    d = zone_data[z]
    avg_std = sum(d["stds"]) / len(d["stds"])
    max_std = max(d["stds"])
    avg_max = sum(d["maxs"]) / len(d["maxs"])
    max_max = max(d["maxs"])
    if avg_std < 0.05:
        prec = "Q4"
    elif avg_std < 0.15:
        prec = "Q8"
    else:
        prec = "F16"
    print(f"{z:>5} {len(d['stds']):>8} {avg_std:>10.4f} {max_std:>10.4f} {avg_max:>10.2f} {max_max:>10.2f} {prec:>10}")

# Save full results
output = {"model": args.model, "per_tensor": results, "per_zone": {
    str(z): {
        "avg_std": round(sum(v["stds"])/len(v["stds"]), 4),
        "max_std": round(max(v["stds"]), 4),
        "avg_max_abs": round(sum(v["maxs"])/len(v["maxs"]), 2),
        "n_tensors": len(v["stds"]),
        "precision_hint": "Q4" if sum(v["stds"])/len(v["stds"]) < 0.05
            else "Q8" if sum(v["stds"])/len(v["stds"]) < 0.15
            else "F16",
        "tensors": v["names"],
    } for z, v in sorted(zone_data.items())
}}
json.dump(output, open(args.out, "w"), indent=2)
print(f"\nSaved → {args.out} ({len(results)} tensors)")
