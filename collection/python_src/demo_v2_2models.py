"""Live demo: 2 models, multiple zones, 1 prompt"""
import sys, subprocess, time, os, shutil, json
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from mcp_client import MCPClient

state_dir = Path(__file__).parent / "mcp_demo_v2"
state_dir.mkdir(exist_ok=True)
os.environ["MCP_STATE_DIR"] = str(state_dir)

proc = subprocess.Popen(
    [sys.executable, "mcp_server.py"],
    cwd=str(Path(__file__).parent),
    env={**os.environ, "MCP_PORT": "18769"},
    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)

mcp = MCPClient("http://localhost:18769")

# Register 2 decks
mcp.register_deck("fg_geom", "zonecard-v2", "route-v46",
                   "FGLS geometry-routed models")
mcp.register_deck("cloud_intent", "intentcard-v1", "route-v46",
                   "Cloud intent routing")

# ── Push 4 cards: 2 models × 2 zones each ──
cards_data = [
    # Model A: qwen25coder (coding expert)
    ("fg_geom:z0", {"deck":"fg_geom","author":"qwen25coder","card_schema_version":"zonecard-v2",
     "entropy":234,"stability":0,"locality":128,"fingerprint":0xA1B2,
     "extensions":{"card_type":2,"pattern":0x410c,"zone_id":0,"model_dim":1536}}),
    ("fg_geom:z2", {"deck":"fg_geom","author":"qwen25coder","card_schema_version":"zonecard-v2",
     "entropy":45,"stability":180,"locality":200,"fingerprint":0xC3D4,
     "extensions":{"card_type":0,"pattern":0x0000,"zone_id":2}}),
    # Model B: smollm2 (lightweight)
    ("fg_geom:z6", {"deck":"fg_geom","author":"smollm2","card_schema_version":"zonecard-v2",
     "entropy":230,"stability":5,"locality":120,"fingerprint":0xE5F6,
     "extensions":{"card_type":2,"pattern":0x4008,"zone_id":6,"model_dim":960}}),
    ("fg_geom:z8", {"deck":"fg_geom","author":"smollm2","card_schema_version":"zonecard-v2",
     "entropy":88,"stability":170,"locality":80,"fingerprint":0x7A8B,
     "extensions":{"card_type":1,"pattern":0x0000,"zone_id":8}}),
]
for key, card in cards_data:
    r = mcp.push_card(key, card)
    print(f"push {key} → v{r.get('version')}")

print("\n─" * 30)

# ── Resolve 4 routes ──
print("ROUTE RESOLUTIONS:")
for key, card in cards_data:
    ct = card["extensions"].get("card_type", 0)
    route = mcp.resolve_route(ct, card["entropy"], card["locality"], card["stability"])
    print(f"  {key:15s} ct={ct} ent={card['entropy']:3d} loc={card['locality']:3d} st={card['stability']:3d}")
    print(f"  {'':15s} → {route['decision']:20s} / {route['action']:20s} [{route['cache']}]")

# ── Fusion peers ──
print("\nFUSION PEERS:")
for peer_key in ["fg_geom:z0", "fg_geom:z6"]:
    peer = mcp.get_fusion_peer(peer_key)
    if peer:
        print(f"  {peer_key:15s} → peer={peer['peer']} sim={peer['similarity']:.2f}")
    else:
        print(f"  {peer_key:15s} → no peer (below 0.75)")

# ── Cross-deck adapt: fg_geom → cloud_intent ──
print("\nCROSS-DECK ADAPT:")
# Adapt qwen25coder's high-entropy card to cloud intent schema
adapted = mcp.adapt_card(cards_data[0][1], "intentcard-v1")
if adapted:
    print(f"  zonecard-v2 → intentcard-v1:")
    print(f"    entropy={adapted['entropy']} stability={adapted['stability']}")
    print(f"    route_class={adapted['extensions']['route_class']}")
    print(f"    confidence={adapted['extensions']['confidence']}")

# ── 1 Prompt: resolve both models simultaneously ──
print("\n" + "=" * 60)
print("SINGLE PROMPT — 2 models routed simultaneously")
print("=" * 60)

r0 = mcp.resolve_route(2, 234, 128, 0)
r1 = mcp.resolve_route(2, 230, 120, 5)
p0 = mcp.get_fusion_peer("fg_geom:z0")
p1 = mcp.get_fusion_peer("fg_geom:z6")

prompt = f"""[MCP STATE]
qwen25coder at fg_geom:z0 -> {r0['decision']}/{r0['action']}
smollm2 at fg_geom:z6 -> {r1['decision']}/{r1['action']}
"""
for c in cards_data:
    r = mcp.resolve_route(c[1]["extensions"].get("card_type",0), c[1]["entropy"], c[1]["locality"], c[1]["stability"])
    prompt += f"  [{c[1]['author']}] zone={c[1]['extensions'].get('zone_id','?')} ent={c[1]['entropy']} -> {r['decision']}\n"
prompt += f"""
Cross-model fusion:
  qwen25coder at z0 <-> {p0['peer']} (sim={p0['similarity']})
  smollm2 at z6 <-> {p1['peer']} (sim={p1['similarity']})

Zone map: zone 0 = qwen25coder coding expert, zone 6 = smollm2 lightweight
"""
print(f"Prompt length: {len(prompt)} chars")
print(prompt[:400] + "...")

# ── Prompt export ──
print("── PROMPT EXPORT ──")
lines_out = []
stats = mcp.stats()
lines_out.append("[MCP STATE]")
lines_out.append(f"version={stats.get('global_version',0)}  cards={stats.get('cards',0)}  events={stats.get('events',0)}")
lines_out.append(f"rules={stats.get('n_rules',0)}  hits={stats.get('hits',0)}  misses={stats.get('misses',0)}  hit_rate={stats.get('hit_rate',0)}%")
lines_out.append(f"decks={stats.get('decks',[])}")
lines_out.append("")
for key, card in cards_data:
    r = mcp.resolve_route(card["extensions"].get("card_type",0), card["entropy"], card["locality"], card["stability"])
    lines_out.append(f"  {key}: ent={card['entropy']} loc={card['locality']} → {r['decision']}/{r['action']}")
for line in lines_out:
    print(line)

proc.terminate()
proc.wait()
shutil.rmtree(state_dir)
