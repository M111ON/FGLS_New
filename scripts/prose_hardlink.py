#!/usr/bin/env python3
"""
prose_hardlink.py — Verify and restore prose doc ↔ source file links

System:
  - prose.json = registry of files that NEED documentation (113 files)
  - docs/prose/<module>/<name>.md = generated documentation
  - manifest.json = tracks source↔doc relationships + verification status
  - prose_state.json = tracks which files have been documented

Usage:
  python scripts/prose_hardlink.py verify    # Check registered files have docs
  python scripts/prose_hardlink.py sync      # Sync manifest from prose.json
  python scripts/prose_hardlink.py scan      # Find new .h/.c files not in prose.json
  python scripts/prose_hardlink.py stats     # Show statistics
"""

import json
import os
import sys
from pathlib import Path
from datetime import datetime

PROJECT_ROOT = Path("I:/FGLS_new")
DOCS_DIR = PROJECT_ROOT / "docs" / "prose"
MANIFEST = DOCS_DIR / "manifest.json"
PROSE_JSON = PROJECT_ROOT / "runner" / "prose.json"
PROSE_STATE = PROJECT_ROOT / "runner" / "prose_state.json"
RUNNER_ROOT = PROJECT_ROOT / "runner"

# Directories to scan for new files (top-level modules only)
SCAN_DIRS = [
    "pogls_core", "pogls_gguf", "pogls_geo", "pogls_dram",
    "pogls_bermuda", "pogls_kv", "pogls_geopixel",
    "pogls_hilbert_container", "pogls_tools",
    "geopixel", "collection",
]


def load_json(path):
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    return None


def save_json(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def resolve_source(filepath):
    """Check if source file exists in runner/ or project root."""
    for base in [RUNNER_ROOT, PROJECT_ROOT]:
        p = base / filepath
        if p.exists():
            return p
    return None


def doc_path_for(filepath, module_name=None):
    """Compute expected doc path for a source file.
    Uses module_name from prose.json (short name like 'core', not 'pogls_core').
    """
    if module_name is None:
        # Fallback: strip pogls_ prefix from first path component
        p = Path(filepath)
        raw_mod = p.parts[0] if len(p.parts) > 1 else "misc"
        module_name = raw_mod.removeprefix("pogls_")
    return DOCS_DIR / module_name / (Path(filepath).stem + ".md")


def sync_manifest():
    """Build/refresh manifest from prose.json + prose_state.json."""
    prose = load_json(PROSE_JSON) or {"modules": []}
    state = load_json(PROSE_STATE) or {"documented": [], "statuses": {}}
    documented = set(state.get("documented", []))
    statuses = state.get("statuses", {})

    entries = {}
    for m in prose.get("modules", []):
        module_name = m["name"]
        for f in m.get("files", []):
            src_path = f["path"]
            src = resolve_source(src_path)
            doc_p = doc_path_for(src_path, module_name)

            entry = {
                "source": src_path,
                "doc": str(doc_p.relative_to(PROJECT_ROOT)),
                "module": module_name,
                "source_exists": src is not None,
                "doc_exists": doc_p.exists(),
                "documented": src_path in documented,
                "source_status": statuses.get(src_path, "unknown"),
                "last_verified": datetime.now().isoformat(),
            }

            # Determine link status
            if not entry["source_exists"]:
                entry["link_status"] = "source_missing"
            elif entry["documented"] and entry["doc_exists"]:
                entry["link_status"] = "ok"
            elif entry["documented"] and not entry["doc_exists"]:
                entry["link_status"] = "doc_missing"
            else:
                entry["link_status"] = "pending"

            entries[src_path] = entry

    manifest = {
        "version": 2,
        "updated": datetime.now().isoformat(),
        "total_registered": len(entries),
        "entries": entries,
    }
    save_json(MANIFEST, manifest)
    return manifest


def verify_links():
    """Verify all registered files have valid docs."""
    manifest = load_json(MANIFEST)
    if not manifest:
        manifest = sync_manifest()

    results = {"ok": 0, "doc_missing": 0, "source_missing": 0, "pending": 0}
    issues = []

    for path, entry in manifest.get("entries", {}).items():
        # Re-check
        src = resolve_source(path)
        doc_p = Path(PROJECT_ROOT) / entry["doc"]

        entry["source_exists"] = src is not None
        entry["doc_exists"] = doc_p.exists()
        entry["last_verified"] = datetime.now().isoformat()

        if not src:
            entry["link_status"] = "source_missing"
            results["source_missing"] += 1
            issues.append(f"❌ SOURCE MISSING: {path}")
        elif not doc_p.exists():
            entry["link_status"] = "doc_missing"
            results["doc_missing"] += 1
            issues.append(f"📄 DOC MISSING: {path} → {entry['doc']}")
        else:
            entry["link_status"] = "ok"
            results["ok"] += 1

    manifest["updated"] = datetime.now().isoformat()
    save_json(MANIFEST, manifest)

    return results, issues


def scan_new_files():
    """Find .h/.c files in source dirs that aren't in prose.json."""
    prose = load_json(PROSE_JSON) or {"modules": []}
    registered = set()
    for m in prose.get("modules", []):
        for f in m.get("files", []):
            registered.add(f["path"])

    new_files = []
    for scan_dir in SCAN_DIRS:
        dir_path = PROJECT_ROOT / scan_dir
        if not dir_path.exists():
            continue
        for ext in ("*.h", "*.c"):
            for f in dir_path.rglob(ext):
                rel = f.relative_to(PROJECT_ROOT)
                parts = rel.parts
                # Skip deprecated, build, node_modules
                if any(skip in parts for skip in ("deprecated", "build", "node_modules", ".git", ".venv")):
                    continue
                if str(rel) not in registered:
                    new_files.append({
                        "path": str(rel),
                        "module": parts[0],
                        "size": f.stat().st_size,
                    })

    return new_files


def show_stats():
    """Show statistics from manifest + prose state."""
    manifest = load_json(MANIFEST)
    state = load_json(PROSE_STATE) or {"documented": [], "statuses": {}}

    if not manifest:
        print("No manifest found. Run: python scripts/prose_hardlink.py sync")
        return

    entries = manifest.get("entries", {})
    total = len(entries)

    by_status = {}
    by_module = {}
    for path, entry in entries.items():
        ls = entry.get("link_status", "unknown")
        by_status[ls] = by_status.get(ls, 0) + 1
        mod = entry.get("module", "unknown")
        by_module[mod] = by_module.get(mod, 0) + 1

    documented = len(state.get("documented", []))
    statuses = state.get("statuses", {})
    n_active = sum(1 for v in statuses.values() if v == "active")
    n_stale = sum(1 for v in statuses.values() if v == "stale")
    n_deprecated = sum(1 for v in statuses.values() if v == "deprecated")

    print(f"=== Prose Hardlink Stats ===")
    print(f"Registered files: {total}")
    print(f"Documented: {documented}")
    print(f"\nLink status:")
    for s, c in sorted(by_status.items()):
        emoji = {"ok": "✅", "doc_missing": "📄", "source_missing": "❌", "pending": "⏳"}.get(s, "?")
        print(f"  {emoji} {s}: {c}")
    print(f"\nDoc statuses: {n_active} active, {n_stale} stale, {n_deprecated} deprecated")
    print(f"\nBy module:")
    for m, c in sorted(by_module.items()):
        print(f"  {m}: {c}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    action = sys.argv[1].lower()

    if action == "sync":
        manifest = sync_manifest()
        total = manifest["total_registered"]
        print(f"✅ Manifest synced: {total} registered files")
        return 0

    elif action == "verify":
        results, issues = verify_links()
        print(f"=== Verify Results ===")
        print(f"✅ OK: {results['ok']}")
        print(f"📄 Doc missing: {results['doc_missing']}")
        print(f"❌ Source missing: {results['source_missing']}")
        if issues:
            print(f"\nIssues ({len(issues)}):")
            for issue in issues[:30]:
                print(f"  {issue}")
            if len(issues) > 30:
                print(f"  ... +{len(issues)-30} more")
        return 0

    elif action == "scan":
        new_files = scan_new_files()
        print(f"Found {len(new_files)} unregistered files:")
        # Group by module
        by_module = {}
        for f in new_files:
            by_module.setdefault(f["module"], []).append(f)
        for mod, files in sorted(by_module.items()):
            print(f"\n  {mod} ({len(files)} new):")
            for f in files[:5]:
                print(f"    {f['path']}")
            if len(files) > 5:
                print(f"    ... +{len(files)-5} more")
        return 0

    elif action == "stats":
        show_stats()
        return 0

    else:
        print(f"Unknown action: {action}")
        print(__doc__)
        return 1


if __name__ == "__main__":
    sys.exit(main())
