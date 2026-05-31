from __future__ import annotations
from typing import Any


class EngineFeature:
    name: str = ""
    description: str = ""
    icon: str = ""
    version: str = "0.1"
    enabled: bool = True
    error: str = ""

    def register_routes(self, app: Any) -> None:
        pass

    def status(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "icon": self.icon,
            "version": self.version,
            "enabled": self.enabled,
            "error": self.error,
        }
