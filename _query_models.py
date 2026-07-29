#!/usr/bin/env python3
"""Query opencode-zen for available free models."""
import urllib.request, json, os

ENV_FILE = "I:/hermes/.env"
key = ""

with open(ENV_FILE) as f:
    for line in f:
        line = line.strip()
        if line.startswith("OPENCODE_ZEN_API_KEY") and "=" in line:
            key = line.split("=", 1)[1].strip().strip("'\"")

if not key:
    key = os.environ.get("OPENCODE_ZEN_API_KEY", "")

if not key:
    print("No API key found")
    exit(1)

req = urllib.request.Request(
    "https://opencode.ai/zen/v1/models",
    headers={"Authorization": f"Bearer {key}"}
)
resp = urllib.request.urlopen(req, timeout=15)
data = json.loads(resp.read())

if isinstance(data, dict):
    models = data.get("data", [])
elif isinstance(data, list):
    models = data
else:
    models = []

print(f"{len(models)} models available:")
for m in models:
    mid = m.get("id", m.get("name", str(m)[:60]))
    free = "  "
    if isinstance(mid, str) and "free" in mid.lower():
        free = "🔓 "
    print(f"  {free}{mid}")
