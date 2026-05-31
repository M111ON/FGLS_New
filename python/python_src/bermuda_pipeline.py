"""
bermuda_pipeline.py — Bermuda Router + Bond Layer Integration
=============================================================
Float → Bermuda route → verdict → bond pieces → POGLS pipeline

Integration with existing pipeline (geo_field_bridge.py):
  The pipeline adds float routing on top of the existing file-based
  pipeline. After geo_field_bridge produces topology_fp and bond
  pieces for binary files, bermuda_pipeline handles float tensors.

Flow:
  Float [B,S,D] → BermudaRouter.route(mode)
    → RoutingVerdict (shape, polarity, zone, tring_slot per token)
    → verdict → bond pieces (grouped by zone/shape)
    → pieces → slots → plug chain → ready for FGLS store

Usage:
  from bermuda_pipeline import BermudaPipeline
  pipe = BermudaPipeline(dim=128, gear=2)
  result = pipe.process_float(tensor, mode=0)
  print(pipe.summary(result))
"""

import sys, torch
from pathlib import Path
from typing import Optional

# Ensure python_src is in path
PYTHON_SRC = Path(__file__).resolve().parent
if str(PYTHON_SRC) not in sys.path:
    sys.path.insert(0, str(PYTHON_SRC))

# bermuda router lives in collection root
COLLECTION = PYTHON_SRC.parent
if str(COLLECTION) not in sys.path:
    sys.path.insert(0, str(COLLECTION))

from bermuda_router_v1 import BermudaRouter, RoutingVerdict, DEVICE
from bermuda_bond_bridge import BermudaBondBridge, PoglsPiece, PoglsSlot

# C bridge (compiled) — replaces pure Python pogls_bond_py
_bridge = BermudaBondBridge()
_bridge.bermuda_init()
# Set session nonce to match Python default
_bridge.set_nonce(0x0000000000000000)

PLUG_N, PLUG_S, PLUG_E, PLUG_W = 0, 1, 2, 3


class BermudaPipeline:
    """
    Integrates bermuda float routing with POGLS bond + storage layer.

    Flow:
      Float [B,S,D]
        → BermudaRouter.route(mode) → RoutingVerdict
        → verdict → bond pieces (one per unique zone/shape)
        → pieces → slots → plug chain → FGLS-ready

    Usage:
      pipe = BermudaPipeline(dim=128, gear=2)
      result = pipe.process_float(tensor, mode=2)  # CROSS
      pipe.summary(result)
    """

    # shape char → fold_axis for bond_py.make_piece
    SHAPE_AXIS = {
        'I': 1, 'O': 2, 'T': 3,
        'S': 4, 'Z': 5, 'L': 6, 'J': 7,
    }

    def __init__(self, dim: int = 128, gear: int = 2,
                 session_nonce: int = 0x424D524D00000001):
        self.router = BermudaRouter(dim=dim, gear=gear)
        self.session_nonce = session_nonce
        self._route_count = 0

    def process_float(self, tensor: torch.Tensor, mode: int = 0) -> dict:
        """
        Full pipeline: float → routing verdict → bond pieces → slots.

        Args:
          tensor: [B,S,D] or [N,D] float tensor
          mode: 0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB

        Returns:
          {
            "verdict": RoutingVerdict,
            "pieces": {pid: {piece, zone, shape, count, tring_slots}},
            "slots":  {pid: {slot, info}},
            "shadow_norm": float,
            "mode": int,
          }
        """
        # Step 1: Bermuda route
        verdict = self.router.route(tensor, mode)

        # Step 2: Build bond pieces from verdict
        pieces = self._verdict_to_pieces(verdict)

        # Step 3: Create slots + wire plugs
        slots = self._pieces_to_slots(pieces)

        self._route_count += 1

        return {
            "verdict": verdict,
            "pieces": pieces,
            "slots": slots,
            "n_real": verdict.n_tokens,
            "shadow_norm": verdict.shadow_norm,
            "mode": mode,
            "gear": verdict.gear,
        }

    def _verdict_to_pieces(self, verdict: RoutingVerdict) -> dict:
        """Group tokens by (zone, shape) via torch.unique (GPU, ~0.5ms)
        → one bond piece per group via C bridge make_piece."""
        real = verdict.real_mask
        zones = verdict.zone[real]
        shapes = verdict.shape[real]
        tring = verdict.tring_slot[real]
        idx_in = verdict.idx_in[real]
        idx_out = verdict.idx_out[real]

        # torch.unique groups — avoids O(N) Python loop over tokens
        keys = zones.long() * 256 + shapes.long()
        unq_keys, inv, cnt = torch.unique(keys, return_inverse=True,
                                          return_counts=True)
        N = len(unq_keys)

        # First occurrence of each group in original (pre-sort) order
        arange = torch.arange(len(keys), device=DEVICE)
        first_pos = torch.full((N,), len(keys), dtype=torch.long,
                               device=DEVICE)
        first_pos.scatter_reduce_(0, inv, arange, 'amin')
        first_pos = first_pos.clamp(max=len(keys) - 1)

        first_zones = torch.gather(zones, 0, first_pos)
        first_shapes = torch.gather(shapes, 0, first_pos)
        first_tring = torch.gather(tring, 0, first_pos)
        first_idx_in = torch.gather(idx_in, 0, first_pos)
        first_idx_out = torch.gather(idx_out, 0, first_pos)

        z_list = first_zones.cpu().tolist()
        s_list = first_shapes.cpu().tolist()
        c_list = cnt.cpu().tolist()
        f_in = first_idx_in.cpu().tolist()
        f_out = first_idx_out.cpu().tolist()
        t_list = first_tring.cpu().tolist()

        pieces = {}
        nonce = self.session_nonce
        for i in range(N):
            zone = z_list[i]
            shape_char = chr(s_list[i])
            seed = nonce ^ (f_in[i] & 0xFFFF) ^ ((f_out[i] & 0xFFFF) << 16)
            axis = self.SHAPE_AXIS.get(shape_char, 1)
            piece = _bridge.make_piece(seed, axis)
            pid = f"B{self._route_count:04d}_z{zone}_{shape_char}"
            pieces[pid] = {
                "piece": piece,
                "zone": zone,
                "shape": shape_char,
                "count": c_list[i],
                "tring_slots": [t_list[i]],
                "axis": axis,
                "seed": seed,
            }

        return pieces

    def _pieces_to_slots(self, pieces: dict) -> dict:
        """Create bond slots and wire E→W plug chain via C bridge."""
        pids = sorted(pieces.keys())
        slots = {}

        for i, pid in enumerate(pids):
            info = pieces[pid]
            slot = _bridge.make_slot(
                seed=info["seed"],
                axis=info["axis"],
                agent_id=i,
                token_cap=300,
            )
            slots[pid] = {"slot": slot, "info": info}

        # Wire chain via C bridge: each → next via E→W
        for i in range(len(pids) - 1):
            _bridge.plug_connect(
                slots[pids[i]]["slot"], PLUG_E,
                slots[pids[i + 1]]["slot"], PLUG_W,
                ttl=64,
            )

        return slots

    def verify_bonds(self, result: dict) -> list:
        """Verify intrinsic bonds between adjacent slots in the chain via C bridge."""
        slots = result["slots"]
        pids = sorted(slots.keys())
        results = []
        for i in range(len(pids) - 1):
            a = slots[pids[i]]["slot"].piece
            b = slots[pids[i + 1]]["slot"].piece
            v = _bridge.bond_verify(a, b)
            results.append({
                "a": pids[i], "b": pids[i + 1],
                "bond_key": f"{v['bond_key']:016x}",
                "valid": v["valid"],
            })
        return results

    def report(self, result: dict) -> str:
        """Full integration report."""
        v = result["verdict"]
        pieces = result["pieces"]
        bonds = self.verify_bonds(result)

        real = v.real_mask
        route_n = (v.polarity[real] == 0).sum().item()
        ground_n = (v.polarity[real] == 1).sum().item()
        modes = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]

        lines = []
        lines.append("=" * 60)
        lines.append(f"BermudaPipeline  mode={modes[v.mode]}  "
                      f"gear={v.gear}")
        lines.append("=" * 60)
        lines.append(f"  Float tokens: {v.n_tokens}  "
                      f"padding: {v.pad_len}")
        lines.append(f"  Routing: ROUTE={route_n}  "
                      f"GROUND={ground_n}")
        lines.append(f"  Shadow norm: {v.shadow_norm:.2f}")

        lines.append(f"\n  Bond pieces ({len(pieces)}):")
        for pid, info in sorted(pieces.items()):
            tring = ",".join(str(s) for s in info["tring_slots"][:5])
            if len(info["tring_slots"]) > 5:
                tring += f"...(+{len(info['tring_slots'])-5})"
            geo = info['piece'].geo_key
            lines.append(
                f"    {pid}: zone={info['zone']:2d} "
                f"shape={info['shape']} "
                f"count={info['count']:3d} "
                f"geo=0x{geo:016x} "
                f"tring=[{tring}]"
            )

        lines.append(f"\n  Bond chain ({len(bonds)} links):")
        for b in bonds:
            status = "✓" if b["valid"] else "✗"
            lines.append(f"    {b['a']} ── {b['b']}  "
                          f"key={b['bond_key']}  {status}")

        lines.append(f"\n  Ready for: "
                      f"{'FGLS store' if route_n > 0 else ''} "
                      f"{'LC-GCFS' if ground_n > 0 else ''}")
        return "\n".join(lines)


# ── Demo: integrate with geo_field_bridge workflow ─────────
def demo_with_geo_field():
    """Simulate how bermuda pipeline fits with geo_field_bridge processing."""
    print("=" * 60)
    print("BermudaPipeline + geo_field_bridge Integration Demo")
    print("=" * 60)

    # Simulate geo_field_bridge output (what it normally returns from a file)
    geofield_result = {
        "file": "example_embedding.bin",
        "size": 8192,
        "chunks": 128,
        "topology_fp": "a3f0b2c1d4e5f6a7",
        "roundtrip": "PASS",
    }

    # Create synthetic float data (simulates LLM embedding)
    torch.manual_seed(42)
    DIM = 128
    data = torch.randn(256, DIM, device=DEVICE) * 2 + 0.5

    # Initialize bermuda pipeline alongside geofield pipeline
    pipe = BermudaPipeline(dim=DIM, gear=2)

    # Process float through bermuda → bond pieces
    print(f"\n[1] geo_field_bridge processes binary file:")
    print(f"    file={geofield_result['file']} "
          f"topology_fp={geofield_result['topology_fp']}")
    print(f"    → creates wallet + bond pieces for binary chunks")

    print(f"\n[2] bermuda_pipeline processes float tensor "
          f"(same session, nonce=0x{pipe.session_nonce:016x}):")
    print(f"    tensor shape: {list(data.shape)}")

    for mode, label in [(0, "ORBITAL"), (1, "CHIRAL"),
                         (2, "CROSS"), (3, "HUB")]:
        result = pipe.process_float(data, mode)
        print(f"\n  ── {label} ──")
        print(pipe.report(result))

    print(f"\n[3] Combined pipeline would now:")
    print(f"    - Store bond pieces from binary in FGLS")
    print(f"    - Route float tokens per bermuda verdict")
    print(f"    - ROUTE tokens → FGLS Twin Store")
    print(f"    - GROUND tokens → LC-GCFS cold store")
    print(f"    - Shadow residual preserved for fidelity check")


if __name__ == "__main__":
    demo_with_geo_field()
