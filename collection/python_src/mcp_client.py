"""
mcp_client.py — ZoneCard MCP Client v2
CoreCard + push/pull + deck registry + resolve
"""
from __future__ import annotations
import json, urllib.request, urllib.error
from typing import Any, Dict, List, Optional

class MCPClient:
    def __init__(self, base_url: str = "http://localhost:8765", timeout: float = 5.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self._online = True

    def _post(self, path: str, payload: dict) -> Optional[dict]:
        try:
            data = json.dumps(payload).encode()
            req = urllib.request.Request(
                f"{self.base_url}{path}", data=data,
                headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                self._online = True
                return json.loads(resp.read())
        except Exception:
            self._online = False
            return None

    def _get(self, path: str) -> Optional[dict]:
        try:
            with urllib.request.urlopen(f"{self.base_url}{path}", timeout=self.timeout) as resp:
                self._online = True
                return json.loads(resp.read())
        except Exception:
            self._online = False
            return None

    @property
    def online(self) -> bool:
        return self._online

    # ── Push / Pull ──
    def push_card(self, key: str, card: dict, base_version: int = 0) -> dict:
        result = self._post("/cards/push", {"key": key, "card": card, "base_version": base_version})
        return result or {"ok": False, "error": "server unreachable"}

    def pull_cards(self, since_version: int = 0, deck: str = "") -> dict:
        result = self._post("/cards/pull", {"since_version": since_version, "deck": deck})
        return result or {"cards": {}, "events": [], "count": 0}

    def get_card(self, deck: str, key: str) -> Optional[dict]:
        return self._get(f"/cards/{deck}/{key}")

    # ── Route ──
    def resolve_route(self, card_type: int, entropy: int,
                      locality: int, stability: int, hash_val: int = 0) -> dict:
        result = self._post("/tools/resolve_route", {
            "card_type": card_type, "entropy": entropy,
            "locality": locality, "stability": stability, "hash_val": hash_val})
        if result: return result
        return {"decision": "fallback", "action": "full_read", "cache": "offline"}

    def resolve_sequence(self, types: List[int]) -> dict:
        result = self._post("/tools/resolve_sequence", {"types": types})
        return result or {"decisions": []}

    def get_fusion_peer(self, key: str, threshold: float = 0.75) -> Optional[dict]:
        result = self._post("/tools/get_fusion_peer", {"key": key, "threshold": threshold})
        if result and result.get("found"): return result
        return None

    # ── Deck registry ──
    def register_deck(self, deck: str, schema: str, ruleset: str,
                      description: str = "", adapter_from: list = []) -> bool:
        result = self._post("/decks", {
            "deck": deck, "schema": schema, "ruleset": ruleset,
            "version": 1, "description": description, "adapter_from": adapter_from})
        return bool(result and result.get("registered"))

    def list_decks(self) -> dict:
        return self._get("/decks") or {"decks": {}}

    # ── Adapt ──
    def adapt_card(self, card: dict, target_schema: str) -> Optional[dict]:
        result = self._post("/adapt", {"card": card, "target_schema": target_schema})
        if result and result.get("ok"): return result["card"]
        return None

    # ── Zone info ──
    def get_zone_info(self, zone: int) -> Optional[dict]:
        result = self._post("/tools/get_zone_info", {"zone": zone})
        if result and result.get("found"): return result["info"]
        return None

    # ── Events + Stats ──
    def get_events(self, since: int = 0, deck: str = "") -> dict:
        path = f"/events?since={since}" + (f"&deck={deck}" if deck else "")
        return self._get(path) or {"events": []}

    def stats(self) -> dict:
        return self._get("/tools/cache_stats") or {}

    # ── Legacy compat (v1, will error on v2) ──
    def store_zone_card(self, **kwargs) -> bool:
        return False  # v2: use push_card instead

    def store_decision(self, **kwargs) -> bool:
        return False  # v2: use push_card instead
