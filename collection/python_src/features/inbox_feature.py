from __future__ import annotations

import json
import logging
from pathlib import Path

from fastapi import APIRouter
from .base import EngineFeature

logger = logging.getLogger("engine.features.inbox")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent.parent
_STATE_FILE = _COLLECTION / ".inbox_state.json"
_MCP_SERVER = _COLLECTION / "inbox_mcp_server.py"


def _load_state() -> dict:
    if _STATE_FILE.exists():
        try:
            return json.loads(_STATE_FILE.read_text("utf-8"))
        except Exception:
            pass
    return {}


def _pending_summary(state: dict) -> list[dict]:
    incoming = state.get("incoming", [])
    by_status: dict[str, list[dict]] = {}
    for i in incoming:
        s = i.get("status", "?")
        by_status.setdefault(s, []).append(i)
    out = []
    for s in ("conflict", "downgrade", "new", "update"):
        items = by_status.get(s, [])
        if items:
            out.append({"status": s, "count": len(items), "files": [i["name"] for i in items[:10]]})
    return out


class InboxFeature(EngineFeature):
    name = "Inbox Manager"
    description = "Inbox-manager MCP state: pending files, vault stats, file index"
    icon = "inbox"
    version = "1.0.0"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/inbox", tags=["inbox"])

        @router.get("/")
        def root():
            return {
                "name": self.name,
                "description": self.description,
                "icon": self.icon,
                "version": self.version,
            }

        @router.get("/status")
        def inbox_status():
            state = _load_state()
            total = len(state.get("file_index", {}))
            vaulted = sum(len(v) for v in state.get("vault", {}).values())
            pending = _pending_summary(state)
            return {
                "files_indexed": total,
                "vaulted_files": vaulted,
                "pending": pending,
                "state_file": str(_STATE_FILE),
                "mcp_server": str(_MCP_SERVER),
            }

        @router.get("/pending")
        def pending_files():
            state = _load_state()
            return _pending_summary(state)

        @router.get("/vault")
        def vault_summary():
            state = _load_state()
            vault = state.get("vault", {})
            entries = []
            for base, vers in sorted(vault.items())[:30]:
                entries.append({"base": base, "versions": len(vers)})
            return {"total_bases": len(vault), "entries": entries}

        app.include_router(router)
