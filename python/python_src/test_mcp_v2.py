"""Test MCP v2 server end-to-end."""
import sys, subprocess, time, os, shutil
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from mcp_client import MCPClient

state_dir = Path(__file__).parent / "mcp_test_v2"
state_dir.mkdir(exist_ok=True)
os.environ["MCP_STATE_DIR"] = str(state_dir)

proc = subprocess.Popen(
    [sys.executable, "mcp_server.py"],
    cwd=str(Path(__file__).parent),
    env={**os.environ, "MCP_PORT": "18767"},
    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)

if proc.poll():
    print("FAIL:", proc.stderr.read().decode()[:400])
    shutil.rmtree(state_dir)
    sys.exit(1)

mcp = MCPClient("http://localhost:18767")
ok = True

r = mcp.resolve_route(card_type=2, entropy=234, locality=128, stability=0)
print("resolve_route:", r.get("decision"), r.get("cache"))
ok &= r.get("decision") == "crash-plan"

r = mcp.resolve_sequence([1, 0, 1])
print("resolve_sequence [1,0,1]:", len(r.get("decisions",[])), "decisions")
ok &= len(r.get("decisions",[])) == 3

info = mcp.get_zone_info(0)
print("zone 0:", info)
ok &= info is not None and info["zone"] == 0

dok = mcp.register_deck("testdeck", "zonecard-v2", "route-v46", "test")
print("register_deck:", dok)
ok &= dok

push = mcp.push_card("testdeck:zone0", {
    "deck": "testdeck", "author": "tester", "card_schema_version": "zonecard-v2",
    "entropy": 180, "stability": 50, "locality": 200, "fingerprint": 12345,
    "extensions": {"card_type": 2, "pattern": 0x4a, "zone_id": 0},
})
print("push_card:", push.get("version"))
ok &= push.get("ok") is True

pull = mcp.pull_cards(since_version=0, deck="testdeck")
print("pull_cards:", pull.get("count"))
ok &= pull.get("count",0) >= 1

push2 = mcp.push_card("testdeck:zone1", {
    "deck": "testdeck", "author": "user2", "card_schema_version": "zonecard-v2",
    "entropy": 170, "stability": 60, "locality": 190, "fingerprint": 67890,
    "extensions": {"card_type": 2},
})
print("push2:", push2.get("version"))

peer = mcp.get_fusion_peer("testdeck:zone0")
print("fusion_peer:", peer)
ok &= peer is not None

s = mcp.stats()
print("stats:", "cards", s.get("cards"), "decks", s.get("decks"), "rules", s.get("n_rules"))

print("\nALL PASS:", ok)
proc.terminate()
proc.wait()
shutil.rmtree(state_dir)
sys.exit(0 if ok else 1)
