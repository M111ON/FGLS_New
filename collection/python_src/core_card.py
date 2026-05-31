"""
core_card.py — Universal CoreCard schema v1.1
============================================
CoreCard:
  deck, author, card_schema_version    (required)
  entropy, stability, locality         (0-255 stats)
  std, max_abs                         (new: weight distribution)
  fingerprint, version, parent_version
  extensions                           (deck-specific payload)

from_dict merges unknown keys into extensions automatically.
"""
from __future__ import annotations
from dataclasses import dataclass, asdict
from typing import Optional, Dict, Any, Callable


@dataclass
class CoreCard:
    deck: str
    author: str
    card_schema_version: str
    entropy: int = 128
    stability: int = 128
    locality: int = 128
    std: float = 0.0
    max_abs: float = 0.0
    fingerprint: int = 0
    version: int = 0
    parent_version: int = 0
    extensions: Dict[str, Any] = None

    def to_dict(self):
        d = asdict(self)
        if self.extensions:
            d.update(self.extensions)
        return d

    @classmethod
    def from_dict(cls, d: dict):
        core_fields = set(cls.__annotations__.keys())
        core_data = {k: d.get(k) for k in core_fields if k in d}
        ext = {k: v for k, v in d.items() if k not in core_fields}
        if "extensions" not in core_data:
            core_data["extensions"] = ext
        return cls(**core_data)

    def validate(self) -> Optional[str]:
        if self.std < 0:
            return "std must be >= 0"
        for f in ("entropy", "stability", "locality"):
            v = getattr(self, f)
            if not (0 <= v <= 255):
                return f"{f}={v} out of range 0-255"
        if not self.deck:
            return "deck is required"
        if not self.card_schema_version:
            return "card_schema_version is required"
        return None


ADAPTERS: Dict[tuple, Callable] = {}


def register_adapter(src: str, dst: str):
    def decorator(f):
        ADAPTERS[(src, dst)] = f
        return f
    return decorator


def adapt_card(card: CoreCard, target_schema: str) -> Optional[CoreCard]:
    if card.card_schema_version == target_schema:
        return card
    adapter = ADAPTERS.get((card.card_schema_version, target_schema))
    return adapter(card) if adapter else None
