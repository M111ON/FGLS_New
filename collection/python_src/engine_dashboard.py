"""
engine_dashboard.py — Geometry Engine Infrastructure Dashboard
=============================================================
FastAPI server with plugin-based feature system.
Each feature in features/ registers its own API routes.

Usage:
  python engine_dashboard.py              # start on :8766
  python engine_dashboard.py --port 9000  # custom port
"""
from __future__ import annotations

import sys, os, time, logging, argparse
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="[%(name)s] %(message)s",
)
logger = logging.getLogger("engine")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent
for p in [str(_HERE), str(_COLLECTION)]:
    if p not in sys.path:
        sys.path.insert(0, p)

try:
    from fastapi import FastAPI
    from fastapi.responses import HTMLResponse
    import uvicorn
    _HAS_DEPS = True
except ImportError:
    _HAS_DEPS = False
    FastAPI = None

START_TIME = time.time()


def build_app() -> "FastAPI":
    app = FastAPI(
        title="Geometry Engine Dashboard",
        version="0.1.0",
        description="Platform infrastructure dashboard for geometry-addressed routing engine",
    )

    from features import discover_features
    features = discover_features()
    for feat in features:
        try:
            feat.register_routes(app)
            logger.info(f"  Routes registered: {feat.name}")
        except Exception as e:
            feat.enabled = False
            feat.error = str(e)
            logger.warning(f"  Route registration failed: {feat.name}: {e}")

    @app.get("/api/status")
    def engine_status():
        uptime = time.time() - START_TIME
        feature_list = [f.status() for f in features]
        n_online = sum(1 for f in features if f.enabled)
        n_total = len(features)
        return {
            "status": "running",
            "uptime_seconds": round(uptime, 1),
            "features_online": n_online,
            "features_total": n_total,
            "features": feature_list,
            "server": "Geometry Engine Dashboard",
            "version": "0.1.0",
        }

    @app.get("/dashboard", response_class=HTMLResponse)
    def dashboard_page():
        html_path = _HERE / "engine_dashboard.html"
        if html_path.exists():
            return HTMLResponse(content=html_path.read_text(encoding="utf-8"))
        return HTMLResponse("<h1>Dashboard HTML not found</h1>", status_code=404)

    @app.get("/", response_class=HTMLResponse)
    def root():
        return HTMLResponse("""
        <html><body style="font-family:sans-serif;background:#faf8f5;padding:40px;">
        <h1 style="color:#5C3317;">Geometry Engine Dashboard</h1>
        <p><a href="/dashboard" style="color:#C9A96E;">Open Dashboard →</a></p>
        <p><a href="/docs" style="color:#C9A96E;">API Docs →</a></p>
        </body></html>
        """)

    return app


def start(host: str = "127.0.0.1", port: int = 8766):
    if not _HAS_DEPS:
        print("[ERROR] fastapi + uvicorn not installed.")
        print("  pip install fastapi uvicorn")
        sys.exit(1)
    app = build_app()
    print(f"\n  Geometry Engine Dashboard")
    print(f"  {20*'='}")
    print(f"  URL:  http://{host}:{port}/dashboard")
    print(f"  API:  http://{host}:{port}/docs")
    print(f"  {20*'='}\n")
    uvicorn.run(app, host=host, port=port)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8766)
    a = p.parse_args()
    start(a.host, a.port)
