"""
geo_field_bridge.py — C Geofield ↔ Python Bond Layer + Smart Folder Connector
══════════════════════════════════════════════════════════════════════════════
Calls geo_field_bridge.exe, parses JSON, feeds into bond layer + Smart Folder.

Usage:
    from geo_field_bridge import GeoFieldBridge

    bridge = GeoFieldBridge()
    result = bridge.process_file("path/to/file.bin", gp_level=2)

    result = {
        "file":        path,
        "size":        int,
        "chunks":      int,
        "blocks":      int,
        "zone_resets": int,
        "topology_fp": str,       # ← used by bond layer
        "roundtrip":   str,       # PASS/FAIL
        "pieces":      {...},     # ← bond pieces
        "wallet":      {...},     # ← coord wallet
        "smart_folder": {...},    # ← ghost lifecycle
    }
"""

from __future__ import annotations
import json, subprocess, tempfile, shutil, os, sys
from pathlib import Path
from typing import Any

# ── paths ────────────────────────────────────────────────────────────────
COLLECTION   = Path(__file__).resolve().parent.parent
BUILD_DIR    = COLLECTION / "build"
BRIDGE_EXE   = BUILD_DIR / "geo_field_bridge.exe"
PYTHON_SRC   = COLLECTION / "python_src"

# Ensure Python can find bond + smart folder modules
sys.path.insert(0, str(PYTHON_SRC))
ZGLS_ROOT = Path(r"I:\ZGLS")
if ZGLS_ROOT.exists():
    sys.path.insert(0, str(ZGLS_ROOT))
    os.environ.setdefault("SMART_FOLDER_POGLS_PY",
                          str(ZGLS_ROOT / "POGLS_SRC" / "master_extracted" / "python"))

from pogls_bond_py import (
    seed_from_fp, make_piece, bond_key, bond_verify,
    make_slot, plug_connect, PLUG_E, PLUG_W, PLUG_N, PLUG_S,
    piece_to_dict, slot_to_dict,
)

# Try SmartFolder — optional (install via pip install ./lc_gcfs_pkg or use ZGLS path)
SMART_FOLDER_AVAILABLE = False
try:
    sys.path.insert(0, str(ZGLS_ROOT))
    from smart_folder_mvp.store import SmartFolderStore
    from smart_folder_mvp.schema import ENTRY_ACTIVE, ENTRY_GHOST
    SMART_FOLDER_AVAILABLE = True
except ImportError:
    SmartFolderStore = None


class GeoFieldBridge:
    """Bridge between C geofield engine and Python bond layer."""

    def __init__(self, bridge_exe: str | Path = BRIDGE_EXE):
        self.bridge_exe = Path(bridge_exe)
        if not self.bridge_exe.exists():
            raise FileNotFoundError(
                f"geo_field_bridge.exe not found at {bridge_exe}\n"
                f"Compile first: cd geopixel/geofield && gcc -O2 -I. "
                f"-o ../../build/geo_field_bridge geo_field_bridge.c"
            )

    # ── Step 1: Run C geofield on a file ──────────────────────────────
    def run_geofield(self, file_path: str | Path, gp_level: int = 2) -> dict:
        """Call geo_field_bridge.exe → parse JSON output."""
        file_path = str(Path(file_path).resolve())
        r = subprocess.run(
            [str(self.bridge_exe), file_path, str(gp_level)],
            capture_output=True, timeout=30,
        )
        if r.returncode != 0:
            stderr = r.stderr.decode().strip()
            raise RuntimeError(f"geo_field_bridge failed: {stderr}")
        return json.loads(r.stdout.decode())

    # ── Step 2: Create bond pieces from geofield output ───────────────
    def make_bond_pieces(self, geofield_result: dict,
                         agent_axes: list[tuple[str, int]] | None = None
                         ) -> dict[str, Any]:
        """
        Create bond pieces from geofield topology_fp.

        Args:
            geofield_result: output from run_geofield()
            agent_axes: list of (agent_id, fold_axis) tuples.
                        Default: [("A",1), ("B",3), ("C",1), ("D",6)]

        Returns:
            {"A": piece_dict, "B": piece_dict, ...}
        """
        if agent_axes is None:
            agent_axes = [("A", 1), ("B", 3), ("C", 1), ("D", 6)]

        fp = geofield_result["topology_fp"]
        pieces = {}

        for i, (aid, axis) in enumerate(agent_axes):
            # Each agent gets a deterministic seed derived from same fp
            seed = int(fp, 16) ^ (i * 0x100)
            pieces[aid] = make_piece(seed, axis)

        return pieces

    # ── Step 3: Wire extrinsic plugs A─B─C─D chain ────────────────────
    @staticmethod
    def wire_plug_chain(pieces: dict[str, Any],
                        agent_order: list[str] | None = None
                        ) -> dict[str, Any]:
        """Create slots and wire A─B─C─D chain."""
        if agent_order is None:
            agent_order = ["A", "B", "C", "D"]

        axes = {"A": 1, "B": 3, "C": 1, "D": 6}
        slots = {}

        for i, aid in enumerate(agent_order):
            piece = pieces.get(aid)
            if piece is None:
                continue
            slot = make_slot(ord(aid), int(piece["geo_key"]), axes.get(aid, 1))
            slot["piece"] = piece
            slots[aid] = slot

        # Wire chain
        for i in range(len(agent_order) - 1):
            a, b = agent_order[i], agent_order[i + 1]
            if a in slots and b in slots:
                plug_connect(slots[a], PLUG_E, slots[b], PLUG_W, 64)

        return slots

    # ── Step 4: Smart Folder lifecycle (ingest → ghost → reconstruct) ─
    def smart_folder_lifecycle(self, file_path: str | Path,
                               geofield_result: dict,
                               pieces: dict[str, Any] | None = None
                               ) -> dict[str, Any]:
        """Ingest file → ghost delete → store bond wallet."""
        if not SMART_FOLDER_AVAILABLE:
            return {"error": "SmartFolder not available", "status": "skipped"}

        file_path = Path(file_path).resolve()
        ws = Path(tempfile.mkdtemp(prefix="geofield_sf_"))
        store = SmartFolderStore(ws)
        info = store.init()
        ws_id = info["workspace_id"]

        # Ingest
        ingest_result = store.ingest_file(file_path,
                                          virtual_path=file_path.name)

        # Get coord wallet
        manifest = store._load()
        entry = manifest.files.get(file_path.name)
        version = entry.current() if entry else None
        wallet = version.coord_wallet if version else {}

        # Build bond wallet
        bond_wallet = {
            "workspace_id": ws_id,
            "geofield_topology_fp": geofield_result["topology_fp"],
            "geofield_chunks": geofield_result["chunks"],
            "geofield_blocks": geofield_result["blocks"],
            "geofield_roundtrip": geofield_result["roundtrip"],
            "sha256_blob_hash": ingest_result.blob_hash[:16],
            "pieces": {},
        }
        if pieces:
            for aid, p in pieces.items():
                bond_wallet["pieces"][aid] = piece_to_dict(p)

        # Ghost delete
        store.delete_file(file_path.name, mode="ghost")
        status = store.status()

        result = {
            "workspace_id": ws_id,
            "ingest_version": ingest_result.version_id,
            "blob_hash": ingest_result.blob_hash[:16],
            "ghost_files": status["ghost_files"],
            "tracked_files": status["tracked_files"],
            "coord_wallet": wallet,
            "bond_wallet": bond_wallet,
        }

        # Reconstruct (demonstrates recovery)
        if version and version.blob_hash:
            reconstruct = store.reconstruct_from_coord_wallet(
                wallet,
                out_path=ws / f"reconstructed_{file_path.name}",
            )
            result["reconstruct_source"] = reconstruct["reconstruct_source"]
            result["reconstruct_exact"] = reconstruct["exact_restore"]

        shutil.rmtree(ws, ignore_errors=True)
        return result

    # ── Full pipeline: C geofield → bond → Smart Folder ───────────────
    def process_file(self, file_path: str | Path, gp_level: int = 2,
                     agent_axes: list[tuple[str, int]] | None = None,
                     enable_smart_folder: bool = True) -> dict[str, Any]:
        """Run full pipeline: C geofield → bond pieces → Smart Folder lifecycle."""

        # Step 1: C geofield
        geo = self.run_geofield(file_path, gp_level)

        # Step 2: Bond pieces
        pieces = self.make_bond_pieces(geo, agent_axes)
        slots = self.wire_plug_chain(pieces)

        # Step 3: Bond verification
        bonds = {}
        agent_order = [a for a, _ in (agent_axes or
                                       [("A",1),("B",3),("C",1),("D",6)])]
        for i in range(len(agent_order)):
            for j in range(i + 1, len(agent_order)):
                a, b = agent_order[i], agent_order[j]
                if a in pieces and b in pieces:
                    bonds[f"{a}↔{b}"] = bond_verify(pieces[a], pieces[b])

        # Step 4: Smart Folder
        sf_result = {}
        if enable_smart_folder and SMART_FOLDER_AVAILABLE:
            sf_result = self.smart_folder_lifecycle(file_path, geo, pieces)

        return {
            "file":          str(Path(file_path).resolve()),
            "size":          geo["size"],
            "geofield": {
                "chunks":      geo["chunks"],
                "blocks":      geo["blocks"],
                "zone_resets": geo["zone_resets"],
                "skeleton":    geo["skeleton"],
                "roundtrip":   geo["roundtrip"],
                "gp_level":    gp_level,
            },
            "topology_fp":   geo["topology_fp"],
            "pieces":        {aid: piece_to_dict(p) for aid, p in pieces.items()},
            "bonds":         bonds,
            "slots":         {aid: slot_to_dict(s) for aid, s in slots.items()},
            "smart_folder":  sf_result,
        }
