"""Export MCP v2 state as compact text block for prompt injection."""
import json, os, sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))
from mcp_client import MCPClient

mcp = MCPClient("http://localhost:8765")
stats = mcp.stats()
zone_map = json.loads((SCRIPT_DIR / "mcp_zone_map.json").read_text()) if (SCRIPT_DIR / "mcp_zone_map.json").exists() else {}

state_dir = Path(os.environ.get("MCP_STATE_DIR", str(SCRIPT_DIR / "mcp_state_v2")))

lines = []
lines.append("[MCP STATE]")
lines.append(f"version={stats.get('global_version',0)}  cards={stats.get('cards',0)}  events={stats.get('events',0)}")
lines.append(f"rules={stats.get('n_rules',0)}  hits={stats.get('hits',0)}  misses={stats.get('misses',0)}  hit_rate={stats.get('hit_rate',0)}%")
lines.append(f"decks={stats.get('decks',[])}")
lines.append("")

if zone_map.get("zones"):
    lines.append("--- ZONE MAP ---")
    for z in zone_map["zones"]:
        lines.append(f"  zone {z['zone']:>2}: {z['shape']}/{z['model_key']} — {z['note']}")

cards_file = state_dir / "cards.json"
if cards_file.exists():
    cards = json.loads(cards_file.read_text())
    if cards:
        lines.append("")
        lines.append("--- CARDS ---")
        for key, c in sorted(cards.items()):
            ext = c.get("extensions", {})
            lines.append(f"  {key}: deck={c.get('deck','?')} v{c.get('version',0)} ent={c.get('entropy',128)} st={c.get('stability',128)} loc={c.get('locality',128)} fp={c.get('fingerprint',0)}")

fusion_file = state_dir / "fusion_table.json"
if fusion_file.exists():
    fusion = json.loads(fusion_file.read_text())
    if fusion:
        lines.append("")
        lines.append("--- FUSION TABLE ---")
        for fk, entries in sorted(fusion.items()):
            peers = ", ".join(f"{k}(sim={v.get('similarity',0):.2f})" for k, v in sorted(entries.items()))
            lines.append(f"  {fk}: {peers}")

exact_file = state_dir / "exact_cache.json"
if exact_file.exists():
    exact = json.loads(exact_file.read_text())
    if exact:
        lines.append("")
        lines.append("--- EXACT CACHE ---")
        for hk, d in sorted(exact.items()):
            lines.append(f"  hash#{hk}: {d.get('decision','?')}/{d.get('action','?')}")

if not cards_file.exists() and not fusion_file.exists() and not exact_file.exists():
    lines.append("(empty — no data stored yet)")

text = "\n".join(lines)
print(text)
print(f"\n--- {len(text)} chars ---")
