"""
pogls_mcp_server.py — POGLS Pipeline MCP Server

Calls real C executables from runner/ for compress, verify, inspect, etc.
All tools run via subprocess — no unreliable Python module imports.
"""

import os, sys, json, subprocess, tempfile, base64, shutil
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("POGLS Pipeline")

BASE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(BASE, "runner")

def _log(msg):
    print(f"[{os.path.basename(__file__)}] {msg}", file=sys.stderr, flush=True)

def _exe(name):
    """Full path to a runner executable (cross-platform, no .exe hardcode)."""
    is_win = sys.platform.startswith("win")
    p = os.path.join(RUNNER, name)
    if os.path.isfile(p):
        return p
    if is_win:
        p2 = p + ".exe"
        if os.path.isfile(p2):
            return p2
    return p  # let caller handle FileNotFoundError

def _run(exe_name, *args, timeout=60, input_data=None):
    """Run a runner executable with args; return (returncode, stdout, stderr)."""
    exe_path = _exe(exe_name)
    if not os.path.isfile(exe_path):
        return -1, "", f"executable not found: {exe_path}"
    proc = subprocess.run(
        [exe_path] + list(args),
        capture_output=True, timeout=timeout,
        input=input_data
    )
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace"), proc.stderr.decode("utf-8", errors="replace")

def _nice_size(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024: return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"

def _temp_path(suffix=""):
    fd, p = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    return p

# ═════════════════════════════════════════════════════════════════════
# Tools
# ═════════════════════════════════════════════════════════════════════

@mcp.tool()
def pogls_compress(path: str | None = None, data_b64: str | None = None,
                   out: str = "", level: int = 3) -> str:
    """Compress binary → ZSTD. Provide file path or base64 data."""
    tmp_in = None
    try:
        if path:
            in_path = os.path.abspath(path)
            if not os.path.isfile(in_path):
                return f"err  file not found: {in_path}"
            base_name = os.path.splitext(os.path.basename(path))[0]
            default_dir = os.path.dirname(in_path)
        elif data_b64:
            raw = base64.b64decode(data_b64)
            tmp_in = _temp_path(".bin")
            with open(tmp_in, "wb") as f: f.write(raw)
            in_path = tmp_in
            base_name = "data"
            default_dir = "."
        else:
            return "err  provide path or data_b64"

        out_path = out or os.path.join(default_dir, f"{base_name}.zst")
        rc, sout, serr = _run("pogls_compress", in_path, out_path, "-l", str(level))
        if rc != 0:
            return f"err  compress failed (rc={rc}):\n{serr}{sout}"

        orig_sz = os.path.getsize(in_path)
        comp_sz = os.path.getsize(out_path)
        ratio = orig_sz / comp_sz if comp_sz else 0
        return (f"ok  {os.path.basename(out_path)}  "
                f"orig={_nice_size(orig_sz)}  comp={_nice_size(comp_sz)}  ratio={ratio:.2f}x")
    finally:
        if tmp_in and os.path.isfile(tmp_in): os.unlink(tmp_in)


@mcp.tool()
def pogls_decompress(path: str, out: str = "") -> str:
    """Decompress ZSTD → original binary."""
    in_path = os.path.abspath(path)
    if not os.path.isfile(in_path):
        return f"err  file not found: {in_path}"
    base_name = os.path.splitext(os.path.basename(path))[0]
    default_dir = os.path.dirname(in_path)
    out_path = out or os.path.join(default_dir, base_name)
    rc, sout, serr = _run("pogls_decompress", in_path, out_path)
    if rc != 0:
        return f"err  decompress failed (rc={rc}):\n{serr}{sout}"
    sz = os.path.getsize(out_path)
    return f"ok  {os.path.basename(out_path)}  ({_nice_size(sz)})"


@mcp.tool()
def pogls_verify(path: str) -> str:
    """Verify .pogls file integrity. Returns detailed check results."""
    in_path = os.path.abspath(path)
    if not os.path.isfile(in_path):
        return f"err  file not found: {in_path}"
    rc, sout, serr = _run("pogls_verify", in_path)
    result = f"exit code: {rc}\n\n"
    if sout: result += sout
    if serr: result += serr
    if rc == 0:
        result = f"ok  ALL PASS (exit 0)\n\n" + sout
    return result


@mcp.tool()
def pogls_inspect(path: str) -> str:
    """Dump .pogls file header, tensor metadata, and stats."""
    in_path = os.path.abspath(path)
    if not os.path.isfile(in_path):
        return f"err  file not found: {in_path}"
    rc, sout, serr = _run("pogls_inspect", in_path)
    if rc != 0:
        return f"err  inspect failed (rc={rc}):\n{serr}{sout}"
    return sout or serr


@mcp.tool()
def pogls_roundtrip(path: str, level: int = 3) -> str:
    """Verify compress → decompress roundtrip for a file."""
    in_path = os.path.abspath(path)
    if not os.path.isfile(in_path):
        return f"err  file not found: {in_path}"
    rc, sout, serr = _run("pogls_roundtrip", in_path, "-l", str(level))
    if rc != 0:
        return f"err  roundtrip failed (rc={rc}):\n{serr}{sout}"
    return sout or serr


@mcp.tool()
def pogls_build(gguf_path: str, output: str = "",
                compress: bool = False, verify: bool = False) -> str:
    """Convert GGUF model → .pogls tensor store."""
    in_path = os.path.abspath(gguf_path)
    if not os.path.isfile(in_path):
        return f"err  gguf not found: {in_path}"
    base_name = os.path.splitext(os.path.basename(gguf_path))[0]
    default_dir = os.path.dirname(in_path)
    out_path = output or os.path.join(default_dir, f"{base_name}.pogls")
    args = [in_path, out_path]
    if compress: args.append("--compress")
    if verify: args.append("--verify")
    rc, sout, serr = _run("pogls_build", *args)
    if rc != 0:
        return f"err  build failed (rc={rc}):\n{serr}{sout}"
    sz = os.path.getsize(out_path) if os.path.isfile(out_path) else 0
    return (f"ok  {os.path.basename(out_path)}  ({_nice_size(sz)})\n" +
            (sout or ""))


@mcp.tool()
def pogls_diff(file1: str, file2: str, first_diff: int = 20) -> str:
    """Byte-by-byte diff of two files."""
    f1 = os.path.abspath(file1)
    f2 = os.path.abspath(file2)
    for p, label in [(f1, "file1"), (f2, "file2")]:
        if not os.path.isfile(p):
            return f"err  {label} not found: {p}"
    args = [f1, f2, "--first-diff", str(first_diff)]
    rc, sout, serr = _run("pogls_diff", *args)
    if rc != 0:
        return f"err  diff failed (rc={rc}):\n{serr}{sout}"
    result = (sout or serr)
    sz1, sz2 = os.path.getsize(f1), os.path.getsize(f2)
    result += f"\nfile1={_nice_size(sz1)}  file2={_nice_size(sz2)}"
    if sz1 == sz2 and "identical" in result.lower():
        result = f"ok  IDENTICAL  ({_nice_size(sz1)})"
    return result


@mcp.tool()
def addr_resolve(name: str = "", gguf_path: str = "", tier: int = 0) -> str:
    """Resolve tensor address by name or list all tensors in GGUF."""
    args = ["--tier", str(tier)]
    if name:
        args = ["--name", name] + args
    elif gguf_path:
        p = os.path.abspath(gguf_path)
        if not os.path.isfile(p):
            return f"err  gguf not found: {p}"
        args = ["--file", p] + args
    else:
        return "err  provide --name or --gguf-path"
    rc, sout, serr = _run("addr_resolve", *args)
    if rc != 0:
        return f"err  addr_resolve failed (rc={rc}):\n{serr}{sout}"
    return sout or serr


@mcp.tool()
def gguf_dump(path: str) -> str:
    """Dump GGUF model header, KV pairs, and tensor list."""
    in_path = os.path.abspath(path)
    if not os.path.isfile(in_path):
        return f"err  file not found: {in_path}"
    rc, sout, serr = _run("gguf_dump", in_path)
    if rc != 0:
        return f"err  gguf_dump failed (rc={rc}):\n{serr}{sout}"
    return sout or serr


@mcp.tool()
def pogls_test() -> str:
    """Run the full POGLS toolchain test suite."""
    rc, sout, serr = _run("pogls_test")
    result = sout or serr
    pass_count = result.count("PASS")
    fail_count = result.count("FAIL")
    status = "ALL PASS" if fail_count == 0 else f"{fail_count} FAILURE(S)"
    return f"ok  {status}\n\n{result}"


@mcp.tool()
def pogls_pipeline() -> str:
    """Check all pipeline executables and run a quick health check."""
    tools = [
        "pogls_compress", "pogls_decompress",
        "pogls_verify", "pogls_inspect",
        "pogls_roundtrip", "pogls_build",
        "pogls_diff", "addr_resolve",
        "gguf_dump", "pogls_test",
        "pogls_cat", "dramtile_dump",
    ]
    lines = [f"POGLS Pipeline — {RUNNER}"]
    ok = 0; miss = 0
    for t in tools:
        p = _exe(t)
        if os.path.isfile(p):
            sz = os.path.getsize(p)
            base = os.path.basename(p)
            lines.append(f"  [OK  ] {base:30s} {_nice_size(sz)}")
            ok += 1
        else:
            lines.append(f"  [MISS] {t}")
            miss += 1
    lines.append(f"\n  {ok} tools available, {miss} missing")
    if ok == len(tools):
        lines.append("  Status: ALL TOOLS PRESENT")
    elif ok >= 8:
        lines.append("  Status: MOST TOOLS PRESENT (usable)")
    else:
        lines.append("  Status: CRITICAL TOOLS MISSING")
    return "\n".join(lines)


@mcp.tool()
def pogls_xxh64(path: str = "", data_b64: str = "") -> str:
    """Compute xxh64 hash of a file or base64 data using Python fallback."""
    if path:
        p = os.path.abspath(path)
        if not os.path.isfile(p): return f"err  file not found: {p}"
        with open(p, "rb") as f: raw = f.read()
        label = os.path.basename(path)
    elif data_b64:
        raw = base64.b64decode(data_b64)
        label = "data"
    else:
        return "err  provide path or data_b64"
    h = _xxh64(raw)
    return f"xxh64({label}) = {h:016X}  ({_nice_size(len(raw))})"


def _xxh64(data):
    """Minimal xxh64 — no external deps."""
    H1 = 0x9e3779b97f4a7c15
    H2 = 0x6c62272e07bb0142
    M = (1 << 64) - 1
    h = (H1 ^ len(data)) & M
    for i in range(0, len(data), 8):
        if i + 8 <= len(data):
            w = int.from_bytes(data[i:i+8], "little")
        else:
            w = int.from_bytes(data[i:] + b"\x00" * (8 - len(data[i:])), "little")
        h = (((h ^ ((w * H1) & M)) & M))
        h = (((h << 27) | (h >> 37)) & M)
        h = (h * H2 + 0x94d049bb133111eb) & M
    h ^= h >> 33; h &= M
    h *= H1; h &= M
    h ^= h >> 29; h &= M
    h *= H2; h &= M
    h ^= h >> 32; h &= M
    return h


# ═════════════════════════════════════════════════════════════════════
# PROSE — Project Source Explorer
# ═════════════════════════════════════════════════════════════════════

PROSE_HTML_PATH = os.path.join(BASE, "output", "prose.html")

_MODULE_META = {
    "core":      {"label":"pogls_core",     "role":"Foundation Layer",           "desc":"Platform abstraction, compression API, 144² address space, v2 metadata format",                    "dirs":["runner/pogls_core"]},
    "gguf":      {"label":"pogls_gguf",     "role":"GGUF Reader Layer",         "desc":"Standalone GGUF file parser — no llama.cpp dep",                                                     "dirs":["runner/pogls_gguf"]},
    "geo":       {"label":"pogls_geo",      "role":"Geometric Addressing Layer", "desc":"Y-triangle coordinate system: 144² tower, jump, triplet",                                          "dirs":["runner/pogls_geo"]},
    "dram":      {"label":"pogls_dram",     "role":"Memory Store Layer",        "desc":"Flat-arena DRamTile: put/get by addr or name",                                                     "dirs":["runner/pogls_dram"]},
    "bermuda":   {"label":"pogls_bermuda",  "role":"Routing + Compression Layer","desc":"Stride-37 router, Diamond Shell, 3D RLE, shadow bond",                                           "dirs":["runner/pogls_bermuda"]},
    "kv":        {"label":"pogls_kv",       "role":"KV Cache Layer",            "desc":"Adaptive KV remap: ENTROPY/GEO/REBUILD, 3-lane rail",                                              "dirs":["runner/pogls_kv"]},
    "geopixel":  {"label":"pogls_geopixel", "role":"Pixel Compression Layer",   "desc":"64B block codec: FLAT/SMOOTH/GRADIENT/EDGE, Hilbert", "doc":"pogls-geopixel",                      "dirs":["runner/pogls_geopixel"]},
    "tools":     {"label":"pogls_tools",    "role":"CLI Tools",                 "desc":"14 CLI utilities for build, inspect, test, debug",                                                 "dirs":["runner/pogls_tools"]},
}

# External modules — live outside runner/, scanned from other project roots
_EXTERNAL_MODULES = [
    {"name":"geopixel-codec", "label":"geopixel-codec", "role":"Standalone Geopixel Codec",
     "desc":"Full tile codec: v21 encoder/decoder, Hamburger classify, GPX decode, Hilbert curve",
     "root":"geopixel", "dirs":["src","include"], "doc":"pogls-geopixel-codec"},
    {"name":"geofield", "label":"geofield", "role":"GeoField Topology & Routing",
     "desc":"Goldberg sphere GpAddr, Metatron 4-route, FrustumBlock/DiamondBlock, Trit decomposition",
     "root":"collection/geopixel/geofield", "dirs":[""], "doc":"geofield"},
]

def _first_line_comment(path):
    """Extract first meaningful comment from a .c/.h file."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("/*"):
                    inner = line[2:].strip().rstrip("*/").strip()
                    if inner: return inner
                elif line.startswith("//"):
                    inner = line[2:].strip()
                    if inner: return inner
        return ""
    except: return ""

def _scan_dir_files(root_dir, subdirs, module_name, base_path):
    """Scan one or more subdirectories for .c/.h files, return (entries, test_count)."""
    entries = []
    test_count = 0
    for sd in subdirs:
        d = os.path.join(root_dir, sd) if sd != "." else root_dir
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            full = os.path.join(d, fn)
            if not os.path.isfile(full): continue
            ext = os.path.splitext(fn)[1].lower()
            if ext not in (".c", ".h"): continue
            purpose = _first_line_comment(full)
            if not purpose:
                purpose = f"{module_name} module — {fn}"
            if fn.startswith("test_"):
                test_count += 1
                continue
            rel = os.path.relpath(full, base_path).replace("\\", "/")
            entries.append({"path": rel, "purpose": purpose})
    return entries, test_count

def _scan_prose_modules():
    """Scan runner/pogls_* directories + external dirs → list of PROSE module dicts."""
    modules = []
    runner_dir = RUNNER
    base = BASE
    if not os.path.isdir(runner_dir): return modules

    for mod_name, meta in _MODULE_META.items():
        mod_dir = os.path.join(runner_dir, f"pogls_{mod_name}" if mod_name != "tools" else "pogls_tools")
        if not os.path.isdir(mod_dir):
            continue
        files = sorted(os.listdir(mod_dir))
        entries = []
        test_count = 0
        for fn in files:
            full = os.path.join(mod_dir, fn)
            if not os.path.isfile(full): continue
            ext = os.path.splitext(fn)[1].lower()
            if ext not in (".c", ".h"): continue
            purpose = _first_line_comment(full)
            if not purpose:
                purpose = f"{mod_name} module — {fn}"
            is_test = fn.startswith("test_")
            if is_test:
                test_count += 1
                continue
            entries.append({"path": f"pogls_{mod_name}/{fn}" if mod_name != "tools" else f"pogls_tools/{fn}",
                            "purpose": purpose})
        if not entries:
            continue
        mod_dict = {
            "name": mod_name,
            "label": meta["label"],
            "role": meta["role"],
            "desc": meta["desc"],
            "tests": test_count,
            "files": entries,
        }
        if "dirs" in meta:
            mod_dict["dirs"] = meta["dirs"]
        if "doc" in meta:
            mod_dict["doc"] = meta["doc"]
        if "design_note" in meta:
            mod_dict["design_note"] = meta["design_note"]
        modules.append(mod_dict)

    # Scan external modules
    for ext_mod in _EXTERNAL_MODULES:
        root_dir = os.path.join(base, ext_mod["root"])
        if not os.path.isdir(root_dir):
            continue
        entries, test_count = _scan_dir_files(root_dir, ext_mod["dirs"], ext_mod["name"], base)
        if not entries:
            continue
        ext_root = ext_mod["root"]
        ext_subdirs = ext_mod.get("dirs", ["."])
        mod_dict = {
            "name": ext_mod["name"],
            "label": ext_mod["label"],
            "role": ext_mod["role"],
            "desc": ext_mod["desc"],
            "tests": test_count,
            "files": entries,
            "dirs": [os.path.normpath(os.path.join(ext_root, sd)).replace("\\", "/") for sd in ext_subdirs],
        }
        if "doc" in ext_mod:
            mod_dict["doc"] = ext_mod["doc"]
        modules.append(mod_dict)

    return modules


@mcp.tool()
def prose_update() -> str:
    """Scan runner/ and regenerate output/prose.html with fresh file index."""
    modules = _scan_prose_modules()
    if not modules:
        return "err  no modules found in runner/"

    # build JSON payload
    payload = json.dumps({
        "project": "POGLS Toolchain",
        "version": "2.0",
        "root": "runner",
        "modules": modules,
    }, indent=1)

    prose_html = os.path.join(BASE, "output", "prose.html")
    if not os.path.isfile(prose_html):
        return f"err  prose.html not found at {prose_html}"

    with open(prose_html, "r", encoding="utf-8") as f:
        html = f.read()

    import re
    new_html = re.sub(
        r'var PROSE_JSON\s*=\s*\{.*?\};',
        lambda _: f'var PROSE_JSON ={payload};',
        html, count=1, flags=re.DOTALL
    )
    if new_html == html:
        return "err  regex replace failed — PROSE_JSON pattern not matched"

    with open(prose_html, "w", encoding="utf-8") as f:
        f.write(new_html)

    n_files = sum(len(m["files"]) for m in modules)
    return f"ok  prose.html updated — {len(modules)} modules, {n_files} files"


@mcp.tool()
def prose_info() -> str:
    """Show current PROSE module summary without updating."""
    html_path = PROSE_HTML_PATH
    if not os.path.isfile(html_path):
        return f"err  prose.html not found at {html_path}"
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()
    import re
    m = re.search(r'var PROSE_JSON\s*=\s*(\{.*?\});', html, re.DOTALL)
    if not m:
        return "err  no PROSE_JSON in prose.html"
    try:
        data = json.loads(m.group(1))
        lines = [f"PROSE — {data.get('project','?')} v{data.get('version','?')}"]
        for mod in data.get("modules", []):
            lines.append(f"  [{mod['name']:10s}] {len(mod['files']):2d} files  {mod['tests']:2d} tests  {mod['role']}")
        total_files = sum(len(mod.get("files", [])) for mod in data.get("modules", []))
        lines.append(f"\n  {len(data.get('modules', []))} modules, {total_files} files total")
        return "\n".join(lines)
    except json.JSONDecodeError as e:
        return f"err  json parse: {e}"


# ── PROSE HTML as MCP resource ──────────────────────────────────────
@mcp.resource("prose://html")
def prose_html_resource() -> str:
    """PROSE Project Source Explorer — serve the PROSE HTML as a loadable MCP resource."""
    if not os.path.isfile(PROSE_HTML_PATH):
        return f"<!-- PROSE not found at {PROSE_HTML_PATH} -->"
    with open(PROSE_HTML_PATH, "r", encoding="utf-8") as f:
        return f.read()


if __name__ == "__main__":
    _log(f"starting — runner dir: {RUNNER}")
    mcp.run()
