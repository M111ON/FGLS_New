"""
route_cache.py — Rule-based route cache (kills LLM in hot path).
=================================================================

Pattern → decision lookup that covers ~80% of routing cases
without any LLM call. Falls back to planner only for ambiguous patterns.

Usage:
  cache = RouteRuleCache()
  decision = cache.resolve(card)          # single zone
  decisions = cache.resolve_sequence(seq) # multi-zone
  # cache miss → call planner → cache.store(pattern, decision)
  stats = cache.stats()                   # hit/miss
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Optional

CARD_SPARSE = 0
CARD_BATCH = 1
CARD_LZ = 2
CARD_NAMES = {0: "sparse", 1: "batch", 2: "lz"}


@dataclass
class RouteRule:
    """Single routing rule: pattern matcher → decision."""
    priority: int = 0           # higher = checked first
    match_type: str = ""        # 'single' | 'sequence'
    card_type: int = -1         # -1 = any
    entropy_min: int = 0
    entropy_max: int = 255
    locality_min: int = 0
    locality_max: int = 255
    stability_min: int = 0
    stability_max: int = 255
    seq_pattern: tuple = ()     # sequence of card types, e.g. (1,0,1)
    decision: str = "fallback"
    action: str = "full_read"
    reason: str = "generic rule"


DEFAULT_RULES: list[RouteRule] = [
    # ── Skip-middle patterns (high priority) ──
    RouteRule(priority=105, match_type='sequence',
              seq_pattern=(1, 0, 1),
              decision='skip-middle',
              action='batch_skip_middle',
              reason='batch→sparse→batch: skip middle zone'),
    RouteRule(priority=104, match_type='sequence',
              seq_pattern=(0, 1, 0),
              decision='skip-middle',
              action='sparse_skip_middle',
              reason='sparse→batch→sparse: skip middle'),
    RouteRule(priority=103, match_type='sequence',
              seq_pattern=(0, 2, 0),
              decision='skip-middle',
              action='sparse_lz_skip_middle',
              reason='sparse→lz→sparse: skip middle'),
    RouteRule(priority=102, match_type='sequence',
              seq_pattern=(2, 0, 2),
              decision='skip-middle',
              action='lz_sparse_skip_middle',
              reason='lz→sparse→lz: skip middle'),

    # ── All-same type chains (length ≥ 3) ──
    RouteRule(priority=101, match_type='sequence',
              seq_pattern=(0, 0, 0),
              decision='skip-all',
              action='noop',
              reason='all sparse: skip entire block'),
    RouteRule(priority=100, match_type='sequence',
              seq_pattern=(1, 1, 1),
              decision='batch-chain',
              action='batch_chain',
              reason='batch×3: single batch route covers all'),
    RouteRule(priority=98, match_type='sequence',
              seq_pattern=(2, 2, 2),
              decision='lz-chain',
              action='lz_chain',
              reason='LZ×3: sequential decompress'),

    # ── 2-zone same-type ──
    RouteRule(priority=99, match_type='sequence',
              seq_pattern=(0, 0),
              decision='skip-both',
              action='noop',
              reason='sparse×2: skip both zones'),
    RouteRule(priority=97, match_type='sequence',
              seq_pattern=(1, 1),
              decision='batch-merge',
              action='merge_batch',
              reason='batch×2: merge into one route'),
    RouteRule(priority=96, match_type='sequence',
              seq_pattern=(2, 2),
              decision='lz-pair',
              action='pair_decompress',
              reason='LZ×2: paired decompress'),

    # ── 2-zone transitions ──
    RouteRule(priority=95, match_type='sequence',
              seq_pattern=(0, 1),
              decision='sparse-fill',
              action='sparse_to_batch',
              reason='sparse→batch: batch building from sparse'),
    RouteRule(priority=94, match_type='sequence',
              seq_pattern=(1, 0),
              decision='sparse-drain',
              action='batch_to_sparse',
              reason='batch→sparse: sparse draining batch'),
    RouteRule(priority=93, match_type='sequence',
              seq_pattern=(0, 2),
              decision='lz-entrance',
              action='sparse_to_lz',
              reason='sparse→lz: LZ building from sparse'),
    RouteRule(priority=92, match_type='sequence',
              seq_pattern=(2, 0),
              decision='lz-exit',
              action='lz_to_sparse',
              reason='lz→sparse: sparse replaces LZ'),
    RouteRule(priority=91, match_type='sequence',
              seq_pattern=(1, 2),
              decision='transition-b2l',
              action='batch_to_lz',
              reason='batch→lz: transition to LZ'),
    RouteRule(priority=90, match_type='sequence',
              seq_pattern=(2, 1),
              decision='transition-l2b',
              action='lz_to_batch',
              reason='lz→batch: transition to batch'),

    # ── 3-zone entrance/exit ──
    RouteRule(priority=89, match_type='sequence',
              seq_pattern=(0, 0, 1),
              decision='batch-entrance',
              action='enter_batch',
              reason='sparse×2→batch: batch entrance'),
    RouteRule(priority=88, match_type='sequence',
              seq_pattern=(1, 0, 0),
              decision='batch-exit',
              action='exit_batch',
              reason='batch→sparse×2: batch exit'),
    RouteRule(priority=87, match_type='sequence',
              seq_pattern=(2, 2, 0),
              decision='lz-exit',
              action='lz_to_sparse',
              reason='LZ×2→sparse: LZ exit'),
    RouteRule(priority=86, match_type='sequence',
              seq_pattern=(0, 2, 2),
              decision='lz-entrance',
              action='sparse_to_lz',
              reason='sparse→LZ×2: LZ entrance'),

    # ── Entropy cliff (any type, extremely unstable) ──
    RouteRule(priority=85, match_type='single',
              entropy_min=220, stability_max=30,
              decision='crash-plan', action='full_decode',
              reason='entropy cliff: full decode, no reuse'),
    RouteRule(priority=84, match_type='single',
              card_type=0, entropy_min=200,
              decision='sparse-crash', action='full_decode',
              reason='sparse with entropy cliff: full decode'),

    # ── Single-card: sparse (type 0) ──
    RouteRule(priority=83, match_type='single',
              card_type=0, entropy_max=30, stability_min=200,
              decision='skip', action='noop',
              reason='sparse+stable: skip'),
    RouteRule(priority=82, match_type='single',
              card_type=0, entropy_max=30, stability_min=100,
              decision='sparse-skip', action='noop',
              reason='sparse+moderate: skip, no action needed'),
    RouteRule(priority=81, match_type='single',
              card_type=0, entropy_max=30,
              decision='sparse-verify', action='sparse_verify',
              reason='sparse+unstable: verify before skip'),
    RouteRule(priority=80, match_type='single',
              card_type=0, entropy_min=31, entropy_max=127, stability_min=200,
              decision='sparse-route', action='sparse_decode',
              reason='sparse moderate ent+stable: decode'),
    RouteRule(priority=79, match_type='single',
              card_type=0, entropy_min=31, entropy_max=127,
              decision='sparse-verify', action='verify_decode',
              reason='sparse moderate ent: verify then decode'),
    RouteRule(priority=78, match_type='single',
              card_type=0, entropy_min=128,
              decision='sparse-full', action='sparse_decode',
              reason='sparse high ent: decode'),
    RouteRule(priority=77, match_type='single',
              card_type=0, locality_max=50,
              decision='sparse-fresh', action='fresh_route',
              reason='sparse+remote: fresh route'),

    # ── Single-card: batch (type 1) ──
    RouteRule(priority=76, match_type='single',
              card_type=1, entropy_max=50, stability_min=200, locality_min=200,
              decision='batch-chain', action='reuse_state',
              reason='batch+stable+local: reuse decode state'),
    RouteRule(priority=75, match_type='single',
              card_type=1, entropy_max=50, stability_min=200,
              decision='batch', action='batch_route',
              reason='batch+stable: fast route'),
    RouteRule(priority=74, match_type='single',
              card_type=1, entropy_max=50, stability_min=100,
              decision='batch-moderate', action='batch_route_fresh',
              reason='batch+moderate: batch route with fresh state'),
    RouteRule(priority=73, match_type='single',
              card_type=1, entropy_max=50,
              decision='batch-verify-fresh', action='verify_fresh_batch',
              reason='batch+unstable: verify fresh batch'),
    RouteRule(priority=72, match_type='single',
              card_type=1, entropy_min=51, locality_max=50,
              decision='batch-verify', action='verify_batch',
              reason='batch+low locality: verify before route'),
    RouteRule(priority=71, match_type='single',
              card_type=1, entropy_min=51, stability_min=200,
              decision='batch', action='batch_route',
              reason='batch+mod ent+stable: fast route'),
    RouteRule(priority=70, match_type='single',
              card_type=1, entropy_min=51,
              decision='batch-v', action='batch_verify',
              reason='batch moderate entropy: verify'),
    RouteRule(priority=69, match_type='single',
              card_type=1, locality_max=50,
              decision='batch-fresh', action='fresh_route',
              reason='batch+remote: fresh route'),

    # ── Single-card: lz (type 2) ──
    RouteRule(priority=68, match_type='single',
              card_type=2, entropy_min=220, stability_max=50,
              decision='full-decode', action='full_decode',
              reason='LZ+unstable: full decode required'),
    RouteRule(priority=67, match_type='single',
              card_type=2, entropy_min=200,
              decision='full', action='full_decode',
              reason='high-entropy LZ: full decode'),
    RouteRule(priority=66, match_type='single',
              card_type=2, entropy_min=200, stability_min=200,
              decision='lz-route', action='lz_decompress',
              reason='high-entropy LZ but stable: decompress'),
    RouteRule(priority=65, match_type='single',
              card_type=2, entropy_max=199, stability_min=200,
              decision='lz-route', action='lz_decompress',
              reason='LZ+stable: decompress then route'),
    RouteRule(priority=64, match_type='single',
              card_type=2, entropy_max=199,
              decision='lz-verify', action='lz_verify',
              reason='LZ moderate ent: verify decompress'),
    RouteRule(priority=63, match_type='single',
              card_type=2, locality_max=50,
              decision='lz-fresh', action='fresh_route',
              reason='LZ+isolated: fresh route, no reuse'),

    # ── Locality cascade: high locality overrides type-specific ──
    RouteRule(priority=87, match_type='single',
              locality_min=220,
              decision='cascade-reuse', action='reuse_state',
              reason='high locality: reuse previous decode state'),
    RouteRule(priority=86, match_type='single',
              locality_min=180, stability_min=200,
              decision='cascade-merge', action='merge_state',
              reason='high locality+stable: merge state'),

    # ── Catch-all: low priority single ──
    RouteRule(priority=1,
              decision='fallback', action='full_read',
              reason='unmatched: fallback to full read'),
]


class RouteRuleCache:
    """
    Pattern-based route cache. Resolves ~80% of cases instantly.

    Three modes:
      1. Sequence pattern match (exact type list)
      2. Single card rule match (type + entropy + locality + stability)
      3. Exact hash match (cached LLM decisions)
    """

    def __init__(self, rules: list[RouteRule] | None = None):
        self._rules = sorted(DEFAULT_RULES if rules is None else rules,
                             key=lambda r: -r.priority)
        self._exact_cache: dict[int, dict] = {}  # hash → decision
        self._hits = 0
        self._misses = 0
        self._seql_hits = 0
        self._seql_misses = 0

    # ── Single card resolve ──

    def resolve(self, card) -> dict:
        """Resolve a single ZoneCard to a route decision."""
        ct = card.card_type if hasattr(card, 'card_type') else card.get('card_type', -1)
        ent = card.entropy if hasattr(card, 'entropy') else card.get('entropy', 128)
        loc = card.locality if hasattr(card, 'locality') else card.get('locality', 128)
        st = card.stability if hasattr(card, 'stability') else card.get('stability', 128)

        # Check exact cache first (hash-based)
        hv = card.hash_val if hasattr(card, 'hash_val') else card.get('hash_val', 0)
        if hv and hv in self._exact_cache:
            self._hits += 1
            return {**self._exact_cache[hv], "cache": "exact"}

        # Check rules
        for rule in self._rules:
            if rule.match_type != 'single':
                continue
            if rule.card_type >= 0 and ct != rule.card_type:
                continue
            if not (rule.entropy_min <= ent <= rule.entropy_max):
                continue
            if not (rule.locality_min <= loc <= rule.locality_max):
                continue
            if not (rule.stability_min <= st <= rule.stability_max):
                continue
            self._hits += 1
            return {
                "decision": rule.decision,
                "action": rule.action,
                "reason": rule.reason,
                "cache": "rule",
                "rule_priority": rule.priority,
            }

        self._misses += 1
        return {
            "decision": "planner",
            "action": "call_llm",
            "reason": f"no rule for type={CARD_NAMES.get(ct,str(ct))} ent={ent} loc={loc} st={st}",
            "cache": "miss",
        }

    def resolve_sequence(self, seq) -> list[dict]:
        """Resolve a ZoneSequence (multi-zone) to route decisions."""
        types = tuple(c.card_type if hasattr(c, 'card_type') else c.get('card_type', -1)
                      for c in (seq.cards if hasattr(seq, 'cards') else seq))
        n = len(types)

        # Try sequence pattern match first
        for rule in self._rules:
            if rule.match_type != 'sequence':
                continue
            sp = rule.seq_pattern
            if len(sp) != n:
                continue
            if all(t == sp[i] or sp[i] < 0 for i, t in enumerate(types)):
                self._seql_hits += 1
                decisions = []
                for i in range(n):
                    if rule.decision == 'skip-middle' and 0 < i < n - 1:
                        d = {"decision": "skip", "action": "noop",
                             "reason": f"skip-middle (seq rule: {rule.reason})",
                             "cache": "seq_rule"}
                    elif rule.decision == 'skip-all':
                        d = {"decision": "skip", "action": "noop",
                             "reason": rule.reason, "cache": "seq_rule"}
                    elif rule.decision in ('batch-chain', 'batch-merge'):
                        d = {"decision": "batch", "action": rule.action,
                             "reason": rule.reason, "cache": "seq_rule"}
                    else:
                        d = {"decision": rule.decision, "action": rule.action,
                             "reason": rule.reason, "cache": "seq_rule"}
                    decisions.append(d)
                return decisions

        # No sequence match → individual resolve
        self._seql_misses += 1
        return [self.resolve(c) for c in (seq.cards if hasattr(seq, 'cards') else seq)]

    # ── Store ──

    def store(self, card, decision: dict):
        """Cache an LLM decision for exact hash match next time."""
        hv = card.hash_val if hasattr(card, 'hash_val') else card.get('hash_val', 0)
        if hv:
            self._exact_cache[hv] = {
                "decision": decision.get("decision", "fallback"),
                "action": decision.get("action", "full_read"),
                "reason": decision.get("reason", ""),
            }

    def add_rule(self, rule: RouteRule):
        """Add custom rule at runtime."""
        self._rules.append(rule)
        self._rules.sort(key=lambda r: -r.priority)

    # ── Stats ──

    def stats(self) -> dict:
        total = self._hits + self._misses
        seq_total = self._seql_hits + self._seql_misses
        return {
            "single_hits": self._hits,
            "single_misses": self._misses,
            "single_hit_rate": round(self._hits / max(total, 1) * 100, 1),
            "seq_hits": self._seql_hits,
            "seq_misses": self._seql_misses,
            "seq_hit_rate": round(self._seql_hits / max(seq_total, 1) * 100, 1),
            "n_rules": len(self._rules),
            "n_exact_cache": len(self._exact_cache),
        }

    def rules_summary(self) -> list[dict]:
        return [
            {
                "priority": r.priority,
                "type": r.match_type,
                "pattern": list(r.seq_pattern) if r.seq_pattern
                           else f"ct={r.card_type} ent={r.entropy_min}-{r.entropy_max} "
                                f"loc={r.locality_min}-{r.locality_max} "
                                f"st={r.stability_min}-{r.stability_max}",
                "decision": r.decision,
                "action": r.action,
            }
            for r in self._rules
        ]


# ── Singleton ──

_GLOBAL_CACHE: RouteRuleCache | None = None


def get_cache() -> RouteRuleCache:
    global _GLOBAL_CACHE
    if _GLOBAL_CACHE is None:
        _GLOBAL_CACHE = RouteRuleCache()
    return _GLOBAL_CACHE
