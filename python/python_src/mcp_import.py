"""Import AI-generated zone cards / decisions back into MCP server.
ใช้ pipe ผลลัพธ์จาก AI เข้าไป store:

  python mcp_import.py --card '{"zone":0,"shape":"I","model_key":"newmodel","card_type":2,"entropy":180,"locality":90,"stability":50}'
  python mcp_import.py --decision '{"hash_val":1001,"decision":"skip","action":"noop","reason":"stable sparse, no action"}'
  python mcp_import.py --bulk cards.json    # JSON array ของ cards
"""

import json, sys, os
from mcp_client import MCPClient

mcp = MCPClient(os.environ.get("MCP_URL", "http://localhost:8765"))

def store_card(d):
    ok = mcp.store_zone_card(
        zone=d["zone"], shape=d["shape"], model_key=d["model_key"],
        card_type=d["card_type"], entropy=d["entropy"],
        locality=d["locality"], stability=d["stability"],
        pattern=d.get("pattern", 0), hash_val=d.get("hash_val", 0),
    )
    print(f"  card {d['zone']}:{d['shape']}:{d['model_key']} → {'OK' if ok else 'FAIL'}")

def store_decision(d):
    ok = mcp.store_decision(
        hash_val=d["hash_val"], decision=d["decision"],
        action=d["action"], reason=d.get("reason", ""),
        zone=d.get("zone", -1), shape=d.get("shape", ""),
        model_key=d.get("model_key", ""),
    )
    print(f"  decision #{d['hash_val']} → {'OK' if ok else 'FAIL'}")

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", help="JSON string: single zone card")
    ap.add_argument("--decision", help="JSON string: single decision")
    ap.add_argument("--bulk", help="JSON file: array of {type:'card'|'decision', data:{...}}")
    args = ap.parse_args()

    if args.card:
        store_card(json.loads(args.card))
    if args.decision:
        store_decision(json.loads(args.decision))
    if args.bulk:
        with open(args.bulk) as f:
            items = json.load(f)
        for item in items:
            if item["type"] == "card":
                store_card(item["data"])
            elif item["type"] == "decision":
                store_decision(item["data"])
    if not any([args.card, args.decision, args.bulk]):
        print("อ่าน JSON จาก stdin...")
        for line in sys.stdin:
            line = line.strip()
            if not line: continue
            try:
                item = json.loads(line)
                if item.get("type") == "card":
                    store_card(item["data"])
                elif item.get("type") == "decision":
                    store_decision(item["data"])
                else:
                    print(f"  ? unknown type: {item.get('type')}")
            except json.JSONDecodeError:
                print(f"  ? invalid json: {line[:60]}...")
