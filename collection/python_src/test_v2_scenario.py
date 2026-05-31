"""Real scenario: 2 teams, shared MCP, prompt handoff"""
import sys, subprocess, time, os, shutil, json, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from mcp_client import MCPClient

sd = Path(tempfile.mkdtemp(prefix="mcp_scenario_"))
os.environ["MCP_STATE_DIR"] = str(sd)
port = "18773"

proc = subprocess.Popen(
    [sys.executable, "mcp_server.py"],
    cwd=str(Path(__file__).parent),
    env={**os.environ, "MCP_PORT": port},
    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)

mcp = MCPClient(f"http://localhost:{port}")

print("=" * 65)
print("SCENARIO: Geometry routing, 2 teams, 1 shared MCP")
print("=" * 65)

# ── Step 1: Pipeline Team registers decks ──
print("\n[TEAM A: Pipeline] Registering decks + pushing 6 zone cards...")
mcp.register_deck("geom_pipeline", "zonecard-v2", "route-v46",
                   "FGLS geometry pipeline zones")
mcp.register_deck("cloud_routing", "intentcard-v1", "route-v46",
                   "Cloud intent routing")

cards = [
    ("geom_pipeline:z0", {"deck":"geom_pipeline", "author":"qwen25coder",
     "card_schema_version":"zonecard-v2", "entropy":234, "stability":0, "locality":128,
     "std":0.048, "max_abs":3.094,
     "fingerprint":0xABCD, "extensions":{"card_type":2,"zone_id":0,"band":"UHF"}}),
    ("geom_pipeline:z1", {"deck":"geom_pipeline", "author":"qwen25coder",
     "card_schema_version":"zonecard-v2", "entropy":45, "stability":200, "locality":220,
     "std":0.126, "max_abs":3.766,
     "fingerprint":0xBCDE, "extensions":{"card_type":0,"zone_id":1,"band":"VHF"}}),
    ("geom_pipeline:z2", {"deck":"geom_pipeline", "author":"smollm2",
     "card_schema_version":"zonecard-v2", "entropy":230, "stability":5, "locality":120,
     "std":0.137, "max_abs":2.234,
     "fingerprint":0xCDEF, "extensions":{"card_type":2,"zone_id":2,"band":"UHF"}}),
    ("geom_pipeline:z3", {"deck":"geom_pipeline", "author":"smollm2",
     "card_schema_version":"zonecard-v2", "entropy":88, "stability":170, "locality":80,
     "std":0.130, "max_abs":2.938,
     "fingerprint":0xDEF0, "extensions":{"card_type":1,"zone_id":3,"band":"SHF"}}),
    ("geom_pipeline:z4", {"deck":"geom_pipeline", "author":"deepseek",
     "card_schema_version":"zonecard-v2", "entropy":200, "stability":30, "locality":50,
     "std":0.142, "max_abs":3.859,
     "fingerprint":0xEF01, "extensions":{"card_type":2,"zone_id":4,"band":"EHF","note":"new reasoning model"}}),
    ("geom_pipeline:z5", {"deck":"geom_pipeline", "author":"deepseek",
     "card_schema_version":"zonecard-v2", "entropy":60, "stability":190, "locality":180,
     "std":0.158, "max_abs":1.922,
     "fingerprint":0xF012, "extensions":{"card_type":0,"zone_id":5,"band":"HF"}}),
]
for key, card in cards:
    r = mcp.push_card(key, card)
    # diagnostic
    actual = r.get("version", r.get("_raw", "??"))
    print(f"  {key:25s} ok={r.get('ok')} version={actual} server_error={r.get('error')}")
print()

# ── Step 2: Routing Team resolves all routes ──
print("[TEAM B: Routing] Resolving routes for all zones...")
for key, card in cards:
    ct = card["extensions"].get("card_type", 0)
    route = mcp.resolve_route(ct, card["entropy"], card["locality"], card["stability"])
    print(f"  {key:25s} ct={ct} ent={card['entropy']:3d} -> {route['decision']:20s} ({route['action']}) [{route['cache']}]")
print()

# ── Step 3: Sequence resolve ──
print("[TEAM B] Sequence resolve: z0->z1->z2 (3 types in series)")
seq = mcp.resolve_sequence([2, 0, 2])
for i, d in enumerate(seq.get("decisions", [])):
    print(f"  card[{i}] type={[2,0,2][i]}: {d['decision']:15s} ({d['reason']}) [{d['cache']}]")
print()

# ── Step 4: Fusion peer ──
print("[TEAM B] Fusion peers (cross-model similarity):")
indices = {"qwen25coder": 0, "smollm2": 2, "deepseek": 4}
for model, ci in indices.items():
    peer = mcp.get_fusion_peer(f"geom_pipeline:z{ci}")
    if peer:
        print(f"  {model:15s} -> peer={peer['peer']:12s} sim={peer['similarity']:.3f}")
    else:
        print(f"  {model:15s} -> no peer (below 0.75)")
print()

# ── Step 5: Store exact-match decision ──
print("[TEAM B] Storing LLM decision for hash 0xABCD (custom override)...")
r = mcp.push_card("exact:abcd", {
    "deck": "exact", "author": "planner", "card_schema_version": "zonecard-v2",
    "entropy": 128, "stability": 128, "locality": 128,
    "fingerprint": 0xABCD,
    "extensions": {"decision": "custom-full", "action": "deep_decode", "note": "LLM override"},
})
print(f"  ok={r.get('ok')}")
print()

# ── Step 6: Pull cards (team B syncs) ──
print("[TEAM B] Pull all cards since v0:")
pull = mcp.pull_cards(0)
print(f"  {pull['count']} cards pulled (latest keys: {list(pull['cards'].keys())[:3]}...)")
print()

# ── Step 7: Stats ──
s = mcp.stats()
print(f"[MONITOR] stats: cards={s['cards']} events={s['events']} "
      f"decks={s['decks']} rules={s['n_rules']} "
      f"hits={s['hits']} misses={s['misses']}")

# ── Step 8: Prompt export for AI handoff ──
print("\n" + "=" * 65)
print("PROMPT EXPORT (for external AI, no MCP access)")
print("=" * 65)
lines = []
lines.append("=== MCP STATE (read-only snapshot) ===")
lines.append(f"Version: {s.get('global_version','?')} | Rules: {s['n_rules']} | Cards: {s['cards']} | Events: {s['events']}")
lines.append(f"Hit rate: {s.get('hit_rate',0)}% | Decks: {s['decks']}")
lines.append("")
for key in ["geom_pipeline:z0", "geom_pipeline:z1", "geom_pipeline:z2",
            "geom_pipeline:z3", "geom_pipeline:z4", "geom_pipeline:z5"]:
    card = pull["cards"].get(key, {})
    author = card.get("author", "?")
    ct = card.get("extensions", {}).get("card_type", 0)
    route = mcp.resolve_route(ct, card.get("entropy",128), card.get("locality",128), card.get("stability",128))
    lines.append(f"  {key} ({author}) ent={card.get('entropy','?')} -> {route['decision']}")
lines.append("")
lines.append("Fusion pairs:")
for model in ["qwen25coder", "smollm2", "deepseek"]:
    peer = mcp.get_fusion_peer(f"geom_pipeline:z{indices[model]}")
    if peer:
        lines.append(f"  {model} <-> {peer['peer']} (sim={peer['similarity']:.2f})")
lines.append("")
lines.append("Exact cache entries: 1 (hash 0xABCD)")
for line in lines:
    print(line)
print(f"\nExport size: {sum(len(l)+1 for l in lines)} chars")

# ── Step 9: Verify cross-model same-prompt ──
print("\n" + "=" * 65)
print("SINGLE PROMPT DEMO — 2 models routed in parallel")
print("=" * 65)
r0 = mcp.resolve_route(2, 234, 128, 0)  # qwen25coder high entropy
r1 = mcp.resolve_route(2, 230, 120, 5)  # smollm2 high entropy
r2 = mcp.resolve_route(0, 45, 200, 180) # qwen25coder low entropy

prompt = f"""[SYSTEM] You have access to a geometry-routed LLM system with 3 models.

ROUTING TABLE:
- qwen25coder (coding expert) at high entropy -> {r0['decision']}
- smollm2 (lightweight) at high entropy -> {r1['decision']}
- qwen25coder (coding expert) at low entropy -> {r2['decision']}

CURRENT TASK:
Given a high-entropy, stability-0 query about kernel optimization:
1. Route to qwen25coder (ent=234) -> action: {r0['action']}
2. Route to smollm2 (ent=230) -> action: {r1['action']}
3. If both give {r0['decision']}, use fusion peer qwen25coder <-> smollm2

Which model should handle the kernel question?
"""
print(prompt)
print(f"[ANSWER] qwen25coder — high-entropy coding task matches its zone 0 profile")

proc.terminate(); proc.wait()
shutil.rmtree(sd)
