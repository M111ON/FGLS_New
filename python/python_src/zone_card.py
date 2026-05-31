"""
zone_card.py — ZoneCard: tiny geometry signature for LLM routing
=================================================================

v2: Added locality + stability fields for richer routing decisions.

  LLM = planner   (sees card → decides route)
  Engine = executor (fetches real geometry based on route)

ZoneCard fields (~14 bytes unpacked, 12B packed):
  id         — zone index
  card_type  — SPARSE(0) / BATCH(1) / LZ(2)
  entropy    — byte-level entropy 0-255
  pattern    — 16-bit bit-pattern fingerprint
  locality   — 0-255 similarity to neighbor zones
  stability  — 0-255 row-to-row predictability
  neighbor   — (left_zone, right_zone) from registry
  hash_val   — 64-bit fingerprint (optional, extended format)
"""

from __future__ import annotations

import struct
import math
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional

import numpy as np

# ── Card types ─────────────────────────────────────────────────
CARD_SPARSE = 0
CARD_BATCH  = 1
CARD_LZ     = 2

CARD_NAMES = {CARD_SPARSE: "sparse", CARD_BATCH: "batch", CARD_LZ: "lz"}

# ── Packed binary format (v2) ───────────────────────────────────
#   [0:1]  id        u16
#   [1:2]  type      u8
#   [2:3]  entropy   u8
#   [3:4]  pattern   u16
#   [4:5]  locality  u8    NEW
#   [5:6]  stability u8    NEW
#   [6:8]  n_left    u16
#   [8:10] n_right   u16
#   = 12 bytes packed
# Extended (20 bytes):
#   [+10:18] hash    u64

CARD_PACK_FMT  = '<HBBHBBHH'
CARD_PACK_SZ   = struct.calcsize(CARD_PACK_FMT)
CARD_EXT_FMT   = '<HBBHBBHHQ'
CARD_EXT_SZ    = struct.calcsize(CARD_EXT_FMT)
NO_NEIGHBOR    = 0xFFFF

# Old v1 format (10B) — for backward compat
CARD_V1_FMT    = '<HBBHHH'
CARD_V1_SZ     = struct.calcsize(CARD_V1_FMT)
CARD_V1_EXT_FMT = '<HBBHHHQ'
CARD_V1_EXT_SZ = struct.calcsize(CARD_V1_EXT_FMT)

# Defaults for old-format cards (no locality/stability info)
_LEGACY_LOCALITY  = 128
_LEGACY_STABILITY = 128


@dataclass
class ZoneCard:
    """Compact geometry signature for LLM routing decisions."""
    id: int
    card_type: int
    entropy: int
    pattern: int
    locality: int = 128        # 0=unique 255=identical to neighbor
    stability: int = 128       # 0=unstable 255=perfectly predictable
    neighbor: tuple = (None, None)
    hash_val: int = 0

    def __post_init__(self):
        assert 0 <= self.id < 4096, f"id out of range: {self.id}"
        assert self.card_type in (0, 1, 2), f"bad card_type: {self.card_type}"
        assert 0 <= self.entropy <= 255, f"entropy out of range: {self.entropy}"
        assert 0 <= self.pattern < 65536, f"pattern out of range: {self.pattern}"
        assert 0 <= self.locality <= 255, f"locality out of range: {self.locality}"
        assert 0 <= self.stability <= 255, f"stability out of range: {self.stability}"

    @property
    def type_name(self) -> str:
        return CARD_NAMES.get(self.card_type, f"unknown({self.card_type})")

    def to_bytes(self, extended: bool = False) -> bytes:
        nl, nr = self.neighbor
        nl = nl if nl is not None else NO_NEIGHBOR
        nr = nr if nr is not None else NO_NEIGHBOR
        if extended:
            return struct.pack(CARD_EXT_FMT, self.id, self.card_type,
                               self.entropy, self.pattern, self.locality,
                               self.stability, nl, nr, self.hash_val)
        return struct.pack(CARD_PACK_FMT, self.id, self.card_type,
                           self.entropy, self.pattern, self.locality,
                           self.stability, nl, nr)

    @classmethod
    def from_bytes(cls, data: bytes) -> ZoneCard:
        """Deserialize. Handles both v2 (12B/20B) and v1 (10B/18B)."""
        n = len(data)
        if n >= CARD_EXT_SZ:  # 20B v2 extended
            raw = struct.unpack(CARD_EXT_FMT, data[:CARD_EXT_SZ])
            id_, ct, ent, pat, loc, stab, nl, nr, hv = raw
        elif n >= CARD_PACK_SZ:  # 12B v2 packed
            raw = struct.unpack(CARD_PACK_FMT, data[:CARD_PACK_SZ])
            id_, ct, ent, pat, loc, stab, nl, nr = raw
            hv = 0
        elif n >= CARD_V1_EXT_SZ:  # 18B v1 extended
            raw = struct.unpack(CARD_V1_EXT_FMT, data[:CARD_V1_EXT_SZ])
            id_, ct, ent, pat, nl, nr, hv = raw
            loc, stab = _LEGACY_LOCALITY, _LEGACY_STABILITY
        else:  # 10B v1 packed
            raw = struct.unpack(CARD_V1_FMT, data[:CARD_V1_SZ])
            id_, ct, ent, pat, nl, nr = raw
            loc, stab, hv = _LEGACY_LOCALITY, _LEGACY_STABILITY, 0
        nl = None if nl == NO_NEIGHBOR else nl
        nr = None if nr == NO_NEIGHBOR else nr
        return cls(id=id_, card_type=ct, entropy=ent, pattern=pat,
                   locality=loc, stability=stab,
                   neighbor=(nl, nr), hash_val=hv)

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "card_type": self.card_type,
            "type_name": self.type_name,
            "entropy": self.entropy,
            "pattern": f"0x{self.pattern:04X}",
            "pattern_bits": f"{self.pattern:016b}",
            "locality": self.locality,
            "stability": self.stability,
            "neighbor": list(self.neighbor),
            "hash_val": f"0x{self.hash_val:016X}" if self.hash_val else None,
            "packed_bytes": len(self.to_bytes()),
        }

    def __repr__(self) -> str:
        return (f"ZoneCard(id={self.id}, {self.type_name}, "
                f"ent={self.entropy} loc={self.locality} "
                f"st={self.stability} pat=0x{self.pattern:04X})")


# ── Signature computation helpers ──────────────────────────────

def _compute_hash(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h *= 0x100000001B3
        h &= 0xFFFFFFFFFFFFFFFF
    return h


def _compute_entropy(data: bytes) -> int:
    if not data:
        return 0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    ent = 0.0
    for c in counts:
        if c:
            p = c / n
            ent -= p * math.log2(p)
    scaled = int((ent / 8.0) * 255)
    return min(255, max(0, scaled))


def _compute_sparsity(arr: np.ndarray, threshold: float = 0.01) -> float:
    if arr.size == 0:
        return 0.0
    std = float(arr.std())
    if std == 0:
        return 1.0 if float(arr.flat[0]) == 0.0 else 0.0
    return float((np.abs(arr) < threshold * std).mean())


def _compute_pattern_id(arr: np.ndarray) -> int:
    if arr.size == 0:
        return 0
    code = 0
    flat = arr.ravel()
    if _compute_sparsity(arr) > 0.5:
        code |= 0x01
    if _compute_entropy(arr.tobytes()) < 100:
        code |= 0x02
    if arr.ndim >= 2 and arr.shape[0] > 1:
        if float(arr.mean(axis=1).std()) < 0.1:
            code |= 0x04
    alt = np.sum(flat[1:] * flat[:-1] < 0)
    if alt > flat.shape[0] * 0.3:
        code |= 0x08
    skew = float(np.abs(np.mean((flat - flat.mean())**3))) / max(float(flat.std()**3), 1e-10)
    if skew > 2.0:
        code |= 0x10
    kurt = float(np.mean((flat - flat.mean())**4)) / max(float(flat.std()**4), 1e-10)
    if kurt > 0 and kurt < 2.0:
        code |= 0x20
    h = _compute_hash(arr.tobytes())
    code |= ((h & 0xFF) << 8)
    return code & 0xFFFF


def _card_type(arr: np.ndarray) -> int:
    if arr.size > 0 and float(arr.std()) < 1e-8:
        return CARD_SPARSE if bool(np.all(arr == 0)) else CARD_BATCH
    sparsity = _compute_sparsity(arr, threshold=0.05)
    if sparsity > 0.7:
        return CARD_SPARSE
    if arr.ndim >= 2 and arr.shape[0] >= 4:
        ent = _compute_entropy(arr.tobytes())
        row_vars = arr.var(axis=1)
        cv = float(row_vars.std()) / max(float(row_vars.mean()), 1e-10)
        flat = arr.ravel()
        kurt = float(np.mean((flat - flat.mean())**4)) / max(float(flat.std()**4), 1e-10)
        if cv < 0.3 and ent < 100 and kurt < 5.0:
            return CARD_BATCH
    return CARD_LZ


# ── New: locality & stability computation ──────────────────────

def _compute_stability(arr: np.ndarray) -> int:
    """
    Row-to-row predictability (0-255).
    255 = perfectly stable (all rows identical)
    0   = chaotic (rows completely unrelated)
    """
    if arr.size == 0 or arr.ndim < 2 or arr.shape[0] < 2:
        return 128  # neutral default for 1D data
    row_diffs = np.abs(np.diff(arr, axis=0)).mean()
    magnitude = float(np.abs(arr).mean())
    if magnitude < 1e-10:
        return 255  # all zeros = perfectly stable
    # Normalize diff relative to magnitude
    normalized = min(1.0, row_diffs / max(magnitude, 1e-10))
    score = int((1.0 - normalized) * 255)
    return min(255, max(0, score))


def _compute_locality(arr: np.ndarray, neighbor_arr: Optional[np.ndarray] = None) -> int:
    """
    Similarity to neighbor zones (0-255).
    255 = identical to neighbor
    128 = neutral (no neighbor to compare)
    0   = completely different

    Compares: type, entropy, pattern bits, hash.
    """
    if neighbor_arr is None:
        return 128  # no neighbor → neutral
    # Compare multiple dimensions
    scores = []
    # Type match
    t_self = _card_type(arr)
    t_nbr = _card_type(neighbor_arr)
    scores.append(255 if t_self == t_nbr else 0)
    # Entropy similarity
    e_self = _compute_entropy(arr.tobytes())
    e_nbr = _compute_entropy(neighbor_arr.tobytes())
    e_diff = min(255, abs(e_self - e_nbr))
    scores.append(255 - e_diff)
    # Hash similarity (upper byte)
    h_self = (_compute_hash(arr.tobytes()) >> 56) & 0xFF
    h_nbr = (_compute_hash(neighbor_arr.tobytes()) >> 56) & 0xFF
    h_diff = min(255, abs(h_self - h_nbr) * 4)
    scores.append(255 - h_diff)
    avg = int(np.mean(scores))
    return min(255, max(0, avg))


def _compute_locality_from_card(self_card: 'ZoneCard',
                                 neighbor_cards: list['ZoneCard']) -> int:
    """
    Compute locality from already-built cards (no raw data needed).
    Uses card metadata only — pure card-to-card comparison.
    """
    if not neighbor_cards:
        return 128
    scores = []
    for nc in neighbor_cards:
        s = 0
        # Type match (weight: 40%)
        s += 102 if self_card.card_type == nc.card_type else 0
        # Entropy similarity (weight: 30%)
        e_diff = abs(self_card.entropy - nc.entropy)
        s += max(0, 76 - e_diff // 3)
        # Hash upper byte (weight: 30%)
        h_diff = ((self_card.hash_val >> 56) & 0xFF) - ((nc.hash_val >> 56) & 0xFF)
        s += max(0, 76 - abs(h_diff))
        scores.append(s)
    avg = int(np.mean(scores)) if scores else 128
    return min(255, max(0, avg))


# ── Main API ───────────────────────────────────────────────────

def make_card(data: bytes, zone_id: int = 0,
              neighbor: tuple = (None, None),
              neighbor_data: Optional[bytes] = None) -> ZoneCard:
    """Create ZoneCard from raw binary data."""
    arr = np.frombuffer(data, dtype=np.float32)
    ct = _card_type(arr)
    ent = _compute_entropy(data)
    pat = _compute_pattern_id(arr)
    h = _compute_hash(data)
    stability = _compute_stability(arr)
    neigh_arr = np.frombuffer(neighbor_data, dtype=np.float32) if neighbor_data else None
    locality = _compute_locality(arr, neigh_arr)
    return ZoneCard(
        id=zone_id, card_type=ct, entropy=ent, pattern=pat,
        locality=locality, stability=stability,
        neighbor=neighbor, hash_val=h,
    )


def make_card_from_np(arr: np.ndarray, zone_id: int = 0,
                      neighbor: tuple = (None, None),
                      neighbor_arr: Optional[np.ndarray] = None) -> ZoneCard:
    """Create ZoneCard from numpy array."""
    ct = _card_type(arr)
    ent = _compute_entropy(arr.tobytes())
    pat = _compute_pattern_id(arr)
    h = _compute_hash(arr.tobytes())
    stability = _compute_stability(arr)
    locality = _compute_locality(arr, neighbor_arr)
    return ZoneCard(
        id=zone_id, card_type=ct, entropy=ent, pattern=pat,
        locality=locality, stability=stability,
        neighbor=neighbor, hash_val=h,
    )


def card_from_store(store, zone_id: int, shape: str, ns: str = None,
                    neighbor: tuple = (None, None),
                    neighbor_zones: list[int] = None) -> Optional[ZoneCard]:
    """
    Fetch geometry from GeometryStore and return ZoneCard.
    If neighbor_zones provided, also fetches their data for locality.
    """
    weights = store.query(zone_id, shape, ns=ns)
    if weights is None:
        return None
    neigh_arr = None
    if neighbor_zones:
        for nz in neighbor_zones:
            # Look up neighbor in the same store (try all shapes)
            for ns_ in ['I', 'O', 'T', 'S', 'Z', 'L']:
                nw = store.query(nz, ns_)
                if nw is not None:
                    neigh_arr = nw
                    break
            if neigh_arr is not None:
                break
    return make_card_from_np(weights, zone_id=zone_id,
                             neighbor=neighbor, neighbor_arr=neigh_arr)


# ── Multi-Card Reasoning: ZoneSequence ─────────────────────────

class ZoneSequence:
    """
    A window of 3-5 consecutive ZoneCards for multi-zone routing.

    LLM sees a short sequence like:
      [z2 batch loc=240 st=250] [z3 sparse loc=30 st=255] [z4 lz loc=180 st=80]
    And can make smarter routing decisions across zones.
    """

    def __init__(self, cards: list[ZoneCard]):
        assert len(cards) >= 2, "Need at least 2 cards for a sequence"
        self.cards = cards

    @property
    def n(self) -> int:
        return len(self.cards)

    def zone_ids(self) -> list[int]:
        return [c.id for c in self.cards]

    def type_sequence(self) -> list[str]:
        return [c.type_name for c in self.cards]

    def type_pattern(self) -> str:
        """e.g. 'batch→sparse→lz'"""
        return "→".join(self.type_sequence())

    def entropy_sequence(self) -> list[int]:
        return [c.entropy for c in self.cards]

    def locality_sequence(self) -> list[int]:
        return [c.locality for c in self.cards]

    def stability_sequence(self) -> list[int]:
        return [c.stability for c in self.cards]

    def entropy_trend(self) -> str:
        """up / down / flat / mixed"""
        seq = self.entropy_sequence()
        if len(seq) < 2:
            return "flat"
        diffs = [seq[i+1] - seq[i] for i in range(len(seq)-1)]
        pos = sum(1 for d in diffs if d > 10)
        neg = sum(1 for d in diffs if d < -10)
        if pos == len(diffs):
            return "up"
        if neg == len(diffs):
            return "down"
        if pos + neg == 0:
            return "flat"
        return "mixed"

    def stability_trend(self) -> str:
        """increasing / decreasing / stable / erratic"""
        if self.n < 2:
            return "stable"
        st = self.stability_sequence()
        diffs = [st[i+1] - st[i] for i in range(self.n-1)]
        if all(d > 10 for d in diffs):
            return "increasing"
        if all(d < -10 for d in diffs):
            return "decreasing"
        if max(abs(d) for d in diffs) < 20:
            return "stable"
        return "erratic"

    def locality_bonds(self) -> list[int]:
        """Connectivity between adjacent zones: 0=disconnected, 255=strong bond."""
        bonds = []
        for i in range(self.n - 1):
            a, b = self.cards[i], self.cards[i+1]
            if a.card_type == b.card_type:
                bonds.append(min(255, a.locality + (255 - abs(a.entropy - b.entropy))))
            else:
                bonds.append(max(0, a.locality - abs(a.entropy - b.entropy) * 2))
        return bonds

    def suggest_routes(self) -> list[dict]:
        """
        Suggest per-zone routes optimized for the sequence context.
        Unlike single-card routing, sequence-aware routing detects
        patterns like batch→batch→batch (all same path) or
        sparse→batch→lz (different paths per zone).
        """
        type_seq = self.type_sequence()
        ent_seq = self.entropy_sequence()
        st_seq = self.stability_sequence()
        bonds = self.locality_bonds()

        # Detect skip opportunities
        # If a zone is sandwiced between two high-stability zones
        # of the same type → can skip/dedup
        skip_zones = set()
        for i in range(1, self.n - 1):
            if (self.cards[i-1].card_type == self.cards[i+1].card_type
                    and self.cards[i].card_type == CARD_SPARSE
                    and self.cards[i].entropy < 50):
                skip_zones.add(i)

        # Detect batch runs (consecutive batch zones)
        batch_run = False
        run_len = 0
        for i in range(self.n):
            if type_seq[i] == "batch" and st_seq[i] > 150:
                batch_run = True
                run_len += 1

        routes = []
        for i, c in enumerate(self.cards):
            base = self._single_route(c)
            # Adjust based on sequence context
            if i in skip_zones:
                base["decision"] = "skip"
                base["reason"] = f"sparse zone between {type_seq[i-1]} zones — skip"
            elif batch_run and run_len >= 3 and type_seq[i] == "batch":
                base["decision"] = "batch-chain"
                base["reason"] = f"part of batch run ({run_len} consecutive)"
            elif i > 0 and bonds[i-1] > 200 and type_seq[i-1] == type_seq[i]:
                base["decision"] = f"merge-{type_seq[i]}"
                base["reason"] = f"strong bond ({bonds[i-1]}) with z{self.cards[i-1].id} — merge route"
            routes.append({
                "zone": c.id,
                "card_type": c.type_name,
                "locality": c.locality,
                "stability": c.stability,
                "decision": base["decision"],
                "reason": base["reason"],
            })
        return routes

    @staticmethod
    def _single_route(card: ZoneCard) -> dict:
        ct, ent = card.card_type, card.entropy
        if ct == 0 and ent < 80:
            return {"decision": "skip", "reason": "sparse empty"}
        if ct == 0:
            return {"decision": "sparse", "reason": "sparse weights"}
        if ct == 1 and ent < 50:
            return {"decision": "batch", "reason": "uniform fast"}
        if ct == 1:
            return {"decision": "batch-v", "reason": "batch verify"}
        if ct == 2 and ent > 200:
            return {"decision": "full", "reason": "high entropy LZ"}
        if ct == 2:
            return {"decision": "lz", "reason": "LZ decompress"}
        return {"decision": "fallback", "reason": "unclassified"}

    def to_summary(self) -> dict:
        return {
            "n_zones": self.n,
            "zones": self.zone_ids(),
            "type_sequence": self.type_sequence(),
            "type_pattern": self.type_pattern(),
            "entropy_sequence": self.entropy_sequence(),
            "stability_sequence": self.stability_sequence(),
            "locality_bonds": self.locality_bonds(),
            "entropy_trend": self.entropy_trend(),
            "stability_trend": self.stability_trend(),
        }

    def llm_prompt_block(self) -> str:
        """Formatted text block suitable for LLM context."""
        lines = [f"Seq ({self.n} zones):"]
        for i, c in enumerate(self.cards):
            nl = c.neighbor
            nbr_str = f"nbr=[{nl[0]},{nl[1]}]" if nl[0] is not None else ""
            lines.append(
                f"  [{i}] z{c.id:2d} {c.type_name:6s} "
                f"ent={c.entropy:3d} loc={c.locality:3d} "
                f"st={c.stability:3d} {nbr_str}"
            )
        lines.append(f"  pattern: {self.type_pattern()}")
        lines.append(f"  entropy: {self.entropy_trend()}, "
                     f"stability: {self.stability_trend()}")
        return "\n".join(lines)


# ── Helpers for registry → sequence ────────────────────────────

def build_sequences_from_registry(registry_path: str,
                                  window: int = 3) -> list[ZoneSequence]:
    """
    Build ZoneSequences (sliding window) from registry.
    window=3 means each sequence has 3 consecutive zones.
    """
    cards = cards_from_registry(registry_path)
    if len(cards) < window:
        return []
    # Sort by zone id
    cards_sorted = sorted(cards, key=lambda c: c.id)
    # Compute locality for each card based on card-card comparison
    for i, c in enumerate(cards_sorted):
        neighbor_cards = []
        if i > 0:
            neighbor_cards.append(cards_sorted[i-1])
        if i < len(cards_sorted) - 1:
            neighbor_cards.append(cards_sorted[i+1])
        c.locality = _compute_locality_from_card(c, neighbor_cards)

    sequences = []
    for i in range(len(cards_sorted) - window + 1):
        seq_cards = cards_sorted[i:i+window]
        sequences.append(ZoneSequence(seq_cards))
    return sequences


# ── Registry & store functions ─────────────────────────────────

def _read_gsindex(store_path: str) -> Optional[list[dict]]:
    from geometry_store import INDEX_MAGIC
    idx_path = Path(str(store_path) + ".gsidx")
    if not idx_path.exists():
        return None
    with open(idx_path, "rb") as f:
        magic = f.read(8)
    if magic == INDEX_MAGIC:
        from geometry_store import GeometryStore
        try:
            store = GeometryStore(store_path, read_only=True)
            from geometry_store import SHAPES
            entries = []
            for (z, si), (off, n_rows, n_cols) in store._index.items():
                entries.append({"zone": z, "shape": SHAPES[si],
                                "offset": off, "n_rows": n_rows, "n_cols": n_cols})
            store.close()
            return entries
        except Exception:
            return None
    try:
        with open(idx_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _gsindex_for_model(raw_registry: dict, model_key: str) -> Optional[list[dict]]:
    spec = raw_registry["models"].get(model_key, {})
    sp = spec.get("store_path")
    if not sp:
        return None
    entries = _read_gsindex(sp)
    if entries is None:
        return None
    coords = [c for c in raw_registry.get("coords", [])
              if c["model_key"] == model_key]
    if not coords:
        return entries
    coord_map = {int(c["zone"]): str(c["shape"]) for c in coords}
    result = []
    for e in entries:
        z = int(e.get("zone", e.get("face", 0)))
        if z in coord_map:
            e["zone"] = z
            e["shape"] = coord_map[z]
            result.append(e)
    return result


def cards_from_registry(registry_path: str) -> list[ZoneCard]:
    """Build ZoneCards for all entries in a coord registry."""
    import json
    with open(registry_path) as f:
        raw = json.load(f)

    model_groups: dict[str, list[dict]] = {}
    for item in raw.get("coords", []):
        model_groups.setdefault(item["model_key"], []).append(item)

    all_cards: list[ZoneCard] = []
    for mk, items in model_groups.items():
        spec = raw["models"].get(mk, {})
        sp = spec.get("store_path")
        if sp:
            try:
                from geometry_store import GeometryStore
                store = GeometryStore(sp, read_only=True)
                same_zones = sorted(it["zone"] for it in items)
                for item in items:
                    z = int(item["zone"])
                    s = str(item["shape"])
                    ns = item.get("ns") or None
                    idx = same_zones.index(z)
                    left = same_zones[idx - 1] if idx > 0 else None
                    right = same_zones[idx + 1] if idx < len(same_zones) - 1 else None
                    card = card_from_store(store, z, s, ns=ns,
                                           neighbor=(left, right))
                    if card is not None:
                        all_cards.append(card)
                store.close()
                continue
            except Exception:
                pass
        td = spec.get("tensor_dir")
        if td:
            td_path = Path(td)
            if td_path.is_dir():
                entries = _gsindex_for_model(raw, mk)
                if entries:
                    same_zones = sorted(it["zone"] for it in items)
                    for item in items:
                        z = int(item["zone"])
                        idx = same_zones.index(z)
                        left = same_zones[idx - 1] if idx > 0 else None
                        right = same_zones[idx + 1] if idx < len(same_zones) - 1 else None
                        for e in entries:
                            ez = int(e.get("zone", e.get("face", 0)))
                            if ez == z:
                                fname = e.get("path", "")
                                if fname:
                                    fpath = Path(fname)
                                    if not fpath.is_absolute():
                                        fpath = td_path / fpath.name
                                    if fpath.exists():
                                        data = fpath.read_bytes()
                                        card = make_card(data, zone_id=z,
                                                         neighbor=(left, right))
                                        all_cards.append(card)
                                break
                    continue
        for item in items:
            all_cards.append(ZoneCard(
                id=int(item["zone"]), card_type=CARD_BATCH,
                entropy=128, pattern=0, neighbor=(None, None)))

    # Compute locality from card-card comparison
    sorted_cards = sorted(all_cards, key=lambda c: c.id)
    for i, c in enumerate(sorted_cards):
        neighbors = []
        if i > 0:
            neighbors.append(sorted_cards[i-1])
        if i < len(sorted_cards) - 1:
            neighbors.append(sorted_cards[i+1])
        c.locality = _compute_locality_from_card(c, neighbors)

    return sorted_cards
