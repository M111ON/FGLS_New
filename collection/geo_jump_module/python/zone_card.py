"""
zone_card.py — ZoneCard v3
===========================================================
v1:  10B  id/type/entropy/pattern/neighbor
v2:  12B  + locality/stability  (+ 20B extended with hash)
v3:  32B  + zone_id/confidence/flags/parent_id/ttl/hash_val

BACKWARD COMPAT: from_bytes() auto-detects v1/v2/v3.
===========================================================
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import Optional

import numpy as np

# ── Card types ─────────────────────────────────────────────────
CARD_SPARSE = 0
CARD_BATCH  = 1
CARD_LZ     = 2
CARD_NAMES  = {CARD_SPARSE: "sparse", CARD_BATCH: "batch", CARD_LZ: "lz"}

# ── Flags (mirrors zone_card.h) ────────────────────────────────
FLAG_EXPIRED   = 1 << 0   # ttl elapsed
FLAG_HOT_PATH  = 1 << 1   # pentagon axis node
FLAG_GPU_HINT  = 1 << 2   # prefer GPU dispatch
FLAG_CHAINED   = 1 << 3   # part of session chain
FLAG_GLOBE_B   = 1 << 4   # routed via Globe B
# bits 5-7 reserved

# ── Sentinels ──────────────────────────────────────────────────
NO_NEIGHBOR = 0xFFFF
NO_PARENT   = 0xFFFF
NO_ZONE     = 0xFFFF

# ── Task bits (mirrors zone_card.h TASK_*) ─────────────────────
TASK_CODE      = 1 << 0
TASK_MATH      = 1 << 1
TASK_CHAT      = 1 << 2
TASK_REASON    = 1 << 3
TASK_VISION    = 1 << 4
TASK_MULTILANG = 1 << 5
TASK_SUMMARIZE = 1 << 6
TASK_EMBED     = 1 << 7
# bits 8-15 user-defined

TASK_NAMES = {
    TASK_CODE: "code", TASK_MATH: "math", TASK_CHAT: "chat",
    TASK_REASON: "reason", TASK_VISION: "vision",
    TASK_MULTILANG: "multilang", TASK_SUMMARIZE: "summarize",
    TASK_EMBED: "embed",
}

# ── Meta type IDs ──────────────────────────────────────────────
META_TASK      = 0x01
META_TEMPLATE  = 0x02
META_CHAIN     = 0x03
META_CONDITION = 0x04
META_CUSTOM    = 0xFF

_META_HDR_FMT = '<BBH'   # version(1) type_id(1) payload_len(2) = 4 bytes
_META_HDR_SZ  = struct.calcsize(_META_HDR_FMT)  # 4

# ── Condition bytecode ops ─────────────────────────────────────
MCOND_IF_TASK_BIT = 0x01
MCOND_IF_ENTROPY  = 0x02
MCOND_IF_CONF     = 0x03
MCOND_ELSE        = 0x04
MCOND_END         = 0xFF


@dataclass
class ZoneCardMeta:
    """
    Extensible metadata payload pointed to by ZoneCard.ext_ptr.

    Ethereum analogy: ZoneCard = tx header, ZoneCardMeta = calldata.
    Router reads card (32B) O(1); follows ext_ptr only when needed.
    """
    type_id:  int
    payload:  bytes = b""

    # ── convenience constructors ───────────────────────────────

    @staticmethod
    def task(task_bits: int, model_hint: int = 0, priority: int = 128) -> "ZoneCardMeta":
        payload = struct.pack('<HBB', task_bits & 0xFFFF,
                              model_hint & 0xFF, priority & 0xFF)
        return ZoneCardMeta(META_TASK, payload)

    @staticmethod
    def template(template_id: int, lang_hint: int = 0) -> "ZoneCardMeta":
        payload = struct.pack('<HBB', template_id & 0xFFFF, lang_hint & 0xFF, 0)
        return ZoneCardMeta(META_TEMPLATE, payload)

    @staticmethod
    def custom(data: bytes) -> "ZoneCardMeta":
        return ZoneCardMeta(META_CUSTOM, data)

    @staticmethod
    def condition(ops: list[tuple[int, int]]) -> "ZoneCardMeta":
        """ops = [(op, operand), ...] — auto-appends MCOND_END."""
        raw = b"".join(struct.pack('BB', op, operand) for op, operand in ops)
        raw += struct.pack('BB', MCOND_END, 0)
        return ZoneCardMeta(META_CONDITION, raw)

    # ── serialise ─────────────────────────────────────────────
    def to_bytes(self) -> bytes:
        hdr = struct.pack(_META_HDR_FMT, 0x01, self.type_id, len(self.payload))
        return hdr + self.payload

    @classmethod
    def from_bytes(cls, data: bytes) -> "ZoneCardMeta":
        if len(data) < _META_HDR_SZ:
            raise ValueError("meta block too short")
        _ver, type_id, plen = struct.unpack_from(_META_HDR_FMT, data)
        payload = data[_META_HDR_SZ: _META_HDR_SZ + plen]
        return cls(type_id, payload)

    # ── decode convenience ─────────────────────────────────────
    def decode_task(self) -> dict:
        if self.type_id != META_TASK or len(self.payload) < 4:
            return {}
        task_bits, model_hint, priority = struct.unpack_from('<HBB', self.payload)
        names = [n for bit, n in TASK_NAMES.items() if task_bits & bit]
        return {"task_bits": task_bits, "tasks": names,
                "model_hint": model_hint, "priority": priority}

    def decode_template(self) -> dict:
        if self.type_id != META_TEMPLATE or len(self.payload) < 4:
            return {}
        tid, lang, _ = struct.unpack_from('<HBB', self.payload)
        return {"template_id": tid, "lang_hint": lang}

    def decode_condition(self) -> list[dict]:
        if self.type_id != META_CONDITION:
            return []
        ops = []
        op_names = {MCOND_IF_TASK_BIT: "IF_TASK_BIT", MCOND_IF_ENTROPY: "IF_ENTROPY",
                    MCOND_IF_CONF: "IF_CONF", MCOND_ELSE: "ELSE", MCOND_END: "END"}
        for i in range(0, len(self.payload) - 1, 2):
            op, operand = self.payload[i], self.payload[i+1]
            ops.append({"op": op_names.get(op, f"0x{op:02X}"), "operand": operand})
            if op == MCOND_END:
                break
        return ops

    def to_dict(self) -> dict:
        base = {"type_id": self.type_id, "payload_bytes": len(self.payload)}
        if self.type_id == META_TASK:        base["decoded"] = self.decode_task()
        elif self.type_id == META_TEMPLATE:  base["decoded"] = self.decode_template()
        elif self.type_id == META_CONDITION: base["decoded"] = self.decode_condition()
        elif self.type_id == META_CUSTOM:
            base["decoded"] = {"raw": self.payload.decode(errors="replace")}
        return base

    def __repr__(self) -> str:
        type_name = {META_TASK:"task", META_TEMPLATE:"template",
                     META_CHAIN:"chain", META_CONDITION:"condition",
                     META_CUSTOM:"custom"}.get(self.type_id, f"0x{self.type_id:02X}")
        return f"ZoneCardMeta({type_name}, {len(self.payload)}B payload)"

# ── Struct formats ─────────────────────────────────────────────
#
# v2 packed  (12B)  '<HBBHBBHH'
# v2 ext     (24B)  '<HBBHBBHHQ'    ← actual size with alignment
# v3         (32B)  '<HBBHBBHH HBBHHxxxxQ'
#                                    ^padding 4B before hash
#
# The C struct ZoneCardV3 has 4 bytes of implicit padding before
# hash_val (uint64) to align it to 8 bytes.
# Padding bytes are accounted for with '4x' in the format.
#
_V2_FMT    = '<HBBHBBHH'           # 12 bytes
_V2X_FMT   = '<HBBHBBHHQ'         # 20 bytes raw; actual C struct is 24B
# v3: ZoneCardV3 layout (32 bytes, matches C struct with ext_ptr replacing padding)
# '<HBBHBBHH HBBHHIxxxxQ'  — but padding is now ext_ptr (uint32)
# actual: id(2) type(1) ent(1) pat(2) loc(1) stab(1) nl(2) nr(2)
#         zone_id(2) conf(1) flags(1) parent(2) ttl(2) ext_ptr(4) hash(8)
_V3_FMT    = '<HBBHBBHHHBBHHIQ'  # 32 bytes

_V2_SZ     = struct.calcsize(_V2_FMT)    # 12
_V2X_SZ    = struct.calcsize(_V2X_FMT)   # 20 (Python packs without padding)
_V3_SZ     = struct.calcsize(_V3_FMT)    # 32

_V3_STREAM_SZ = 1 + _V3_SZ             # 33 (version byte + struct)

# v1 legacy
_V1_FMT    = '<HBBHHH'
_V1X_FMT   = '<HBBHHHQ'
_V1_SZ     = struct.calcsize(_V1_FMT)
_V1X_SZ    = struct.calcsize(_V1X_FMT)

_VERSION_1 = 0x01
_VERSION_2 = 0x02
_VERSION_3 = 0x03

_LEGACY_LOCALITY  = 128
_LEGACY_STABILITY = 128


# ══════════════════════════════════════════════════════════════
# Dataclass
# ══════════════════════════════════════════════════════════════

@dataclass
class ZoneCard:
    """Compact geometry + routing signature."""

    # ── v2 core ───────────────────────────────────────────────
    id:          int
    card_type:   int
    entropy:     int
    pattern:     int
    locality:    int       = 128
    stability:   int       = 128
    neighbor:    tuple     = (None, None)

    # ── v3 routing extension ──────────────────────────────────
    zone_id:     int       = NO_ZONE     # geo address 0-20735
    confidence:  int       = 128         # router confidence 0-255
    flags:       int       = 0           # FLAG_* bitmask
    parent_id:   int       = NO_PARENT   # session chain
    ttl:         int       = 0           # 0 = never expires
    ext_ptr:     int       = 0           # pointer to ZoneCardMeta (0=none)
    hash_val:    int       = 0           # FNV-1a 64-bit

    def __post_init__(self):
        assert 0 <= self.id < 65536
        assert self.card_type in (0, 1, 2), f"bad card_type: {self.card_type}"
        assert 0 <= self.entropy  <= 255
        assert 0 <= self.pattern  < 65536
        assert 0 <= self.locality <= 255
        assert 0 <= self.stability <= 255
        assert 0 <= self.confidence <= 255
        assert 0 <= self.flags <= 255

    # ── flag helpers ──────────────────────────────────────────
    @property
    def is_expired(self)  -> bool: return bool(self.flags & FLAG_EXPIRED)
    @property
    def is_hot_path(self) -> bool: return bool(self.flags & FLAG_HOT_PATH)
    @property
    def is_gpu_hint(self) -> bool: return bool(self.flags & FLAG_GPU_HINT)
    @property
    def is_chained(self)  -> bool: return bool(self.flags & FLAG_CHAINED)
    @property
    def is_globe_b(self)  -> bool: return bool(self.flags & FLAG_GLOBE_B)

    def set_flag(self, flag: int)   -> None: self.flags |=  flag
    def clear_flag(self, flag: int) -> None: self.flags &= ~flag & 0xFF

    # ── type name ─────────────────────────────────────────────
    @property
    def type_name(self) -> str:
        return CARD_NAMES.get(self.card_type, f"unknown({self.card_type})")

    # ── serialise ─────────────────────────────────────────────
    def to_bytes(self, version: int = 3) -> bytes:
        nl = self.neighbor[0] if self.neighbor[0] is not None else NO_NEIGHBOR
        nr = self.neighbor[1] if self.neighbor[1] is not None else NO_NEIGHBOR
        if version == 3:
            body = struct.pack(
                _V3_FMT,
                self.id, self.card_type, self.entropy, self.pattern,
                self.locality, self.stability, nl, nr,
                self.zone_id, self.confidence, self.flags,
                self.parent_id, self.ttl, self.ext_ptr,
                self.hash_val,
            )
            return bytes([_VERSION_3]) + body
        if version == 2:
            return struct.pack(
                _V2_FMT,
                self.id, self.card_type, self.entropy, self.pattern,
                self.locality, self.stability, nl, nr,
            )
        raise ValueError(f"unsupported version: {version}")

    # ── deserialise (version-aware) ───────────────────────────
    @classmethod
    def from_bytes(cls, data: bytes) -> ZoneCard:
        n = len(data)

        # v3 stream (version byte = 0x03 + 32B body)
        if n >= _V3_STREAM_SZ and data[0] == _VERSION_3:
            (id_, ct, ent, pat, loc, stab, nl, nr,
             zone_id, conf, flags, parent_id, ttl, ext_ptr, hv) = struct.unpack_from(
                _V3_FMT, data, 1)
            nl = None if nl == NO_NEIGHBOR else nl
            nr = None if nr == NO_NEIGHBOR else nr
            return cls(id=id_, card_type=ct, entropy=ent, pattern=pat,
                       locality=loc, stability=stab, neighbor=(nl, nr),
                       zone_id=zone_id, confidence=conf, flags=flags,
                       parent_id=parent_id, ttl=ttl, ext_ptr=ext_ptr, hash_val=hv)

        # v2 extended (20B, no version byte)
        if n >= _V2X_SZ and data[0] not in (_VERSION_1, _VERSION_2, _VERSION_3):
            id_, ct, ent, pat, loc, stab, nl, nr, hv = struct.unpack_from(_V2X_FMT, data)
            nl = None if nl == NO_NEIGHBOR else nl
            nr = None if nr == NO_NEIGHBOR else nr
            card = cls(id=id_, card_type=ct, entropy=ent, pattern=pat,
                       locality=loc, stability=stab, neighbor=(nl, nr), hash_val=hv)
            card.confidence = _calc_confidence(stab, loc, ent)
            return card

        # v2 packed (12B)
        if n >= _V2_SZ and data[0] not in (_VERSION_1, _VERSION_2, _VERSION_3):
            id_, ct, ent, pat, loc, stab, nl, nr = struct.unpack_from(_V2_FMT, data)
            nl = None if nl == NO_NEIGHBOR else nl
            nr = None if nr == NO_NEIGHBOR else nr
            card = cls(id=id_, card_type=ct, entropy=ent, pattern=pat,
                       locality=loc, stability=stab, neighbor=(nl, nr))
            card.confidence = _calc_confidence(stab, loc, ent)
            return card

        # v1 extended
        if n >= _V1X_SZ:
            id_, ct, ent, pat, nl, nr, hv = struct.unpack_from(_V1X_FMT, data)
            nl = None if nl == NO_NEIGHBOR else nl
            nr = None if nr == NO_NEIGHBOR else nr
            return cls(id=id_, card_type=ct, entropy=ent, pattern=pat,
                       locality=_LEGACY_LOCALITY, stability=_LEGACY_STABILITY,
                       neighbor=(nl, nr), hash_val=hv)

        # v1 packed
        if n >= _V1_SZ:
            id_, ct, ent, pat, nl, nr = struct.unpack_from(_V1_FMT, data)
            nl = None if nl == NO_NEIGHBOR else nl
            nr = None if nr == NO_NEIGHBOR else nr
            return cls(id=id_, card_type=ct, entropy=ent, pattern=pat,
                       locality=_LEGACY_LOCALITY, stability=_LEGACY_STABILITY,
                       neighbor=(nl, nr))

        raise ValueError(f"data too short ({n}B) to deserialise ZoneCard")

    # ── dict / repr ───────────────────────────────────────────
    def to_dict(self) -> dict:
        flags_names = []
        if self.is_expired:   flags_names.append("EXPIRED")
        if self.is_hot_path:  flags_names.append("HOT_PATH")
        if self.is_gpu_hint:  flags_names.append("GPU_HINT")
        if self.is_chained:   flags_names.append("CHAINED")
        if self.is_globe_b:   flags_names.append("GLOBE_B")
        return {
            "id":          self.id,
            "card_type":   self.card_type,
            "type_name":   self.type_name,
            "entropy":     self.entropy,
            "pattern":     f"0x{self.pattern:04X}",
            "pattern_bits":f"{self.pattern:016b}",
            "locality":    self.locality,
            "stability":   self.stability,
            "neighbor":    list(self.neighbor),
            "zone_id":     None if self.zone_id == NO_ZONE else self.zone_id,
            "confidence":  self.confidence,
            "flags":       f"0x{self.flags:02X}",
            "flags_names": flags_names,
            "parent_id":   None if self.parent_id == NO_PARENT else self.parent_id,
            "ttl":         self.ttl,
            "ext_ptr":     f"0x{self.ext_ptr:08X}" if self.ext_ptr else None,
            "has_meta":    self.ext_ptr != 0,
            "hash_val":    f"0x{self.hash_val:016X}" if self.hash_val else None,
            "packed_bytes":len(self.to_bytes()),
        }

    def __repr__(self) -> str:
        zone_str = f" geo={self.zone_id}" if self.zone_id != NO_ZONE else ""
        hot_str  = " HOT" if self.is_hot_path else ""
        return (f"ZoneCard(id={self.id}{zone_str}, {self.type_name},"
                f" ent={self.entropy} loc={self.locality}"
                f" st={self.stability} conf={self.confidence}"
                f" pat=0x{self.pattern:04X}{hot_str})")


# ══════════════════════════════════════════════════════════════
# Internal helpers
# ══════════════════════════════════════════════════════════════

def _calc_confidence(stability: int, locality: int, entropy: int) -> int:
    """stability×0.4 + locality×0.4 + (255-entropy)×0.2"""
    score = int(stability * 0.4 + locality * 0.4 + (255 - entropy) * 0.2)
    return min(255, max(0, score))


def _compute_hash(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def _compute_entropy(data: bytes) -> int:
    if not data:
        return 0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    ent = sum(-c/n * math.log2(c/n) for c in counts if c)
    return min(255, max(0, int((ent / 8.0) * 255)))


def _compute_sparsity(arr: np.ndarray, threshold: float = 0.01) -> float:
    if arr.size == 0:
        return 0.0
    std = float(arr.std())
    if std == 0:
        return 1.0 if float(arr.flat[0]) == 0.0 else 0.0
    return float((np.abs(arr) < threshold * std).mean())


def _compute_stability(arr: np.ndarray) -> int:
    if arr.size == 0 or arr.ndim < 2 or arr.shape[0] < 2:
        return 128
    row_diffs = float(np.abs(np.diff(arr, axis=0)).mean())
    magnitude = float(np.abs(arr).mean())
    if magnitude < 1e-10:
        return 255
    norm = min(1.0, row_diffs / max(magnitude, 1e-10))
    return min(255, max(0, int((1.0 - norm) * 255)))


def _compute_locality(arr: np.ndarray,
                       neighbor_arr: Optional[np.ndarray] = None) -> int:
    if neighbor_arr is None:
        return 128
    scores = [
        255 if _card_type(arr) == _card_type(neighbor_arr) else 0,
        255 - min(255, abs(_compute_entropy(arr.tobytes())
                          - _compute_entropy(neighbor_arr.tobytes()))),
        255 - min(255, abs(int((_compute_hash(arr.tobytes()) >> 56) & 0xFF)
                          - int((_compute_hash(neighbor_arr.tobytes()) >> 56) & 0xFF)) * 4),
    ]
    return min(255, max(0, int(sum(scores) / len(scores))))


def _compute_locality_from_card(self_card: ZoneCard,
                                 neighbor_cards: list[ZoneCard]) -> int:
    if not neighbor_cards:
        return 128
    scores = []
    for nc in neighbor_cards:
        s  = 102 if self_card.card_type == nc.card_type else 0
        s += max(0, 76 - abs(self_card.entropy - nc.entropy) // 3)
        h_diff = abs(int((self_card.hash_val >> 56) & 0xFF)
                    - int((nc.hash_val       >> 56) & 0xFF))
        s += max(0, 76 - h_diff)
        scores.append(s)
    return min(255, max(0, int(sum(scores) / len(scores))))


def _compute_pattern_id(arr: np.ndarray) -> int:
    if arr.size == 0:
        return 0
    flat = arr.ravel()
    code = 0
    if _compute_sparsity(arr) > 0.5:                code |= 0x01
    if _compute_entropy(arr.tobytes()) < 100:        code |= 0x02
    if arr.ndim >= 2 and arr.shape[0] > 1:
        if float(arr.mean(axis=1).std()) < 0.1:     code |= 0x04
    alt = int(np.sum(flat[1:] * flat[:-1] < 0))
    if alt > flat.shape[0] * 0.3:                   code |= 0x08
    std3 = max(float(flat.std()**3), 1e-10)
    skew = float(abs(np.mean((flat - flat.mean())**3))) / std3
    if skew > 2.0:                                   code |= 0x10
    std4 = max(float(flat.std()**4), 1e-10)
    kurt = float(np.mean((flat - flat.mean())**4)) / std4
    if 0 < kurt < 2.0:                               code |= 0x20
    h = _compute_hash(arr.tobytes())
    code |= ((h & 0xFF) << 8)
    return code & 0xFFFF


def _card_type(arr: np.ndarray) -> int:
    if arr.size > 0 and float(arr.std()) < 1e-8:
        return CARD_SPARSE if bool(np.all(arr == 0)) else CARD_BATCH
    if _compute_sparsity(arr, 0.05) > 0.7:
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


# ══════════════════════════════════════════════════════════════
# Public factory functions
# ══════════════════════════════════════════════════════════════

def make_card(data: bytes,
              zone_id: int = NO_ZONE,
              geo_address: int = NO_ZONE,
              neighbor: tuple = (None, None),
              neighbor_data: Optional[bytes] = None,
              parent_id: int = NO_PARENT,
              ttl: int = 0,
              flags: int = 0) -> ZoneCard:
    """Create ZoneCard v3 from raw binary data."""
    arr = np.frombuffer(data, dtype=np.float32)
    return make_card_from_np(arr, zone_id=zone_id, geo_address=geo_address,
                              neighbor=neighbor,
                              neighbor_arr=(np.frombuffer(neighbor_data, dtype=np.float32)
                                            if neighbor_data else None),
                              parent_id=parent_id, ttl=ttl, flags=flags)


def make_card_from_np(arr: np.ndarray,
                      zone_id: int = NO_ZONE,
                      geo_address: int = NO_ZONE,
                      neighbor: tuple = (None, None),
                      neighbor_arr: Optional[np.ndarray] = None,
                      parent_id: int = NO_PARENT,
                      ttl: int = 0,
                      flags: int = 0) -> ZoneCard:
    """Create ZoneCard v3 from numpy array."""
    ct      = _card_type(arr)
    ent     = _compute_entropy(arr.tobytes())
    pat     = _compute_pattern_id(arr)
    hv      = _compute_hash(arr.tobytes())
    stab    = _compute_stability(arr)
    loc     = _compute_locality(arr, neighbor_arr)
    conf    = _calc_confidence(stab, loc, ent)

    # auto-set HOT_PATH flag if geo_address is pentagon axis
    # pentagon anchors: every (geo_address % 864) == 0 (approx)
    if geo_address != NO_ZONE and geo_address % 864 == 0:
        flags |= FLAG_HOT_PATH
    if parent_id != NO_PARENT:
        flags |= FLAG_CHAINED

    return ZoneCard(
        id=zone_id & 0xFFFF, card_type=ct, entropy=ent, pattern=pat,
        locality=loc, stability=stab, neighbor=neighbor,
        zone_id=geo_address, confidence=conf,
        flags=flags, parent_id=parent_id, ttl=ttl, ext_ptr=0, hash_val=hv,
    )


# ══════════════════════════════════════════════════════════════
# ZoneSequence (unchanged API, extended awareness)
# ══════════════════════════════════════════════════════════════

class ZoneSequence:
    """
    Sliding window of ZoneCards for multi-zone routing.
    v3: also exposes confidence trend and flag summary.
    """

    def __init__(self, cards: list[ZoneCard]):
        assert len(cards) >= 2
        self.cards = cards

    @property
    def n(self) -> int:
        return len(self.cards)

    def zone_ids(self)      -> list[int]: return [c.id for c in self.cards]
    def type_sequence(self) -> list[str]: return [c.type_name for c in self.cards]
    def type_pattern(self)  -> str:       return "→".join(self.type_sequence())
    def entropy_sequence(self)    -> list[int]: return [c.entropy    for c in self.cards]
    def locality_sequence(self)   -> list[int]: return [c.locality   for c in self.cards]
    def stability_sequence(self)  -> list[int]: return [c.stability  for c in self.cards]
    def confidence_sequence(self) -> list[int]: return [c.confidence for c in self.cards]

    def _trend(self, seq: list[int], up_thr: int = 10) -> str:
        if len(seq) < 2: return "flat"
        diffs = [seq[i+1] - seq[i] for i in range(len(seq)-1)]
        pos = sum(1 for d in diffs if d >  up_thr)
        neg = sum(1 for d in diffs if d < -up_thr)
        if pos == len(diffs): return "up"
        if neg == len(diffs): return "down"
        if pos + neg == 0:    return "flat"
        return "mixed"

    def entropy_trend(self)    -> str: return self._trend(self.entropy_sequence())
    def stability_trend(self)  -> str: return self._trend(self.stability_sequence())
    def confidence_trend(self) -> str: return self._trend(self.confidence_sequence())

    def hot_zones(self) -> list[int]:
        """Return indices of hot-path cards in this sequence."""
        return [i for i, c in enumerate(self.cards) if c.is_hot_path]

    def locality_bonds(self) -> list[int]:
        bonds = []
        for i in range(self.n - 1):
            a, b = self.cards[i], self.cards[i+1]
            if a.card_type == b.card_type:
                bonds.append(min(255, a.locality + (255 - abs(a.entropy - b.entropy))))
            else:
                bonds.append(max(0, a.locality - abs(a.entropy - b.entropy) * 2))
        return bonds

    def suggest_routes(self) -> list[dict]:
        type_seq = self.type_sequence()
        st_seq   = self.stability_sequence()
        bonds    = self.locality_bonds()
        skip_zones = {
            i for i in range(1, self.n - 1)
            if (self.cards[i-1].card_type == self.cards[i+1].card_type
                and self.cards[i].card_type == CARD_SPARSE
                and self.cards[i].entropy < 50)
        }
        batch_run = sum(1 for i in range(self.n)
                        if type_seq[i] == "batch" and st_seq[i] > 150)

        routes = []
        for i, c in enumerate(self.cards):
            base = self._single_route(c)
            if c.is_hot_path:
                base["decision"] = "hot-path"
                base["reason"]   = "pentagon axis — bypass clock"
            elif c.is_expired:
                base["decision"] = "evict"
                base["reason"]   = "card expired (ttl elapsed)"
            elif i in skip_zones:
                base["decision"] = "skip"
                base["reason"]   = f"sparse zone between {type_seq[i-1]} zones"
            elif batch_run >= 3 and type_seq[i] == "batch":
                base["decision"] = "batch-chain"
                base["reason"]   = f"batch run ({batch_run} consecutive)"
            elif i > 0 and bonds[i-1] > 200 and type_seq[i-1] == type_seq[i]:
                base["decision"] = f"merge-{type_seq[i]}"
                base["reason"]   = f"strong bond ({bonds[i-1]}) with z{self.cards[i-1].id}"
            routes.append({
                "zone":       c.id,
                "card_type":  c.type_name,
                "locality":   c.locality,
                "stability":  c.stability,
                "confidence": c.confidence,
                "flags":      f"0x{c.flags:02X}",
                "decision":   base["decision"],
                "reason":     base["reason"],
            })
        return routes

    @staticmethod
    def _single_route(card: ZoneCard) -> dict:
        ct, ent = card.card_type, card.entropy
        if ct == CARD_SPARSE and ent < 80:
            return {"decision": "skip",    "reason": "sparse empty"}
        if ct == CARD_SPARSE:
            return {"decision": "sparse",  "reason": "sparse weights"}
        if ct == CARD_BATCH and ent < 50:
            return {"decision": "batch",   "reason": "uniform fast"}
        if ct == CARD_BATCH:
            return {"decision": "batch-v", "reason": "batch verify"}
        if ct == CARD_LZ and ent > 200:
            return {"decision": "full",    "reason": "high entropy LZ"}
        if ct == CARD_LZ:
            return {"decision": "lz",      "reason": "LZ decompress"}
        return {"decision": "fallback", "reason": "unclassified"}

    def to_summary(self) -> dict:
        return {
            "n_zones":           self.n,
            "zones":             self.zone_ids(),
            "type_sequence":     self.type_sequence(),
            "type_pattern":      self.type_pattern(),
            "entropy_sequence":  self.entropy_sequence(),
            "stability_sequence":self.stability_sequence(),
            "confidence_sequence":self.confidence_sequence(),
            "locality_bonds":    self.locality_bonds(),
            "entropy_trend":     self.entropy_trend(),
            "stability_trend":   self.stability_trend(),
            "confidence_trend":  self.confidence_trend(),
            "hot_zones":         self.hot_zones(),
        }

    def llm_prompt_block(self) -> str:
        lines = [f"Seq ({self.n} zones):"]
        for i, c in enumerate(self.cards):
            nl = c.neighbor
            nbr = f"nbr=[{nl[0]},{nl[1]}]" if nl[0] is not None else ""
            hot = " ★" if c.is_hot_path else ""
            lines.append(
                f"  [{i}] z{c.id:2d} {c.type_name:6s} "
                f"ent={c.entropy:3d} loc={c.locality:3d} "
                f"st={c.stability:3d} conf={c.confidence:3d}{hot} {nbr}"
            )
        lines.append(f"  pattern:    {self.type_pattern()}")
        lines.append(f"  entropy:    {self.entropy_trend()}, "
                     f"stability: {self.stability_trend()}, "
                     f"confidence: {self.confidence_trend()}")
        hots = self.hot_zones()
        if hots:
            lines.append(f"  hot zones:  {hots} (pentagon axis — bypass clock)")
        return "\n".join(lines)


# ══════════════════════════════════════════════════════════════
# Registry helpers (unchanged signatures)
# ══════════════════════════════════════════════════════════════

def build_sequences_from_registry(registry_path: str,
                                   window: int = 3) -> list[ZoneSequence]:
    cards = cards_from_registry(registry_path)
    if len(cards) < window:
        return []
    cards_sorted = sorted(cards, key=lambda c: c.id)
    for i, c in enumerate(cards_sorted):
        nbrs = []
        if i > 0:             nbrs.append(cards_sorted[i-1])
        if i < len(cards_sorted)-1: nbrs.append(cards_sorted[i+1])
        c.locality = _compute_locality_from_card(c, nbrs)
    return [ZoneSequence(cards_sorted[i:i+window])
            for i in range(len(cards_sorted) - window + 1)]


def card_from_store(store, zone_id: int, shape: str, ns: str = None,
                    neighbor: tuple = (None, None),
                    neighbor_zones: list[int] = None,
                    geo_address: int = NO_ZONE) -> Optional[ZoneCard]:
    weights = store.query(zone_id, shape, ns=ns)
    if weights is None:
        return None
    neigh_arr = None
    if neighbor_zones:
        for nz in neighbor_zones:
            for ns_ in ['I', 'O', 'T', 'S', 'Z', 'L']:
                nw = store.query(nz, ns_)
                if nw is not None:
                    neigh_arr = nw
                    break
            if neigh_arr is not None:
                break
    return make_card_from_np(weights, zone_id=zone_id,
                              geo_address=geo_address,
                              neighbor=neighbor, neighbor_arr=neigh_arr)


def cards_from_registry(registry_path: str) -> list[ZoneCard]:
    import json
    from pathlib import Path
    with open(registry_path) as f:
        raw = json.load(f)

    model_groups: dict[str, list[dict]] = {}
    for item in raw.get("coords", []):
        model_groups.setdefault(item["model_key"], []).append(item)

    all_cards: list[ZoneCard] = []
    for mk, items in model_groups.items():
        spec   = raw["models"].get(mk, {})
        sp     = spec.get("store_path")
        if sp:
            try:
                from geometry_store import GeometryStore
                store = GeometryStore(sp, read_only=True)
                same_zones = sorted(it["zone"] for it in items)
                for item in items:
                    z  = int(item["zone"])
                    s  = str(item["shape"])
                    ns = item.get("ns") or None
                    idx   = same_zones.index(z)
                    left  = same_zones[idx-1] if idx > 0 else None
                    right = same_zones[idx+1] if idx < len(same_zones)-1 else None
                    geo_addr = int(item.get("geo_address", NO_ZONE))
                    card = card_from_store(store, z, s, ns=ns,
                                          neighbor=(left, right),
                                          geo_address=geo_addr)
                    if card is not None:
                        all_cards.append(card)
                store.close()
                continue
            except Exception:
                pass

        for item in items:
            all_cards.append(ZoneCard(
                id=int(item["zone"]), card_type=CARD_BATCH,
                entropy=128, pattern=0, neighbor=(None, None)))

    sorted_cards = sorted(all_cards, key=lambda c: c.id)
    for i, c in enumerate(sorted_cards):
        nbrs = []
        if i > 0:              nbrs.append(sorted_cards[i-1])
        if i < len(sorted_cards)-1: nbrs.append(sorted_cards[i+1])
        c.locality = _compute_locality_from_card(c, nbrs)
    return sorted_cards
