#!/usr/bin/env python3
"""
prose_doc_updater.py — Gradually updates documentation for registered files in prose.json
Runs as cron job: picks one undocumented file per run, reads its source, generates doc snippet,
and writes it to docs/prose/<module>/<filename>.md

Status detection (3 tiers):
  [DEPRECATED]  — file in deprecated/ dir OR "deprecated": true in prose.json
  [STALE]       — not modified in >30 days OR included by <2 active files
  [ACTIVE]      — recently modified and/or heavily included

State tracked in runner/prose_state.json
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path
from datetime import datetime, timedelta

PROJECT_ROOT = Path("I:/FGLS_new")
RUNNER_ROOT = PROJECT_ROOT / "runner"
DEPRECATED_ROOT = PROJECT_ROOT / "deprecated"
PROSE_JSON = RUNNER_ROOT / "prose.json"
PROSE_STATE = RUNNER_ROOT / "prose_state.json"
DOCS_DIR = PROJECT_ROOT / "docs" / "prose"

# Threshold for stale detection
STALE_DAYS = 30
STALE_MIN_INCLUDES = 2


def load_prose():
    with open(PROSE_JSON, "r", encoding="utf-8") as f:
        return json.load(f)


def load_state():
    if PROSE_STATE.exists():
        with open(PROSE_STATE, "r", encoding="utf-8") as f:
            return json.load(f)
    return {"documented": [], "last_run": None, "last_module": None, "last_file": None}


def save_state(state):
    with open(PROSE_STATE, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, ensure_ascii=False)


# ─── Status Detection ────────────────────────────────────────────────

def is_deprecated(filepath, file_entry):
    """Check if a file is deprecated."""
    if file_entry.get("deprecated", False):
        return True
    # Check deprecated/ directory mirrors
    for root in [DEPRECATED_ROOT, RUNNER_ROOT, PROJECT_ROOT]:
        dep_path = root / "deprecated" / filepath
        if dep_path.exists():
            return True
    # Check if resolved path lives under deprecated/
    source = resolve_source(filepath)
    if source:
        try:
            if str(source.resolve()).startswith(str(DEPRECATED_ROOT.resolve())):
                return True
        except Exception:
            pass
    return False


def git_last_modified(filepath):
    """Get last commit date for a file via git log."""
    full = resolve_source(filepath)
    if not full:
        return None
    try:
        result = subprocess.run(
            ["git", "log", "-1", "--format=%ci", "--", str(full)],
            capture_output=True, text=True, timeout=5,
            cwd=str(PROJECT_ROOT)
        )
        if result.returncode == 0 and result.stdout.strip():
            return datetime.strptime(result.stdout.strip()[:19], "%Y-%m-%d %H:%M:%S")
    except Exception:
        pass
    return None


def count_includes(filepath):
    """Count how many other files #include this file."""
    filename = Path(filepath).name
    if not filename.endswith(".h"):
        return -1  # Only check headers

    count = 0
    search_dirs = [
        RUNNER_ROOT,
        PROJECT_ROOT / "collection",
        PROJECT_ROOT / "geopixel",
    ]

    for search_dir in search_dirs:
        if not search_dir.exists():
            continue
        try:
            result = subprocess.run(
                ["grep", "-rl", f'#include.*{filename}', str(search_dir)],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0:
                for line in result.stdout.strip().split("\n"):
                    if line and Path(line).name != filename:
                        count += 1
        except Exception:
            pass

    return count


def detect_file_status(filepath, file_entry):
    """
    Detect file status: deprecated > stale > active
    Returns (status, detail_str)
    """
    if is_deprecated(filepath, file_entry):
        return "deprecated", "No longer in active use"

    last_mod = git_last_modified(filepath)
    include_count = count_includes(filepath)

    is_stale = False
    reasons = []

    if last_mod:
        age_days = (datetime.now() - last_mod).days
        if age_days > STALE_DAYS:
            is_stale = True
            reasons.append(f"last modified {age_days}d ago")
    else:
        is_stale = True
        reasons.append("no git history found")

    if include_count >= 0 and include_count < STALE_MIN_INCLUDES:
        is_stale = True
        reasons.append(f"included by only {include_count} file(s)")

    if is_stale:
        detail = "; ".join(reasons) if reasons else "inactive"
        return "stale", detail

    detail_parts = []
    if last_mod:
        detail_parts.append(f"modified {(datetime.now() - last_mod).days}d ago")
    if include_count >= 0:
        detail_parts.append(f"included by {include_count} file(s)")
    return "active", "; ".join(detail_parts) if detail_parts else "in use"


# ─── Doc Extraction ──────────────────────────────────────────────────

def resolve_source(filepath):
    """Resolve source file — check runner/ first, then project root."""
    candidates = [
        RUNNER_ROOT / filepath,
        PROJECT_ROOT / filepath,
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def extract_doc_from_source(filepath, status="active", status_detail=""):
    """Extract documentation from a C/H source file."""
    full = resolve_source(filepath)
    if not full:
        return None

    with open(full, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()

    lines = content.split("\n")
    in_block_comment = False
    header_comments = []
    api_sigs = []
    defines = []
    structs = []

    for line in lines:
        stripped = line.strip()

        if "/*" in stripped and "*/" not in stripped:
            in_block_comment = True
            comment_text = stripped.replace("/*", "").strip()
            if comment_text:
                header_comments.append(comment_text)
            continue
        if in_block_comment:
            if "*/" in stripped:
                in_block_comment = False
                comment_text = stripped.replace("*/", "").strip()
                if comment_text:
                    header_comments.append(comment_text)
            else:
                comment_text = stripped.rstrip("* /").strip()
                if comment_text:
                    header_comments.append(comment_text)
            continue

        if stripped.startswith("///") or stripped.startswith("//!"):
            header_comments.append(stripped.lstrip("/ !").strip())
            continue

        if "PGLS_API" in stripped or (
            re.match(r"^(static\s+)?(inline\s+)?\w+[\s\*]+\w+\s*\(", stripped)
            and not stripped.startswith("//")
        ):
            sig = stripped.split("{")[0].strip().rstrip(";")
            if len(sig) > 10:
                api_sigs.append(sig)

        if stripped.startswith("#define ") and not stripped.startswith("#define _"):
            defines.append(stripped)

        if stripped.startswith("typedef struct") or stripped.startswith("struct "):
            structs.append(stripped.split("{")[0].strip().rstrip(";"))

    # ─── Build doc ───
    module_name = filepath.split("/")[0] if "/" in filepath else filepath.split("\\")[0]
    filename = Path(filepath).name

    # Status banner
    status_banners = {
        "deprecated": "> ⚠️ **[DEPRECATED]** — This file is no longer actively used. Documentation preserved for reference.\n\n",
        "stale":      "> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.\n\n",
        "active":     "> 🟢 **[ACTIVE]** — This file is in current use.\n\n",
    }

    status_labels = {
        "deprecated": "`deprecated`",
        "stale":      "`stale`",
        "active":     "`active`",
    }

    doc = f"# {filename}\n\n"
    doc += status_banners.get(status, "")
    doc += f"**Module:** `{module_name}`  \n"
    doc += f"**Path:** `{filepath}`  \n"
    doc += f"**Status:** {status_labels.get(status, '`unknown`')}  \n"
    if status_detail:
        doc += f"**Note:** {status_detail}  \n"
    doc += f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M')}  \n\n"

    if header_comments:
        doc += "## Description\n\n"
        doc += "\n".join(header_comments[:20]) + "\n\n"

    if structs:
        doc += "## Structures\n\n"
        for s in structs[:10]:
            doc += f"- `{s}`\n"
        doc += "\n"

    if api_sigs:
        doc += "## API Functions\n\n"
        for sig in api_sigs[:20]:
            doc += f"- `{sig}`\n"
        doc += "\n"

    if defines:
        doc += "## Constants\n\n"
        for d in defines[:15]:
            doc += f"- `{d}`\n"
        doc += "\n"

    if not header_comments and not api_sigs and not structs:
        doc += "_No extractable documentation found in source comments._\n\n"

    return doc


# ─── File Picker ─────────────────────────────────────────────────────

def pick_next_file(prose_data, state):
    """Pick the next undocumented file, round-robin through modules."""
    modules = prose_data.get("modules", [])
    if not modules:
        return None, None

    documented = set(state.get("documented", []))

    last_module = state.get("last_module")
    start_idx = 0
    if last_module:
        for i, m in enumerate(modules):
            if m["name"] == last_module:
                start_idx = (i + 1) % len(modules)
                break

    for offset in range(len(modules)):
        idx = (start_idx + offset) % len(modules)
        module = modules[idx]
        for file_entry in module.get("files", []):
            fpath = file_entry["path"]
            if fpath not in documented:
                return module, file_entry

    return None, None


# ─── Main ────────────────────────────────────────────────────────────

def main():
    prose_data = load_prose()
    state = load_state()

    module, file_entry = pick_next_file(prose_data, state)

    if not file_entry:
        print("All files documented! Nothing to do.")
        return 0

    filepath = file_entry["path"]
    module_name = module["name"]

    # Detect status
    status, status_detail = detect_file_status(filepath, file_entry)
    status_tag = f" [{status.upper()}]" if status != "active" else ""

    print(f"[prose] Processing: {filepath} (module: {module_name}){status_tag}")
    if status_detail:
        print(f"[prose]   Detail: {status_detail}")

    # Generate doc
    doc_content = extract_doc_from_source(filepath, status=status, status_detail=status_detail)
    if not doc_content:
        print(f"[prose] Source not found: {filepath}, skipping")
        state.setdefault("documented", []).append(filepath)
        state["last_run"] = datetime.now().isoformat()
        state["last_module"] = module_name
        state["last_file"] = filepath
        save_state(state)
        return 0

    # Write doc file
    module_dir = DOCS_DIR / module_name
    module_dir.mkdir(parents=True, exist_ok=True)

    filename = Path(filepath).stem + ".md"
    doc_path = module_dir / filename
    with open(doc_path, "w", encoding="utf-8") as f:
        f.write(doc_content)

    # Update state (store status too)
    state.setdefault("documented", []).append(filepath)
    state.setdefault("statuses", {})[filepath] = status
    state["last_run"] = datetime.now().isoformat()
    state["last_module"] = module_name
    state["last_file"] = filepath
    save_state(state)

    total_files = sum(len(m.get("files", [])) for m in prose_data.get("modules", []))
    done = len(state["documented"])

    # Summary counts
    statuses = state.get("statuses", {})
    n_active = sum(1 for v in statuses.values() if v == "active")
    n_stale = sum(1 for v in statuses.values() if v == "stale")
    n_deprecated = sum(1 for v in statuses.values() if v == "deprecated")

    print(f"[prose] Written: {doc_path}")
    print(f"[prose] Progress: {done}/{total_files} documented ({n_active} active, {n_stale} stale, {n_deprecated} deprecated)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
