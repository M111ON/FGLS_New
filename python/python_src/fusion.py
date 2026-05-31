"""
fusion.py — Cross-model geometry fusion

Enables sharing of routing decisions and decoded state between models
when their ZoneCards are similar enough.

Architecture:
  CardSimilarity(A, B) → 0..1 score
  FusionTable          → cross-model card pair registry
  FusedRoute           → borrow peer's route decision
  SharedState          → borrow peer's decoded weights
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Optional

# ── Card similarity weights ──
W_SAME_TYPE     = 0.35
W_ENTROPY       = 0.20
W_PATTERN       = 0.20
W_LOCALITY      = 0.15
W_STABILITY     = 0.10

FUSION_THRESHOLD     = 0.75   # min score to share route decisions
STATE_SHARE_THRESH   = 0.90  # min score to share decoded weights


def _pattern_similarity(pa: int, pb: int) -> float:
    """Bitwise Jaccard-style similarity of 16-bit patterns."""
    if pa == 0 and pb == 0:
        return 1.0
    intersection = bin(pa & pb).count("1")
    union = bin(pa | pb).count("1")
    return intersection / max(union, 1)


def card_similarity(card_a, card_b) -> float:
    """
    Compute similarity score between two ZoneCards.
    Returns 0.0 (totally different) to 1.0 (identical).
    """
    score = 0.0

    # Same card_type is a strong signal
    if card_a.card_type == card_b.card_type:
        score += W_SAME_TYPE

    # Entropy proximity
    ent_diff = abs(card_a.entropy - card_b.entropy)
    if ent_diff <= 10:
        score += W_ENTROPY
    elif ent_diff <= 50:
        score += W_ENTROPY * (1 - (ent_diff - 10) / 40)

    # Pattern overlap
    score += W_PATTERN * _pattern_similarity(
        getattr(card_a, 'pattern', 0), getattr(card_b, 'pattern', 0))

    # Locality proximity
    loc_diff = abs(card_a.locality - card_b.locality)
    if loc_diff <= 20:
        score += W_LOCALITY
    elif loc_diff <= 100:
        score += W_LOCALITY * (1 - (loc_diff - 20) / 80)

    # Stability proximity
    st_diff = abs(card_a.stability - card_b.stability)
    if st_diff <= 20:
        score += W_STABILITY
    elif st_diff <= 100:
        score += W_STABILITY * (1 - (st_diff - 20) / 80)

    return min(score, 1.0)


# ── Fusion entry ──

@dataclass
class FusionEntry:
    """One cross-model card fusion record."""
    peer_key: str           # model key of peer
    similarity: float       # 0..1 score
    route_decision: str = ""
    route_action: str = ""
    weights_hash: int = 0   # for exact weight dedup


# ── FusionTable ──

class FusionTable:
    """
    Runtime registry of cross-model card pairs.

    Populated lazily: when card A is resolved for model X, check
    if any peer model has a similar card → add fusion entry.
    Also supports pre-seeding from registry config.
    """

    def __init__(self):
        self._lock = threading.RLock()
        # key: (zone, shape) → { model_key → FusionEntry }
        self._table: dict[tuple, dict[str, FusionEntry]] = {}
        self._stats = {"probes": 0, "hits": 0, "stores": 0}

    def store(self, zone: int, shape: str,
              model_key: str, card,
              peer_key: str = None,
              peer_card=None,
              force: bool = False) -> Optional[FusionEntry]:
        """
        Store or update fusion entry for (zone, shape, model_key).

        If peer_key+peer_card given, computes similarity automatically.
        Returns FusionEntry if stored, None if below threshold.
        """
        key = (zone, shape)
        with self._lock:
            if key not in self._table:
                self._table[key] = {}

            if peer_key is not None and peer_card is not None:
                sim = card_similarity(card, peer_card)
                if sim < FUSION_THRESHOLD and not force:
                    return None
                entry = FusionEntry(peer_key=peer_key, similarity=sim)
                self._table[key][model_key] = entry
                self._stats["stores"] += 1
                return entry

            return None

    def probe(self, zone: int, shape: str,
              model_key: str, card=None) -> Optional[FusionEntry]:
        """
        Check if a peer model has a fused card for this (zone, shape).

        Returns a COPY of the best-match FusionEntry with peer_key set to
        the owner model (the peer of 'model_key'), or None.
        """
        key = (zone, shape)
        with self._lock:
            entries = self._table.get(key, {})
            self._stats["probes"] += 1
            if not entries:
                return None

            best_raw = None
            best_owner = None
            best_sim = 0.0
            for owner_key, entry in entries.items():
                if owner_key == model_key:
                    continue
                if entry.similarity > best_sim:
                    best_sim = entry.similarity
                    best_raw = entry
                    best_owner = owner_key

            if best_raw and best_sim >= FUSION_THRESHOLD:
                self._stats["hits"] += 1
                return FusionEntry(
                    peer_key=best_owner,
                    similarity=best_raw.similarity,
                    route_decision=best_raw.route_decision,
                    route_action=best_raw.route_action,
                    weights_hash=best_raw.weights_hash,
                )
            return None

    def best_peer(self, zone: int, shape: str,
                  model_key: str) -> Optional[tuple[str, float]]:
        """Find best peer model key + similarity for a (zone, shape).
        The peer IS the owner of the best-matching entry."""
        key = (zone, shape)
        with self._lock:
            entries = self._table.get(key, {})
            best_key = None
            best_sim = 0.0
            for owner_key, entry in entries.items():
                if owner_key == model_key:
                    continue
                if entry.similarity > best_sim:
                    best_sim = entry.similarity
                    best_key = owner_key
            if best_key:
                return (best_key, best_sim)
            return None

    def stats(self) -> dict:
        with self._lock:
            n_pairs = sum(len(v) for v in self._table.values())
            return {
                "n_zone_keys": len(self._table),
                "n_pairs": n_pairs,
                "probes": self._stats["probes"],
                "hits": self._stats["hits"],
                "stores": self._stats["stores"],
                "hit_rate": round(
                    self._stats["hits"] / max(self._stats["probes"], 1) * 100, 1),
                "fusion_threshold": FUSION_THRESHOLD,
                "state_share_threshold": STATE_SHARE_THRESH,
            }


# ── Singleton ──

_GLOBAL_FUSION: FusionTable | None = None


def get_fusion() -> FusionTable:
    global _GLOBAL_FUSION
    if _GLOBAL_FUSION is None:
        _GLOBAL_FUSION = FusionTable()
    return _GLOBAL_FUSION


def reset_fusion():
    """Clear singleton (for test isolation)."""
    global _GLOBAL_FUSION
    _GLOBAL_FUSION = None


# ── Fingerprint cache ──

_FINGERPRINT_CACHE: dict[int, dict] = {}  # hash → {"weights": ndarray, "model_key": str}
_FINGERPRINT_LOCK = threading.Lock()


def compute_fingerprint(weights) -> int:
    """Fast xxh64 of float32 weights array. Deterministic, O(n)."""
    import xxhash
    return xxhash.xxh64(weights.tobytes()).intdigest()


def fingerprint_lookup(fingerprint: int) -> Optional[dict]:
    """Check global fingerprint cache for exact weight reuse."""
    with _FINGERPRINT_LOCK:
        return _FINGERPRINT_CACHE.get(fingerprint)


def fingerprint_store(fingerprint: int, weights, model_key: str):
    """Store weights in global fingerprint cache."""
    if fingerprint == 0:
        return
    with _FINGERPRINT_LOCK:
        _FINGERPRINT_CACHE[fingerprint] = {
            "weights": weights,
            "model_key": model_key,
        }


def fingerprint_stats() -> dict:
    with _FINGERPRINT_LOCK:
        return {"n_entries": len(_FINGERPRINT_CACHE)}


# ── Fusion → Rule promotion ──

def promote_to_rule(card, decision: str, action: str,
                    similarity: float = 0.90):
    """
    Promote a fusion borrowing into a native RouteRule.

    Creates a rule matching (card_type, entropy_bucket, stability_bucket)
    so subsequent cards with similar profiles hit it directly —
    no fusion probe needed.
    """
    from route_cache import RouteRule

    ct = card.card_type if hasattr(card, 'card_type') else card.get('card_type', -1)
    ent = card.entropy if hasattr(card, 'entropy') else card.get('entropy', 128)
    st = card.stability if hasattr(card, 'stability') else card.get('stability', 128)

    # Bucket entropy and stability for generalization
    ent_min = max(0, ent - 15)
    ent_max = min(255, ent + 15)
    st_min = max(0, st - 20)
    st_max = min(255, st + 20)

    rule = RouteRule(
        priority=60,  # below single-card rules (61-87), above catch-all (1)
        match_type='single',
        card_type=ct if ct >= 0 else -1,
        entropy_min=ent_min,
        entropy_max=ent_max,
        stability_min=st_min,
        stability_max=st_max,
        decision=decision,
        action=action,
        reason=f"promoted from fusion (sim={similarity:.2f}, "
               f"ct={ct} ent={ent_min}-{ent_max} st={st_min}-{st_max})",
    )

    from route_cache import get_cache
    gc = get_cache()
    gc.add_rule(rule)
    return rule


# ── FusedRoute: cross-model route resolution + promotion ──

def resolve_fused(cache, card, zone: int, shape: str,
                  model_key: str, engine_pool=None) -> dict:
    """
    Resolve route with cross-model fusion fallback + auto-promotion.

    1. Try own cache (rule or exact)
    2. If miss, check fusion table for peer decision
    3. If peer found AND similarity ≥ 0.90 → PROMOTE to native rule
    4. If peer found AND similarity < 0.90 → borrow + hash-cache
    5. Only then fall through to planner/LLM
    """
    from route_cache import get_cache

    r = cache.resolve(card)
    if r["cache"] != "miss":
        return r

    ft = get_fusion()
    entry = ft.probe(zone, shape, model_key, card)
    if entry and entry.route_decision:
        borrowed = {
            "decision": entry.route_decision,
            "action": entry.route_action,
            "reason": f"fused from {entry.peer_key} "
                      f"(sim={entry.similarity:.2f})",
            "cache": "fusion",
        }

        # Cache in exact hash cache
        cache_global = get_cache()
        hv = card.hash_val if hasattr(card, 'hash_val') else card.get('hash_val', 0)
        if hv:
            cache_global.store(card, borrowed)

        # PROMOTE: sim ≥ 0.90 → native RouteRule (no fusion probe next time)
        if entry.similarity >= 0.90:
            promote_to_rule(
                card,
                decision=entry.route_decision,
                action=entry.route_action,
                similarity=entry.similarity,
            )
            borrowed["promoted"] = True
            borrowed["reason"] += " [PROMOTED to rule]"

        return borrowed

    return r


def store_fusion_entry(zone: int, shape: str,
                       model_key: str, card,
                       peer_key: str, peer_card) -> None:
    """Compute and store fusion entry between two model cards."""
    ft = get_fusion()
    ft.store(zone, shape, model_key, card,
             peer_key=peer_key, peer_card=peer_card)
    # Also store reverse
    ft.store(zone, shape, peer_key, peer_card,
             peer_key=model_key, peer_card=card)


# ── SharedState: cross-model weight borrowing (fingerprint-first) ──

def borrow_shared_state(engine_pool, zone: int, shape: str,
                         model_key: str, card,
                         fingerprint: int = 0) -> Optional[dict]:
    """
    Borrow reusable weights from peer engine or global fingerprint cache.

    Priority:
      1. Global fingerprint cache (O(1), exact match)
      2. Peer engine _last_weights + similarity ≥ 0.90
      3. Peer engine store query + similarity ≥ 0.90

    fingerprint: optional pre-computed xxh64 of target weights.
    Returns weights dict with source info, or None.
    """
    # Step 1: Global fingerprint cache (O(1) exact match)
    if fingerprint:
        cached = fingerprint_lookup(fingerprint)
        if cached:
            return {
                "weights": cached["weights"],
                "source": f"fingerprint:{fingerprint:x} ({cached['model_key']})",
                "similarity": 1.0,
                "zone": zone,
                "method": "fingerprint",
            }

    # Step 2: Try similarity-based borrowing from peer
    ft = get_fusion()
    best = ft.best_peer(zone, shape, model_key)
    if best is None:
        return None
    peer_key, sim = best
    if sim < STATE_SHARE_THRESH:
        return None

    try:
        peer_engine = engine_pool.get(peer_key, warm=False)
    except KeyError:
        return None

    # Step 2a: Peer's last weights + fingerprint
    last_w = getattr(peer_engine, '_last_weights', None)
    last_z = getattr(peer_engine, '_last_zone', None)
    last_fp = getattr(peer_engine, '_state_fingerprint', 0)

    if last_w is not None and last_z == zone:
        if last_fp:
            fingerprint_store(last_fp, last_w, peer_key)
        return {
            "weights": last_w,
            "source": f"{peer_key}.last (sim={sim:.2f})",
            "similarity": sim,
            "zone": zone,
            "method": "peer_last",
        }

    # Step 2b: Peer store query + fingerprint
    try:
        w = peer_engine.store.query(zone, shape)
        if w is not None:
            fp = compute_fingerprint(w)
            fingerprint_store(fp, w, peer_key)
            return {
                "weights": w,
                "source": f"{peer_key}.store (sim={sim:.2f})",
                "similarity": sim,
                "zone": zone,
                "method": "peer_store",
            }
    except Exception:
        pass

    return None
