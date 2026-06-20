#!/usr/bin/env python3
"""
inbox_mcp_server.py — Smart Inbox Manager MCP Server

Auto-places files, manages version vault, resolves conflicts,
tracks context across sessions. Raw folder-based (no git).

Connect from opencode:
  mcp {
    "inbox-manager": {
      "type": "local",
      "command": ["python", "I:/FGLS_new/inbox_mcp_server.py"],
      "enabled": true
    }
  }

Tools:
  - scan:              rescan workspace, rebuild index
  - put_zip:           ingest ZIP → auto-place files
  - put_file:          ingest single file → auto-place
  - get_context:       context summary for LLM (skeleton|active|full)
  - get_project_index: cached project index (no re-scan) — use in new sessions
  - list_incoming:     pending files waiting apply
  - apply_all:         apply all safe incoming
  - resolve:           resolve specific conflict
  - rollback:          restore vaulted version
  - vault_list:        browse vault
  - dep_graph:         dependency graph for a file
  - auto_place:        suggest where a file should go (dry-run)

Cross-session state persists in .inbox_state.json + .inbox_index.md (human-readable).
New sessions: call get_project_index(skeleton) for instant cached overview.
"""

import os, re, struct, sys, json, hashlib, zipfile, io, time
from datetime import datetime
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

# ── globals ──────────────────────────────────────────────────────────
WORKSPACE = Path(__file__).parent.resolve()
_DRIVE_ROOT = Path(Path(__file__).anchor)
VAULT_DIR = Path(os.environ.get("INBOX_VAULT_DIR", str(_DRIVE_ROOT / ".vault"))).resolve()
STATE_FILE = VAULT_DIR / ".inbox_state.json"
ZIP_ARCHIVE = VAULT_DIR / "zips"

MAX_VAULT_PER_FILE = 10
VAULT_SIZE_LIMIT = 3 * 1024 * 1024  # 3 MB max per file
VAULT_EXTS = {".c", ".h", ".py", ".md", ".json", ".txt"}
TEXT_EXTS = {".h", ".c", ".cpp", ".cc", ".py", ".js", ".ts", ".json", ".yaml", ".yml", ".md", ".txt", ".rs", ".go", ".java", ".toml", ".ini", ".cfg"}

DEP_PATTERNS = [
    (re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE), "c"),
    (re.compile(r'^\s*(?:import|from)\s+([\w.]+)', re.MULTILINE), "py"),
    (re.compile(r'(?:import|require)\s*\(?[\'"]([^\'"]+)[\'"]\)?', re.MULTILINE), "js"),
    (re.compile(r'^\s*#include\s+<([^>]+)>', re.MULTILINE), "c_std"),
    (re.compile(r'^use\s+(\w+(?:::\w+)*)', re.MULTILINE), "rs"),
    (re.compile(r'^import\s+(?:(\w+\.)*(\w+))', re.MULTILINE), "go"),
]

VER_RE = re.compile(r"^(.+?)_v(\d+(?:[._]\d+)*)(\.[a-z0-9]+)$", re.IGNORECASE)

# ── FastMCP app ──────────────────────────────────────────────────────
mcp = FastMCP("Inbox Manager")

# ── state helpers ────────────────────────────────────────────────────
def _load_state() -> dict:
    if STATE_FILE.exists():
        try: return json.loads(STATE_FILE.read_text("utf-8"))
        except: pass
    return {"roots": [], "file_index": {}, "vault": {}, "incoming": [],
            "project_name": WORKSPACE.name,
            "last_scan_ts": 0.0,
            "folder_groups": {}}

def _save_state(state: dict):
    STATE_FILE.write_text(json.dumps(state, indent=2, default=str), "utf-8")

def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]

def _parse_ver(name: str) -> dict | None:
    m = VER_RE.match(name)
    if not m: return None
    parts = [int(x) for x in re.split(r"[._]", m.group(2))]
    return {"base": m.group(1), "ver": m.group(2), "ver_parts": parts, "ext": m.group(3)}

def _ver_gt(a: list[int], b: list[int]) -> bool:
    for i in range(max(len(a), len(b))):
        ai = a[i] if i < len(a) else 0
        bi = b[i] if i < len(b) else 0
        if ai != bi: return ai > bi
    return False

# ── file scanning ────────────────────────────────────────────────────
def _walk(root: Path, prefix: str = "", root_idx: int = 0) -> dict[str, dict]:
    out = {}
    if not root.exists(): return out
    for entry in sorted(root.iterdir(), key=lambda p: p.name):
        name = entry.name
        if name.startswith(".") or name == "__pycache__" or name == "node_modules": continue
        rel = f"{prefix}/{name}" if prefix else name
        if entry.is_dir():
            out.update(_walk(entry, rel, root_idx))
        else:
            pv = _parse_ver(name)
            fp = entry
            try:
                st = fp.stat()
                h = _sha256(fp.read_bytes()) if st.st_size < 10_000_000 else ""
            except:
                h = ""
            out[rel] = {
                "name": name,
                "size": entry.stat().st_size if entry.exists() else 0,
                "mtime": entry.stat().st_mtime if entry.exists() else 0,
                "hash": h,
                "parsed": pv,
                "root_idx": root_idx,
                "rel": rel,
            }
    return out

def _scan_imports(text: str) -> set[str]:
    imports = set()
    for pat, lang in DEP_PATTERNS:
        for m in pat.finditer(text):
            raw = m.group(1)
            fname = raw.split("/")[-1] if "/" in raw else raw
            imports.add(fname)
    return imports

def _build_dep_graph(state: dict):
    graph = {}
    for rel, fi in state["file_index"].items():
        name = fi["name"]
        if name not in graph:
            graph[name] = {"imports": set(), "imported_by": set()}
        ext = Path(name).suffix.lower()
        if ext and ext in {e.lower() for e in TEXT_EXTS}:
            try:
                p = WORKSPACE / rel
                if p.exists() and p.stat().st_size < 500_000:
                    deps = _scan_imports(p.read_text("utf-8", errors="replace"))
                    for d in deps:
                        if d != name:
                            graph[name]["imports"].add(d)
                            if d not in graph:
                                graph[d] = {"imports": set(), "imported_by": set()}
                            graph[d]["imported_by"].add(name)
            except: pass
    return graph


# ── index summary helpers ────────────────────────────────────────────
def _compute_folder_groups(file_index: dict) -> dict:
    groups = {}
    for rel in file_index:
        top = rel.split("/")[0]
        groups[top] = groups.get(top, 0) + 1
    return dict(sorted(groups.items(), key=lambda x: -x[1]))

def _write_index_summary(state: dict):
    """Write .inbox_index.md so new sessions can read cached data directly."""
    ts = state.get("last_scan_ts", 0)
    when = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S") if ts else "never"
    total = len(state.get("file_index", {}))
    groups = state.get("folder_groups", {})
    vaulted = sum(len(v) for v in state.get("vault", {}).values())
    dg = state.get("_dep_graph", {})
    ws_names = {fi["name"] for fi in state.get("file_index", {}).values()}
    hubs = sorted(
        [(n, len(g.get("imports", [])), len(g.get("imported_by", [])))
         for n, g in dg.items() if n in ws_names],
        key=lambda x: x[1] + x[2] * 2, reverse=True
    )[:5]
    lines = [
        f"# {state.get('project_name', '?')} — Index",
        f"Last scan: {when}  |  Files: {total}  |  Vault: {vaulted}",
        "",
        "## Folder groups",
    ]
    for folder, count in groups.items():
        lines.append(f"  {folder}/  ({count} files)")
    if hubs:
        lines.extend(["", "## Top hubs"])
        for h in hubs:
            lines.append(f"  {h[0]}  in:{h[1]}  by:{h[2]}")
    # incoming pending
    pending = [i for i in state.get("incoming", []) if i["status"] in ("new", "update", "conflict", "downgrade")]
    if pending:
        lines.extend(["", "## Pending incoming"])
        for p in pending:
            lines.append(f"  [{p['status']}] {p['name']}")
    lines.append("")
    (WORKSPACE / ".inbox_index.md").write_text("\n".join(lines), "utf-8")


# ═════════════════════════════════════════════════════════════════════
# MCP Tools
# ═════════════════════════════════════════════════════════════════════

@mcp.tool()
def scan(roots: list[str] | None = None) -> str:
    """Rescan workspace roots. Builds file index + dep graph."""
    state = _load_state()
    if roots:
        state["roots"] = roots
    if not state["roots"]:
        state["roots"] = ["."]
    state["file_index"] = {}
    for ri, root_rel in enumerate(state["roots"]):
        root_path = WORKSPACE / root_rel
        if root_path.exists():
            state["file_index"].update(_walk(root_path, "", ri))
    # build dep graph
    graph = _build_dep_graph(state)
    state["_dep_graph"] = {k: {"imports": list(v["imports"]), "imported_by": list(v["imported_by"])} for k, v in graph.items()}
    # save metadata for cross-session access
    state["project_name"] = WORKSPACE.name
    state["last_scan_ts"] = time.time()
    state["folder_groups"] = _compute_folder_groups(state["file_index"])
    _save_state(state)
    _write_index_summary(state)
    total = len(state["file_index"])
    vaulted = sum(len(v) for v in state.get("vault", {}).values())
    missing = sum(1 for n in state.get("_dep_graph", {}) if n not in {fi["name"] for fi in state["file_index"].values()} and any(state["_dep_graph"][n].get("imported_by")))
    return f"ok  scanned {total} files, {vaulted} vaulted, {missing} missing deps"


@mcp.tool()
def put_zip(zip_path: str | None = None, zip_b64: str | None = None, dry_run: bool = False) -> str:
    """Ingest ZIP → auto-place files with versioning. Provide zip_path (local) or zip_b64 (base64)."""
    state = _load_state()
    if zip_path:
        zp = Path(zip_path)
        if not zp.exists(): return f"err  not found: {zip_path}"
        raw = zp.read_bytes()
        fname = zp.name
    elif zip_b64:
        import base64
        raw = base64.b64decode(zip_b64)
        fname = "stream.zip"
    else:
        return "err  provide zip_path or zip_b64"
    try:
        zf = zipfile.ZipFile(io.BytesIO(raw))
    except Exception as e:
        return f"err  bad zip: {e}"
    entries = [e for e in zf.infolist() if not e.is_dir()]
    report = []
    for entry in entries:
        name = Path(entry.filename).name
        if not name: continue
        content = zf.read(entry)
        h = _sha256(content)
        pv = _parse_ver(name)
        # find match in existing files
        match_key = None
        match_fi = None
        for r, fi in state["file_index"].items():
            fp = fi.get("parsed") or _parse_ver(fi["name"])
            if fp and pv and fp["base"] == pv["base"] and fp["ext"] == pv["ext"]:
                match_key = r; match_fi = fi; break
        # determine status
        if match_fi:
            existing_h = match_fi.get("hash", "")
            if existing_h == h:
                status = "same"
            elif pv and match_fi.get("parsed"):
                if _ver_gt(pv["ver_parts"], match_fi["parsed"]["ver_parts"]):
                    status = "update"
                elif pv["ver"] == match_fi["parsed"]["ver"]:
                    status = "conflict"  # same ver different content
                else:
                    status = "downgrade"
            else:
                status = "conflict"
        else:
            status = "new"
        item = {
            "name": name, "zip": fname, "hash": h,
            "parsed": pv, "status": status,
            "match_key": match_key, "content_len": len(content),
            "content_b64": None if dry_run else _b64(content),
        }
        state.setdefault("incoming", []).append(item)
        report.append(f"  {status:10s} {name}" + (f" → {match_key}" if match_key else ""))
    # archive zip
    if not dry_run:
        ZIP_ARCHIVE.mkdir(parents=True, exist_ok=True)
        (ZIP_ARCHIVE / f"{Path(fname).stem}_{int(datetime.now().timestamp())}.zip").write_bytes(raw)
    _save_state(state)
    return "\n".join([f"ok  ingested {len(entries)} files from {fname}"] + report)


def _b64(data: bytes) -> str:
    import base64; return base64.b64encode(data).decode()


@mcp.tool()
def put_file(path: str, content_b64: str | None = None, dry_run: bool = False) -> str:
    """Ingest single file → auto-place with versioning."""
    state = _load_state()
    if content_b64:
        import base64
        raw = base64.b64decode(content_b64)
        fname = Path(path).name
    else:
        p = Path(path)
        if not p.exists(): return f"err  not found: {path}"
        raw = p.read_bytes()
        fname = p.name
    h = _sha256(raw)
    pv = _parse_ver(fname)
    match_key = None; match_fi = None
    for r, fi in state["file_index"].items():
        fp = fi.get("parsed") or _parse_ver(fi["name"])
        if fp and pv and fp["base"] == pv["base"] and fp["ext"] == pv["ext"]:
            match_key = r; match_fi = fi; break
    if match_fi:
        if match_fi.get("hash") == h:
            status = "same"
        elif pv and match_fi.get("parsed") and _ver_gt(pv["ver_parts"], match_fi["parsed"]["ver_parts"]):
            status = "update"
        else:
            status = "conflict"
    else:
        status = "new"
    item = {"name": fname, "zip": "direct", "hash": h, "parsed": pv, "status": status,
            "match_key": match_key, "content_len": len(raw), "content_b64": None if dry_run else _b64(raw)}
    state.setdefault("incoming", []).append(item)
    _save_state(state)
    return f"ok  {status}: {fname}" + (f" → {match_key}" if match_key else "")


@mcp.tool()
def list_incoming(status_filter: str | None = None) -> str:
    """List pending incoming files."""
    state = _load_state()
    items = state.get("incoming", [])
    if status_filter:
        items = [i for i in items if i["status"] == status_filter]
    if not items:
        return "no incoming files"
    lines = [f"{'status':10s} {'name':30s} {'size':>8s}  match"]
    lines.append("-" * 70)
    for i in items:
        lines.append(f"{i['status']:10s} {i['name']:30s} {i.get('content_len', 0):>8d}  {i.get('match_key', '-')}")
    return "\n".join(lines)


@mcp.tool()
def apply_all(statuses: list[str] | None = None) -> str:
    """Apply incoming files (default: new, update). Returns report."""
    state = _load_state()
    if statuses is None:
        statuses = ["new", "update"]
    to_apply = [i for i in state.get("incoming", []) if i["status"] in statuses and i.get("content_b64")]
    if not to_apply:
        return "nothing to apply"
    import base64
    report = []
    for item in to_apply:
        try:
            target_rel = _auto_place(item, state)
            target_path = WORKSPACE / target_rel
            target_path.parent.mkdir(parents=True, exist_ok=True)
            # vault existing if update
            if item["match_key"] and item["match_key"] in state["file_index"]:
                _vault_file(item["match_key"], state)
            raw = base64.b64decode(item["content_b64"])
            target_path.write_bytes(raw)
            report.append(f"  {item['name']} → {target_rel}")
            item["status"] = "done"
        except Exception as e:
            report.append(f"  {item['name']} FAIL: {e}")
    # keep non-done items
    state["incoming"] = [i for i in state["incoming"] if i["status"] != "done"]
    _save_state(state)
    # rescan
    scan(None)
    return "\n".join(["ok  applied " + str(len(to_apply)) + " files"] + report)


def _auto_place(item: dict, state: dict) -> str:
    """Smart placement: where should this file go."""
    name = item["name"]
    pv = item["parsed"]
    # 1. if match exists, use same path
    if item["match_key"]:
        return item["match_key"]
    # 2. infer from conventions
    ext = Path(name).suffix.lower()
    base = pv["base"] if pv else Path(name).stem
    # known conventions
    conv_map = {
        ".h": "include/", ".hpp": "include/",
        ".c": "src/", ".cpp": "src/", ".cc": "src/",
        ".py": "python_client/", ".rs": "src/",
        ".js": "src/", ".ts": "src/",
    }
    # look for sibling files with same base
    siblings = [r for r, fi in state["file_index"].items() if fi.get("parsed", {}).get("base") == base if isinstance(fi.get("parsed"), dict)]
    if siblings:
        return siblings[0]
    # dep-based: check if any existing file imports this
    dep_graph = state.get("_dep_graph", {})
    importers = dep_graph.get(name, {}).get("imported_by", [])
    if importers:
        importer_rels = [r for r, fi in state["file_index"].items() if fi["name"] in importers]
        if importer_rels:
            dir_part = str(Path(importer_rels[0]).parent)
            return f"{dir_part}/{name}" if dir_part != "." else name
    # convention-based
    suggested_dir = conv_map.get(ext, "")
    return f"{suggested_dir}{name}"


def _vault_file(rel: str, state: dict):
    """Snapshot existing file to vault before overwrite."""
    fi = state["file_index"].get(rel)
    if not fi: return
    ext = Path(rel).suffix.lower()
    if ext not in VAULT_EXTS: return
    src = WORKSPACE / rel
    if not src.exists(): return
    size = src.stat().st_size
    if size > VAULT_SIZE_LIMIT: return
    VAULT_DIR.mkdir(parents=True, exist_ok=True)
    content = src.read_bytes()
    h = _sha256(content)
    base = fi.get("parsed", {}).get("base") or fi["name"]
    vault_entry = {
        "ver": fi.get("parsed", {}).get("ver") or "0",
        "orig_rel": rel,
        "ts": int(datetime.now().timestamp()),
        "hash": h,
        "size": size,
    }
    state.setdefault("vault", {}).setdefault(base, []).append(vault_entry)
    state["vault"][base] = sorted(state["vault"][base], key=lambda x: x["ts"], reverse=True)[:MAX_VAULT_PER_FILE]
    # physical backup
    vault_name = f"{base}_v{vault_entry['ts']}.bak"
    (VAULT_DIR / vault_name).write_bytes(content)


def _rebuild_vault_index(state: dict) -> int:
    """Scan physical vault files and rebuild JSON vault index."""
    if not VAULT_DIR.exists(): return 0
    vault = state.setdefault("vault", {})
    prev = len(vault)
    for vf in sorted(VAULT_DIR.iterdir()):
        if vf.suffix not in (".bak", ".snap") or not vf.is_file(): continue
        name = vf.stem
        # parse base name + timestamp from patterns: base_vTS or base.v_TS
        ts = 0
        base = name
        if "_v" in name:
            base, ts_str = name.rsplit("_v", 1)
            try: ts = int(float(ts_str))
            except: ts = 0
        elif ".v_" in name:
            base, ts_str = name.rsplit(".v_", 1)
            try: ts = int(float(ts_str))
            except: ts = 0
        if not ts: continue
        vault.setdefault(base, [])
        # avoid duplicates
        existing = {e.get("ts") for e in vault[base]}
        if ts in existing: continue
        entry = {
            "ver": "0",
            "orig_rel": "",
            "ts": ts,
            "hash": _sha256(vf.read_bytes()) if vf.stat().st_size < 10_000_000 else "",
            "size": vf.stat().st_size,
        }
        vault[base].append(entry)
        vault[base] = sorted(vault[base], key=lambda x: x["ts"], reverse=True)[:MAX_VAULT_PER_FILE]
    return len(vault) - prev


@mcp.tool()
def vault_rebuild_index() -> str:
    """Scan physical .vault/ directory and rebuild JSON vault index."""
    state = _load_state()
    n = _rebuild_vault_index(state)
    _save_state(state)
    total = sum(len(v) for v in state.get("vault", {}).values())
    return f"ok  rebuilt vault index: {n} new entries, {total} total"


@mcp.tool()
def resolve(conflict_name: str, strategy: str = "incoming") -> str:
    """Resolve conflict for a file. strategy: incoming|keep|merge (future)."""
    state = _load_state()
    items = [i for i in state.get("incoming", []) if i["name"] == conflict_name and i["status"] in ("conflict", "downgrade")]
    if not items:
        return f"no conflict for {conflict_name}"
    item = items[0]
    if strategy == "incoming" and item.get("content_b64"):
        import base64
        target_rel = _auto_place(item, state)
        target_path = WORKSPACE / target_rel
        target_path.parent.mkdir(parents=True, exist_ok=True)
        if item["match_key"]:
            _vault_file(item["match_key"], state)
        target_path.write_bytes(base64.b64decode(item["content_b64"]))
        item["status"] = "done"
        state["incoming"] = [i for i in state["incoming"] if i["status"] != "done"]
        _save_state(state)
        scan(None)
        return f"ok  resolved {conflict_name} → {target_rel} (incoming)"
    elif strategy == "keep":
        item["status"] = "rejected"
        state["incoming"] = [i for i in state["incoming"] if i["status"] != "rejected"]
        _save_state(state)
        return f"ok  kept current for {conflict_name}"
    return f"err  unknown strategy: {strategy}"


@mcp.tool()
def rollback(base_name: str, version_index: int = 0) -> str:
    """Restore a vaulted version. version_index=0 means most recent."""
    state = _load_state()
    versions = state.get("vault", {}).get(base_name, [])
    if not versions: return f"no vault entries for {base_name}"
    if version_index < 0 or version_index >= len(versions):
        return f"version_index {version_index} out of range (0-{len(versions)-1})"
    v = versions[version_index]
    vault_name = f"{base_name}_v{v['ts']}.bak"
    vp = VAULT_DIR / vault_name
    if not vp.exists():
        vault_name_old = f"{base_name}_v{v['ts']}.snap"
        vp = VAULT_DIR / vault_name_old
        if not vp.exists():
            # fallback: glob for any matching vault file
            import glob
            pat = str(VAULT_DIR / f"{base_name}_v*")
            matches = sorted(glob.glob(pat))
            if matches:
                vp = Path(matches[0])
            else:
                return f"vault file missing: {vault_name}"
    # find original location
    orig_rel = v.get("orig_rel")
    if not orig_rel:
        for r, fi in state["file_index"].items():
            if fi.get("parsed", {}).get("base") == base_name or fi["name"].startswith(base_name):
                orig_rel = r; break
    if not orig_rel: return f"cannot find original location for {base_name}"
    target = WORKSPACE / orig_rel
    target.parent.mkdir(parents=True, exist_ok=True)
    # vault current before overwrite
    _vault_file(orig_rel, state)
    target.write_bytes(vp.read_bytes())
    _save_state(state)
    scan(None)
    return f"ok  rolled back {base_name} → {orig_rel} (v{v.get('ver', '?')})"


@mcp.tool()
def vault_list(base_filter: str | None = None) -> str:
    """List vault contents."""
    state = _load_state()
    vault = state.get("vault", {})
    if base_filter:
        vault = {k: v for k, v in vault.items() if base_filter in k}
    if not vault:
        return "vault is empty"
    lines = [f"{'base':25s} {'versions':>8s}  latest"]
    lines.append("-" * 60)
    for base in sorted(vault.keys())[:50]:
        vers = vault[base]
        latest = vers[0] if vers else {}
        lines.append(f"{base:25s} {len(vers):>8d}  v{latest.get('ver','?')}  {datetime.fromtimestamp(latest.get('ts',0)).strftime('%Y-%m-%d %H:%M')}")
    return "\n".join(lines)


@mcp.tool()
def get_context(tier: str = "active") -> str:
    """Context summary for LLM sessions. tier: skeleton|active|full."""
    state = _load_state()
    roots = state.get("roots", ["."])
    graph = state.get("_dep_graph", {})
    fi = state.get("file_index", {})
    vault = state.get("vault", {})

    # stats
    total = len(fi)
    ws_files = {fi[r]["name"] for r in fi}
    missing = [n for n, g in graph.items() if n not in ws_files and g.get("imported_by")]

    # folder structure
    folders = {}
    for rel in fi:
        parts = rel.split("/")
        folder = "/".join(parts[:-1]) if len(parts) > 1 else "."
        folders.setdefault(folder, []).append(parts[-1])

    # hubs (most connected files)
    hubs = sorted(
        [(n, len(g.get("imports", [])), len(g.get("imported_by", [])))
         for n, g in graph.items() if n in ws_files],
        key=lambda x: x[1] + x[2] * 2, reverse=True
    )

    out = f"# INBOX MANAGER CONTEXT ({tier})\n"
    out += f"# {datetime.now().strftime('%Y-%m-%d %H:%M')}\n"
    out += f"# Roots: {', '.join(roots)} | Files: {total} | Vault: {sum(len(v) for v in vault.values())}\n\n"

    if tier == "skeleton":
        out += f"[ROOTS] {', '.join(roots)}\n"
        out += f"[FILES] {total}\n"
        out += f"[FOLDERS] {len(folders)}\n"
        out += f"[HUBS] {' '.join(f'{h[0]}({h[2]})' for h in hubs[:5])}\n"
        if missing:
            out += f"[MISSING] {' '.join(missing[:10])}\n"
        if vault:
            out += f"[VAULT] {len(vault)} files\n"
        return out

    # active
    out += "## Folders\n"
    for fld in sorted(folders)[:20]:
        files = folders[fld][:6]
        out += f"  {fld}/ ({len(folders[fld])} files)\n"
        for f in files:
            out += f"    {f}\n"
        if len(folders[fld]) > 6:
            out += f"    ... +{len(folders[fld])-6}\n"

    out += "\n## Hubs (most referenced)\n"
    for h in hubs[:10]:
        out += f"  {h[0]}  imports:{h[1]}  imported-by:{h[2]}\n"

    if missing:
        out += f"\n## Missing Dependencies ({len(missing)})\n"
        for n in missing[:15]:
            importers = graph.get(n, {}).get("imported_by", [])
            out += f"  ! {n}  needed-by: {', '.join(importers[:3])}\n"

    out += "\n## Versioned Files\n"
    vers_found = 0
    for r, fi_e in fi.items():
        pv = fi_e.get("parsed")
        if pv and pv.get("ver"):
            out += f"  {fi_e['name']}  v{pv['ver']}\n"
            vers_found += 1
            if vers_found >= 20: break

    if tier == "full":
        out += "\n## All Files\n"
        for rel in sorted(fi):
            out += f"  {rel}\n"
        out += "\n## Full Dep Graph\n"
        dg = state.get("_dep_graph", {})
        for n in sorted(dg)[:60]:
            g = dg[n]
            marker = "MISS" if n not in ws_files else "    "
            if not g.get("imports") and not g.get("imported_by"):
                continue
            out += f"  {marker} {n}\n"
            if g.get("imports"):
                out += f"    imports: {', '.join(g['imports'][:8])}\n"
            if g.get("imported_by"):
                out += f"    used-by: {', '.join(g['imported_by'][:8])}\n"

    return out


@mcp.tool()
def dep_graph(name: str | None = None) -> str:
    """Dependency graph for a file (or all hubs if omitted)."""
    state = _load_state()
    dg = state.get("_dep_graph", {})
    if not dg:
        return "no dep graph — run scan first"
    if name:
        g = dg.get(name)
        if not g:
            # try case-insensitive
            for k, v in dg.items():
                if k.lower() == name.lower():
                    g = v; break
        if not g:
            return f"not found in dep graph: {name}"
        ws_files = {fi["name"] for fi in state["file_index"].values()}
        lines = [f"=== {name} ==="]
        lines.append(f"  imports     ({len(g.get('imports', []))})")
        for d in sorted(g.get("imports", []))[:20]:
            mark = "!" if d not in ws_files else " "
            lines.append(f"    {mark} {d}")
        lines.append(f"  imported-by ({len(g.get('imported_by', []))})")
        for d in sorted(g.get("imported_by", []))[:20]:
            lines.append(f"      {d}")
        return "\n".join(lines)
    # all hubs
    ws_files = {fi["name"] for fi in state["file_index"].values()}
    hubs = sorted(
        [(n, len(g["imports"]), len(g["imported_by"])) for n, g in dg.items() if n in ws_files],
        key=lambda x: x[1] + x[2] * 2, reverse=True
    )[:30]
    lines = ["top 30 hubs:"]
    for h in hubs:
        lines.append(f"  {h[0]:25s}  in:{h[1]:>3d}  by:{h[2]:>3d}")
    return "\n".join(lines)


@mcp.tool()
def auto_place(path: str) -> str:
    """Dry-run: analyze a file and suggest optimal placement."""
    p = Path(path)
    if not p.exists(): return f"err  not found: {path}"
    content = p.read_bytes()
    name = p.name
    state = _load_state()
    pv = _parse_ver(name)
    h = _sha256(content)

    # find matches
    matches = []
    for r, fi in state["file_index"].items():
        if fi["name"] == name:
            matches.append((r, fi, "exact name"))
        elif pv and fi.get("parsed") and fi["parsed"]["base"] == pv["base"] and fi["parsed"]["ext"] == pv["ext"]:
            matches.append((r, fi, f"same base+ext (v{fi['parsed']['ver']})"))
        elif fi.get("hash") == h:
            matches.append((r, fi, "content match"))

    # check content type
    ext = p.suffix.lower()
    lines = [f"analysis for: {name}"]
    lines.append(f"  size: {len(content)} bytes")
    lines.append(f"  hash: {h}")
    lines.append(f"  ext:  {ext}")
    if pv:
        lines.append(f"  version: {pv['ver']}  base: {pv['base']}")

    if matches:
        lines.append(f"\n  matches found in workspace:")
        for r, fi, reason in matches:
            lines.append(f"    {r:40s} ({reason})")
    else:
        suggested = _auto_place({"name": name, "parsed": pv}, state)
        lines.append(f"\n  no direct match → suggested: {suggested}")

    # check imports for context
    if ext in {e.lower() for e in TEXT_EXTS}:
        try:
            imports = _scan_imports(content.decode("utf-8", errors="replace"))
            if imports:
                ws_files = {fi["name"] for fi in state["file_index"].values()}
                found = [d for d in imports if d in ws_files]
                missing = [d for d in imports if d not in ws_files]
                lines.append(f"\n  imports: {len(imports)}")
                if found: lines.append(f"    found:  {' '.join(found[:10])}")
                if missing: lines.append(f"    missing: {' '.join(missing[:10])}")
        except: pass

    return "\n".join(lines)


@mcp.tool()
def incoming_summary() -> str:
    """Summary of important changes for cross-session handoff."""
    state = _load_state()
    incoming = state.get("incoming", [])
    pending = [i for i in incoming if i["status"] in ("new", "update", "conflict", "downgrade")]
    if not pending:
        return "no pending changes"
    lines = [f"Pending: {len(pending)} files"]
    for s in ("conflict", "downgrade", "new", "update"):
        items = [i for i in pending if i["status"] == s]
        if items:
            lines.append(f"\n  {s.upper()} ({len(items)}):")
            for i in items:
                lines.append(f"    {i['name']}  ({i.get('match_key', '?')})")
    return "\n".join(lines)


@mcp.tool()
def incoming_purge_stale() -> str:
    """Remove incoming items with null content_b64 (can never be applied)."""
    state = _load_state()
    n = _cleanup_stale_incoming(state)
    _save_state(state)
    return f"ok  purged {n} stale incoming items"


@mcp.tool()
def get_project_index(detail: str = "skeleton") -> str:
    """Return cached project index without re-scanning.
    detail: skeleton (default) | full"""
    state = _load_state()
    ts = state.get("last_scan_ts", 0)
    if ts == 0 and not state.get("file_index"):
        return "no scan data — run scan() first"
    when = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M") if ts else "never"
    total = len(state.get("file_index", {}))
    groups = state.get("folder_groups", {})
    vaulted = sum(len(v) for v in state.get("vault", {}).values())
    roots = ", ".join(state.get("roots", ["."]))
    dg = state.get("_dep_graph", {})
    ws_names = {fi["name"] for fi in state.get("file_index", {}).values()}
    hubs = sorted(
        [(n, len(g.get("imports", [])), len(g.get("imported_by", [])))
         for n, g in dg.items() if n in ws_names],
        key=lambda x: x[1] + x[2] * 2, reverse=True
    )[:5]
    missing = [n for n, g in dg.items() if n not in ws_names and g.get("imported_by")]

    lines = [f"# {state.get('project_name', '?')}  (last scan: {when})",
             f"Roots: {roots}  |  Files: {total}  |  Vault: {vaulted}  |  Dep nodes: {len(dg)}"]
    lines.append(f"\n## Folder groups ({len(groups)})")
    for folder, count in list(groups.items())[:30]:
        lines.append(f"  {folder}/  ({count})")
    if len(groups) > 30:
        lines.append(f"  ... +{len(groups)-30} more")
    lines.append(f"\n## Top hubs")
    for h in hubs:
        lines.append(f"  {h[0]}  in:{h[1]}  by:{h[2]}")
    if missing:
        lines.append(f"\n## Missing deps ({len(missing)})")
        for n in missing[:10]:
            importers = dg.get(n, {}).get("imported_by", [])
            lines.append(f"  ! {n}  needed-by: {', '.join(importers[:3])}")
    if detail == "full":
        lines.append(f"\n## All files ({total})")
        for rel in sorted(state.get("file_index", {}))[:100]:
            lines.append(f"  {rel}")
        if total > 100:
            lines.append(f"  ... +{total-100} more")
    return "\n".join(lines)


@mcp.tool()
def dashboard_status() -> str:
    """Show Engine Dashboard status + link. Dashboard exposes inbox state visually."""
    state = _load_state()
    total = len(state.get("file_index", {}))
    vaulted = sum(len(v) for v in state.get("vault", {}).values())
    pending = [i for i in state.get("incoming", []) if i["status"] in ("new", "update", "conflict", "downgrade")]
    lines = [
        "## Engine Dashboard",
        f"  URL:   http://127.0.0.1:8766/dashboard",
        f"  API:   http://127.0.0.1:8766/docs",
        "",
        f"  Inbox state:  {total} files indexed, {vaulted} vaulted, {len(pending)} pending",
    ]
    if pending:
        for s in ("conflict", "downgrade", "new", "update"):
            items = [i for i in pending if i["status"] == s]
            if items:
                lines.append(f"    {s.upper()}: {len(items)}")
    # quick check if dashboard is reachable
    import urllib.request
    try:
        urllib.request.urlopen("http://127.0.0.1:8766/api/status", timeout=1.0)
        lines.append("  Dashboard: RUNNING")
    except Exception:
        lines.append("  Dashboard: STOPPED (start with: python collection/python_src/engine_dashboard.py)")
    return "\n".join(lines)


# ── startup helpers ──────────────────────────────────────────────────
def _cleanup_stale_incoming(state: dict) -> int:
    """Remove incoming items with null content_b64 that can never be applied."""
    incoming = state.get("incoming", [])
    before = len(incoming)
    state["incoming"] = [i for i in incoming if i.get("content_b64") is not None or i["status"] == "done"]
    return before - len(state["incoming"])


# ── run ──────────────────────────────────────────────────────────────
# ── File Vault (time-travel snapshots) ─────────────────────────────
try:
    from file_vault.core import FileVault as _FileVault

    def _fvault() -> _FileVault:
        return _FileVault(WORKSPACE)

    @mcp.tool()
    def fvault_checkpoint(label: str = "") -> str:
        """Take a snapshot of watched files (.h/.c/.py/.dll/.so). Optionally label it."""
        fv = _fvault()
        fv.add_root(str(WORKSPACE))
        has_ch, n, _ = fv.has_changes_since_last()
        if has_ch:
            fv.snapshot(label=f"auto_{n}_files_changed")
        snap_id = fv.checkpoint(label=label)
        return f"snapshot {snap_id:06d} — {label or '(no label)'}"

    @mcp.tool()
    def fvault_rewind(snap_id: int, dry_run: bool = False) -> str:
        """Restore files from a snapshot. Use dry_run=True to preview."""
        fv = _fvault()
        has_ch, n, _ = fv.has_changes_since_last()
        if has_ch:
            fv.snapshot(label=f"pre_rewind_{n}_files_changed")
        n_restored = fv.rewind(target_id=snap_id, dry_run=dry_run)
        mode = "would restore" if dry_run else "restored"
        return f"{mode} {n_restored} files from snap_{snap_id:06d}"

    @mcp.tool()
    def fvault_ffwd(from_id: int, to_id: int | None = None, dry_run: bool = False) -> str:
        """Fast-forward: restore only files changed between two snapshots."""
        fv = _fvault()
        n_restored = fv.ffwd(from_id=from_id, to_id=to_id, dry_run=dry_run)
        target = f"snap_{to_id:06d}" if to_id else "latest"
        mode = "would restore" if dry_run else "restored"
        return f"{mode} {n_restored} changed files (snap_{from_id:06d} → {target})"

    @mcp.tool()
    def fvault_branch(from_id: int, name: str) -> str:
        """Branch snapshot history from a given snapshot."""
        snap_id = _fvault().branch(from_id=from_id, branch_name=name)
        return f"branched snap_{from_id:06d} → snap_{snap_id:06d} ({name})"

    @mcp.tool()
    def fvault_list(branch: str | None = None, limit: int = 20) -> str:
        """List snapshot timeline. Optionally filter by branch."""
        snaps = _fvault().list(branch=branch, limit=limit)
        if not snaps:
            return "no snapshots"
        lines = []
        for s in reversed(snaps):
            ts = datetime.fromtimestamp(s['timestamp']).strftime("%Y-%m-%d %H:%M:%S")
            label = f" [{s['label']}]" if s.get('label') else ""
            lines.append(f"  {s['id']:06d}  {ts}  {s['branch']:>8}  {s['n_files']:>4} files{label}")
        return "\n".join(lines)

    @mcp.tool()
    def fvault_diff(snap_a: int, snap_b: int, verbose: bool = False) -> str:
        """Compare two snapshots: added, removed, changed files."""
        d = _fvault().diff(snap_a, snap_b)
        lines = [
            f"snap_{snap_a:06d} → snap_{snap_b:06d}",
            f"  added:   {len(d['added'])}",
            f"  removed: {len(d['removed'])}",
            f"  changed: {len(d['changed'])}",
            f"  same:    {d['same_count']}",
        ]
        if verbose:
            for f in d['added']: lines.append(f"    + {f}")
            for f in d['removed']: lines.append(f"    - {f}")
            for f in d['changed']: lines.append(f"    ~ {f}")
        return "\n".join(lines)

    @mcp.tool()
    def fvault_status() -> str:
        """Show file vault status — snapshots, size, roots."""
        s = _fvault().status()
        lines = [
            f"vault: {s['vault_path']}",
            f"branch: {s['branch']}",
            f"snapshots: {s['snapshots_total']}",
            f"size: {s['vault_size_bytes']:,} bytes",
            f"roots: {len(s['roots'])}",
        ]
        return "\n".join(lines)

    print(f"[inbox] file-vault tools registered (snapshot dir: {WORKSPACE / '.file_vault'})", flush=True)

except ImportError:
    print(f"[inbox] file-vault not available (pip install -e I:\\storage-cleaner)", flush=True)


if __name__ == "__main__":
    # ── auto-migrate old vault from workspace root to drive root ──
    _old_vault = WORKSPACE / ".vault"
    _old_state = WORKSPACE / ".inbox_state.json"
    if _old_vault.exists() and _old_vault != VAULT_DIR:
        _dst = VAULT_DIR
        _dst.mkdir(parents=True, exist_ok=True)
        for _f in _old_vault.iterdir():
            _dest = _dst / _f.name
            if not _dest.exists():
                _f.rename(_dest)
        try:
            _old_vault.rmdir()
            print(f"[inbox] migrated .vault/ → {VAULT_DIR}", flush=True)
        except OSError:
            print(f"[inbox] partial vault migration (some files may remain in {_old_vault})", flush=True)
    if _old_state.exists() and _old_state != STATE_FILE:
        _dest_state = STATE_FILE
        if not _dest_state.exists():
            _old_state.rename(_dest_state)
            print(f"[inbox] migrated .inbox_state.json → {STATE_FILE}", flush=True)

    VAULT_DIR.mkdir(parents=True, exist_ok=True)
    ZIP_ARCHIVE.mkdir(parents=True, exist_ok=True)
    # load or init state
    state = _load_state()
    # rebuild vault index if JSON is empty but physical vault has files
    n_vault = _rebuild_vault_index(state)
    if n_vault:
        print(f"[inbox] rebuilt vault index: {n_vault} entries from physical .vault/", flush=True)
        _save_state(state)
    # clean up stale incoming (null content_b64)
    n_stale = _cleanup_stale_incoming(state)
    if n_stale:
        print(f"[inbox] purged {n_stale} stale incoming items (null content_b64)", flush=True)
        _save_state(state)
    # auto-load cached index
    if state.get("last_scan_ts", 0) > 0 and state.get("file_index"):
        print(f"[inbox] loaded cached index: {state.get('project_name', '?')} ({len(state['file_index'])} files)", flush=True)
    mcp.run()
