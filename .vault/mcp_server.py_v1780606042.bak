"""
mcp_server.py — ZoneCard MCP Server v2
CoreCard + Deck + Versioning + Event Store + Push/Pull + 44 rules + seq resolve
"""
from __future__ import annotations
import asyncio, json, os, threading, time
from pathlib import Path
from dataclasses import dataclass
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, Request, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse, JSONResponse
from pydantic import BaseModel

STATE_DIR = Path(os.environ.get("MCP_STATE_DIR", "./mcp_state_v2"))
STATE_DIR.mkdir(exist_ok=True)

_lock = threading.RLock()
cards: Dict[str, dict] = {}
events: List[dict] = []
deck_manifests: Dict[str, dict] = {}
exact_cache: Dict[str, dict] = {}
fusion_table: Dict[str, dict] = {}
_global_version = 0
_hits = 0
_misses = 0

# ── Zone map ──
SCRIPT_DIR = Path(__file__).parent
ZONE_MAP_FILE = SCRIPT_DIR / "mcp_zone_map.json"
zone_map: dict = {}
if ZONE_MAP_FILE.exists():
    try:
        zone_map = json.loads(ZONE_MAP_FILE.read_text())
        print(f"[MCPv2] zone_map: {len(zone_map.get('zones',[]))} zones")
    except Exception as e:
        print(f"[MCPv2] warn zone_map: {e}")

# ── Route rules (canonical 44 from v1) ──
CARD_NAMES = {0: "sparse", 1: "batch", 2: "lz"}

@dataclass
class RouteRule:
    priority: int = 0
    match_type: str = ""
    card_type: int = -1
    entropy_min: int = 0; entropy_max: int = 255
    locality_min: int = 0; locality_max: int = 255
    stability_min: int = 0; stability_max: int = 255
    seq_pattern: tuple = ()
    decision: str = "fallback"
    action: str = "full_read"
    reason: str = "generic rule"

ROUTE_RULES: list[RouteRule] = [
    RouteRule(priority=105, match_type='sequence', seq_pattern=(1,0,1), decision='skip-middle', action='batch_skip_middle', reason='batch→sparse→batch'),
    RouteRule(priority=104, match_type='sequence', seq_pattern=(0,1,0), decision='skip-middle', action='sparse_skip_middle', reason='sparse→batch→sparse'),
    RouteRule(priority=103, match_type='sequence', seq_pattern=(0,2,0), decision='skip-middle', action='sparse_lz_skip_middle', reason='sparse→lz→sparse'),
    RouteRule(priority=102, match_type='sequence', seq_pattern=(2,0,2), decision='skip-middle', action='lz_sparse_skip_middle', reason='lz→sparse→lz'),
    RouteRule(priority=101, match_type='sequence', seq_pattern=(0,0,0), decision='skip-all', action='noop', reason='all sparse'),
    RouteRule(priority=100, match_type='sequence', seq_pattern=(1,1,1), decision='batch-chain', action='batch_chain', reason='batch×3'),
    RouteRule(priority=98,  match_type='sequence', seq_pattern=(2,2,2), decision='lz-chain', action='lz_chain', reason='LZ×3'),
    RouteRule(priority=99,  match_type='sequence', seq_pattern=(0,0), decision='skip-both', action='noop', reason='sparse×2'),
    RouteRule(priority=97,  match_type='sequence', seq_pattern=(1,1), decision='batch-merge', action='merge_batch', reason='batch×2'),
    RouteRule(priority=96,  match_type='sequence', seq_pattern=(2,2), decision='lz-pair', action='pair_decompress', reason='LZ×2'),
    RouteRule(priority=95,  match_type='sequence', seq_pattern=(0,1), decision='sparse-fill', action='sparse_to_batch', reason='sparse→batch'),
    RouteRule(priority=94,  match_type='sequence', seq_pattern=(1,0), decision='sparse-drain', action='batch_to_sparse', reason='batch→sparse'),
    RouteRule(priority=93,  match_type='sequence', seq_pattern=(0,2), decision='lz-entrance', action='sparse_to_lz', reason='sparse→lz'),
    RouteRule(priority=92,  match_type='sequence', seq_pattern=(2,0), decision='lz-exit', action='lz_to_sparse', reason='lz→sparse'),
    RouteRule(priority=91,  match_type='sequence', seq_pattern=(1,2), decision='transition-b2l', action='batch_to_lz', reason='batch→lz'),
    RouteRule(priority=90,  match_type='sequence', seq_pattern=(2,1), decision='transition-l2b', action='lz_to_batch', reason='lz→batch'),
    RouteRule(priority=89,  match_type='sequence', seq_pattern=(0,0,1), decision='batch-entrance', action='enter_batch', reason='sparse×2→batch'),
    RouteRule(priority=88,  match_type='sequence', seq_pattern=(1,0,0), decision='batch-exit', action='exit_batch', reason='batch→sparse×2'),
    RouteRule(priority=87,  match_type='sequence', seq_pattern=(2,2,0), decision='lz-exit', action='lz_to_sparse', reason='LZ×2→sparse'),
    RouteRule(priority=86,  match_type='sequence', seq_pattern=(0,2,2), decision='lz-entrance', action='sparse_to_lz', reason='sparse→LZ×2'),
    RouteRule(priority=85,  match_type='single', entropy_min=220, stability_max=30, decision='crash-plan', action='full_decode', reason='entropy cliff'),
    RouteRule(priority=84,  match_type='single', card_type=0, entropy_min=200, decision='sparse-crash', action='full_decode', reason='sparse entropy cliff'),
    RouteRule(priority=83,  match_type='single', card_type=0, entropy_max=30, stability_min=200, decision='skip', action='noop', reason='sparse+stable'),
    RouteRule(priority=82,  match_type='single', card_type=0, entropy_max=30, stability_min=100, decision='sparse-skip', action='noop', reason='sparse+moderate'),
    RouteRule(priority=81,  match_type='single', card_type=0, entropy_max=30, decision='sparse-verify', action='sparse_verify', reason='sparse+unstable'),
    RouteRule(priority=80,  match_type='single', card_type=0, entropy_min=31, entropy_max=127, stability_min=200, decision='sparse-route', action='sparse_decode', reason='sparse mod+stable'),
    RouteRule(priority=79,  match_type='single', card_type=0, entropy_min=31, entropy_max=127, decision='sparse-verify', action='verify_decode', reason='sparse moderate'),
    RouteRule(priority=78,  match_type='single', card_type=0, entropy_min=128, decision='sparse-full', action='sparse_decode', reason='sparse high ent'),
    RouteRule(priority=77,  match_type='single', card_type=0, locality_max=50, decision='sparse-fresh', action='fresh_route', reason='sparse+remote'),
    RouteRule(priority=76,  match_type='single', card_type=1, entropy_max=50, stability_min=200, locality_min=200, decision='batch-chain', action='reuse_state', reason='batch+stable+local'),
    RouteRule(priority=75,  match_type='single', card_type=1, entropy_max=50, stability_min=200, decision='batch', action='batch_route', reason='batch+stable'),
    RouteRule(priority=74,  match_type='single', card_type=1, entropy_max=50, stability_min=100, decision='batch-moderate', action='batch_route_fresh', reason='batch+moderate'),
    RouteRule(priority=73,  match_type='single', card_type=1, entropy_max=50, decision='batch-verify-fresh', action='verify_fresh_batch', reason='batch+unstable'),
    RouteRule(priority=72,  match_type='single', card_type=1, entropy_min=51, locality_max=50, decision='batch-verify', action='verify_batch', reason='batch+low loc'),
    RouteRule(priority=71,  match_type='single', card_type=1, entropy_min=51, stability_min=200, decision='batch', action='batch_route', reason='batch mod+stable'),
    RouteRule(priority=70,  match_type='single', card_type=1, entropy_min=51, decision='batch-v', action='batch_verify', reason='batch moderate'),
    RouteRule(priority=69,  match_type='single', card_type=1, locality_max=50, decision='batch-fresh', action='fresh_route', reason='batch+remote'),
    RouteRule(priority=68,  match_type='single', card_type=2, entropy_min=220, stability_max=50, decision='full-decode', action='full_decode', reason='LZ+unstable'),
    RouteRule(priority=67,  match_type='single', card_type=2, entropy_min=200, decision='full', action='full_decode', reason='LZ high ent'),
    RouteRule(priority=66,  match_type='single', card_type=2, entropy_min=200, stability_min=200, decision='lz-route', action='lz_decompress', reason='LZ high ent stable'),
    RouteRule(priority=65,  match_type='single', card_type=2, entropy_max=199, stability_min=200, decision='lz-route', action='lz_decompress', reason='LZ+stable'),
    RouteRule(priority=64,  match_type='single', card_type=2, entropy_max=199, decision='lz-verify', action='lz_verify', reason='LZ moderate'),
    RouteRule(priority=63,  match_type='single', card_type=2, locality_max=50, decision='lz-fresh', action='fresh_route', reason='LZ+isolated'),
    RouteRule(priority=91,  match_type='single', locality_min=220, decision='cascade-reuse', action='reuse_state', reason='high locality'),
    RouteRule(priority=90,  match_type='single', locality_min=180, stability_min=200, decision='cascade-merge', action='merge_state', reason='high loc+stable'),
    RouteRule(priority=1,   decision='fallback', action='full_read', reason='unmatched'),
]
_sorted_rules = sorted(ROUTE_RULES, key=lambda r: -r.priority)

def _resolve_one(ct: int, ent: int, loc: int, st: int, hash_val: int = 0) -> dict:
    global _hits, _misses
    hk = str(hash_val)
    if hash_val and hk in exact_cache:
        _hits += 1
        return {**exact_cache[hk], "cache": "exact"}
    for r in _sorted_rules:
        if r.match_type != 'single': continue
        if r.card_type >= 0 and ct != r.card_type: continue
        if not (r.entropy_min <= ent <= r.entropy_max): continue
        if not (r.locality_min <= loc <= r.locality_max): continue
        if not (r.stability_min <= st <= r.stability_max): continue
        _hits += 1
        return {"decision": r.decision, "action": r.action, "reason": r.reason, "cache": "rule", "priority": r.priority}
    _misses += 1
    return {"decision": "planner", "action": "call_llm", "cache": "miss", "reason": f"no rule ct={ct} ent={ent}"}

def _resolve_seq(seq_types) -> list[dict]:
    n = len(seq_types)
    for r in _sorted_rules:
        if r.match_type != 'sequence': continue
        sp = r.seq_pattern
        if len(sp) != n: continue
        if all(t == sp[i] or sp[i] < 0 for i, t in enumerate(seq_types)):
            return [{"decision": "skip" if (r.decision == 'skip-middle' and 0 < i < n-1) or r.decision == 'skip-all' else (r.decision if r.decision in ('batch','skip') else "route"), "action": r.action, "reason": r.reason, "cache": "seq_rule"} for i in range(n)]
    return [_resolve_one(t, 128, 128, 128) for t in seq_types]

def _similarity(a: dict, b: dict) -> float:
    score = 0.0
    if a.get("card_schema_version") == b.get("card_schema_version"): score += 0.10
    ac = a.get("card_type") or a.get("extensions",{}).get("card_type")
    bc = b.get("card_type") or b.get("extensions",{}).get("card_type")
    if ac is not None and ac == bc: score += 0.30
    score += 0.20 * max(0, 1 - abs(a.get("entropy",128)-b.get("entropy",128))/50)
    score += 0.15 * max(0, 1 - abs(a.get("locality",128)-b.get("locality",128))/100)
    score += 0.10 * max(0, 1 - abs(a.get("stability",128)-b.get("stability",128))/100)
    if a.get("fingerprint") and a.get("fingerprint") == b.get("fingerprint"): score += 0.15
    return min(score, 1.0)

def _validate_core(card: dict) -> Optional[str]:
    from core_card import CoreCard
    try:
        cc = CoreCard.from_dict(card)
        return cc.validate()
    except Exception as e:
        return str(e)

def _load_state():
    global cards, events, deck_manifests, exact_cache, fusion_table, _global_version
    files = {"cards": STATE_DIR/"cards.json", "events": STATE_DIR/"events.json",
             "deck_manifests": STATE_DIR/"decks.json", "exact_cache": STATE_DIR/"exact_cache.json",
             "fusion_table": STATE_DIR/"fusion_table.json"}
    for name, path in files.items():
        if path.exists():
            try:
                with open(path) as f: data = json.load(f)
                if isinstance(data, list): globals()[name].extend(data)
                else: globals()[name].update(data)
                print(f"[MCPv2] {name}: {len(data)} entries")
            except Exception as e:
                print(f"[MCPv2] warn {path}: {e}")
    if events:
        _global_version = max(e.get("version", 0) for e in events)

def _save_state():
    files = {"cards": STATE_DIR/"cards.json", "events": STATE_DIR/"events.json",
             "deck_manifests": STATE_DIR/"decks.json", "exact_cache": STATE_DIR/"exact_cache.json",
             "fusion_table": STATE_DIR/"fusion_table.json"}
    with _lock:
        for name, path in files.items():
            try:
                with open(path, "w") as f: json.dump(globals()[name], f, indent=2)
            except Exception as e:
                print(f"[MCPv2] save warn {path}: {e}")

app = FastAPI(title="ZoneCard MCP v2", version="2.0.0")
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])

@app.on_event("startup")
async def startup(): _load_state()

@app.on_event("shutdown")
async def shutdown(): _save_state()

@app.get("/sse")
async def sse(request: Request):
    manifest = {"name":"zonecard-mcp-v2","version":"2.0.0","description":"CoreCard MCP v2"}
    async def stream():
        yield f"event: endpoint\ndata: /messages\n\n"
        yield f"event: manifest\ndata: {json.dumps(manifest)}\n\n"
        while not await request.is_disconnected():
            yield ": keepalive\n\n"
            await asyncio.sleep(15)
    return StreamingResponse(stream(), media_type="text/event-stream")

# ── Push ──
class PushReq(BaseModel):
    key: str; card: dict; base_version: int = 0

@app.post("/cards/push")
async def push(req: PushReq):
    global _global_version
    err = _validate_core(req.card)
    if err: return JSONResponse({"ok": False, "error": err}, status_code=400)
    with _lock:
        existing = cards.get(req.key)
        if existing:
            sv = existing.get("version", 0)
            if req.base_version < sv:
                return JSONResponse({"ok": False, "error": "version_conflict",
                    "server_version": sv, "your_base": req.base_version,
                    "hint": f"pull v{sv} and rebase"}, status_code=409)
        _global_version += 1
        new_card = {**req.card, "version": _global_version,
                    "parent_version": req.base_version,
                    "server_timestamp": int(time.time())}
        cards[req.key] = new_card
        events.append({"version": _global_version, "key": req.key,
            "author": req.card.get("author","?"), "deck": req.card.get("deck",""),
            "card_schema_version": req.card.get("card_schema_version",""),
            "parent_version": req.base_version,
            "fingerprint": req.card.get("fingerprint",0), "timestamp": int(time.time())})
        fk = ":".join(req.key.split(":")[:3])
        if fk not in fusion_table: fusion_table[fk] = {}
        author = req.card.get("author","")
        for pk, pc in cards.items():
            if pk == req.key: continue
            if pc.get("deck") != req.card.get("deck"): continue
            sim = _similarity(new_card, pc)
            if sim >= 0.75:
                peer_author = pc.get("author","")
                fusion_table[fk][author] = {"similarity": sim, "peer": peer_author, "version": _global_version}
                # Also store under peer's fk for cross-lookup
                pk_fk = ":".join(pk.split(":")[:3])
                if pk_fk != fk:
                    if pk_fk not in fusion_table: fusion_table[pk_fk] = {}
                    fusion_table[pk_fk][peer_author] = {"similarity": sim, "peer": author, "version": _global_version}
    _save_state()
    return JSONResponse({"ok": True, "version": _global_version})

# ── Pull ──
class PullReq(BaseModel):
    since_version: int = 0; deck: str = ""

@app.post("/cards/pull")
async def pull(req: PullReq):
    with _lock:
        evs = [e for e in events if e["version"] > req.since_version and (not req.deck or e.get("deck") == req.deck)]
        result = {e["key"]: cards[e["key"]] for e in evs if e["key"] in cards}
    return JSONResponse({"cards": result, "events": evs, "latest_version": _global_version, "count": len(result)})

# ── Get card ──
@app.get("/cards/{deck}/{key:path}")
async def get_card(deck: str, key: str):
    full = f"{deck}:{key}"
    with _lock:
        card = cards.get(full) or cards.get(key)
    if not card: raise HTTPException(404, f"not found: {full}")
    return JSONResponse({"found": True, "card": card})

# ── Tools ──
class RouteReq(BaseModel):
    card_type: int = 1; entropy: int = 128; locality: int = 128; stability: int = 128; hash_val: int = 0

@app.post("/tools/resolve_route")
async def resolve_route(req: RouteReq):
    return JSONResponse(_resolve_one(req.card_type, req.entropy, req.locality, req.stability, req.hash_val))

class SeqReq(BaseModel):
    types: List[int]

@app.post("/tools/resolve_sequence")
async def resolve_sequence(req: SeqReq):
    return JSONResponse({"decisions": _resolve_seq(req.types)})

class FusionReq(BaseModel):
    key: str; threshold: float = 0.75

@app.post("/tools/get_fusion_peer")
async def fusion_peer(req: FusionReq):
    fk = ":".join(req.key.split(":")[:3])
    with _lock:
        entries = fusion_table.get(fk, {})
    best = max(entries.items(), key=lambda x: x[1].get("similarity",0), default=None)
    if not best or best[1].get("similarity",0) < req.threshold:
        return JSONResponse({"found": False})
    return JSONResponse({"found": True, "peer": best[1].get("peer"), "similarity": best[1]["similarity"]})

@app.post("/tools/store_decision")
async def store_decision(req: dict):
    return JSONResponse({"ok": False, "error": "use push_card instead"})

@app.post("/tools/store_zone_card")
async def store_zone_card(req: dict):
    return JSONResponse({"ok": False, "error": "use push_card instead"})

# ── Stats ──
@app.get("/tools/cache_stats")
async def stats():
    total = _hits + _misses
    with _lock:
        return JSONResponse({
            "hits": _hits, "misses": _misses,
            "hit_rate": round(_hits/max(total,1)*100,1),
            "cards": len(cards), "events": len(events),
            "decks": list(deck_manifests.keys()),
            "global_version": _global_version, "n_rules": len(_sorted_rules),
        })

# ── Decks ──
class DeckReq(BaseModel):
    deck: str; schema: str; ruleset: str
    version: int = 1; description: str = ""; adapter_from: list = []

@app.get("/decks")
async def list_decks():
    return JSONResponse({"decks": deck_manifests})

@app.post("/decks")
async def reg_deck(req: DeckReq):
    with _lock: deck_manifests[req.deck] = req.dict()
    _save_state()
    return JSONResponse({"registered": True, "deck": req.deck})

# ── Adapt ──
class AdaptReq(BaseModel):
    card: dict; target_schema: str

@app.post("/adapt")
async def adapt(req: AdaptReq):
    try:
        from core_card import CoreCard, adapt_card as _adapt
        src = CoreCard.from_dict(req.card)
        result = _adapt(src, req.target_schema)
        if result is None:
            return JSONResponse({"ok": False, "error": f"no adapter {src.card_schema_version}→{req.target_schema}"}, status_code=400)
        return JSONResponse({"ok": True, "card": result.to_dict()})
    except Exception as e:
        return JSONResponse({"ok": False, "error": str(e)}, status_code=500)

# ── Zone info ──
class ZoneInfoReq(BaseModel):
    zone: int

@app.post("/tools/get_zone_info")
async def get_zone_info(req: ZoneInfoReq):
    for z in zone_map.get("zones", []):
        if z["zone"] == req.zone:
            return JSONResponse({"found": True, "info": z})
    return JSONResponse({"found": False, "zone": req.zone})

# ── Events ──
@app.get("/events")
async def get_events(since: int = 0, deck: str = ""):
    with _lock:
        evs = [e for e in events if e["version"] > since and (not deck or e.get("deck") == deck)]
    return JSONResponse({"events": evs, "latest_version": _global_version})

# ── Admin ──
@app.post("/admin/reset")
async def reset():
    global cards, events, deck_manifests, exact_cache, fusion_table, _global_version, _hits, _misses
    with _lock:
        cards.clear(); events.clear(); deck_manifests.clear()
        exact_cache.clear(); fusion_table.clear()
        _global_version = 0; _hits = 0; _misses = 0
    _save_state()
    return JSONResponse({"reset": True})

# ── Dispatcher ──
@app.post("/messages")
async def messages(request: Request):
    body = await request.json()
    tool = body.get("tool") or body.get("name")
    p = body.get("input") or body.get("params") or {}
    if tool == "push_card": return await push(PushReq(**p))
    if tool == "pull_cards": return await pull(PullReq(**p))
    if tool == "resolve_route": return await resolve_route(RouteReq(**p))
    if tool == "resolve_sequence": return await resolve_sequence(SeqReq(**p))
    if tool == "get_fusion_peer": return await fusion_peer(FusionReq(**p))
    if tool == "register_deck": return await reg_deck(DeckReq(**p))
    if tool == "adapt_card": return await adapt(AdaptReq(**p))
    if tool == "get_zone_info": return await get_zone_info(ZoneInfoReq(**p))
    if tool == "store_decision": return await store_decision(p)
    if tool == "store_zone_card": return await store_zone_card(p)
    return JSONResponse({"error": f"unknown: {tool}"}, status_code=400)

if __name__ == "__main__":
    import uvicorn
    port = int(os.environ.get("MCP_PORT", 8765))
    print(f"[MCPv2] zonecard-mcp-v2 on port {port}")
    uvicorn.run(app, host="0.0.0.0", port=port)
