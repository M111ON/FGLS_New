from __future__ import annotations

import hashlib
import json
import os
import sys
import tkinter as tk
import urllib.request
import urllib.error
from pathlib import Path
from tkinter import filedialog, ttk

API_BASE = os.environ.get("BOND_API_URL", "http://127.0.0.1:8000")

BG = "#f9f5f0"
SURFACE = "#ffffff"
BORDER = "#e8ddd4"
CHOCO = "#5c3317"
CHOCO_D = "#3b1f0e"
CHOCO_L = "#c4845a"
CREAM = "#f0e6d9"
MUTED = "#8a6550"
TEXT = "#1a0f06"
WARN_BG = "#fff8f3"
WARN_FG = "#a0522d"
GREEN = "#4a7c59"
FONT_SM = ("Consolas", 9)
FONT_MED = ("Consolas", 10)
FONT_BOLD = ("Consolas", 10, "bold")


# ── Topology scan ─────────────────────────────────────────────────────

def _stable_id(text: str, length: int = 16) -> str:
    return hashlib.sha256(text.encode()).hexdigest()[:length]


def _scan_fp(file_path: str) -> tuple[str, dict]:
    """Scan a file and return (fp_hex, topology_result)."""
    data = Path(file_path).read_bytes()
    from smart_folder_mvp.pogls_bridge import topology_scan_multiscale
    topo = topology_scan_multiscale(data)
    raw = "|".join([
        topo.get("content_type", "unknown"),
        topo.get("routing_hint", "unknown"),
        str(topo.get("intra_dedup", 0)),
        str(topo.get("phi_ratio", 0)),
    ])
    fp = _stable_id(raw, length=16)
    return fp, topo


# ── API calls ──────────────────────────────────────────────────────────

def api_post(path: str, body: dict) -> dict:
    data = json.dumps(body).encode()
    req = urllib.request.Request(f"{API_BASE}{path}", data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        detail = e.read()
        try:
            return {"error": json.loads(detail).get("detail", str(e))}
        except Exception:
            return {"error": str(e)}
    except Exception as e:
        return {"error": str(e)}


# ── Windows context menu ──────────────────────────────────────────────

MENU_KEY = r"Software\Classes\*\shell\BondLayer"
MENU_LABEL = "Bond Layer"


def install_context_menu():
    try:
        import winreg
        here = Path(__file__).resolve()
        pythonw = here.parent.parent / "pythonw.exe"
        if not pythonw.exists():
            pythonw = Path(sys.executable).parent / "pythonw.exe"
        cmd = f'"{pythonw}" "{here}" "%1"'
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, MENU_KEY) as key:
            winreg.SetValueEx(key, "MUIVerb", 0, winreg.REG_SZ, MENU_LABEL)
            winreg.SetValueEx(key, "Icon", 0, winreg.REG_SZ, str(pythonw))
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, MENU_KEY + r"\command") as key:
            winreg.SetValueEx(key, "", 0, winreg.REG_SZ, cmd)
        return True
    except Exception:
        return False


def uninstall_context_menu():
    try:
        import winreg
        import subprocess
        subprocess.run(["reg", "delete", f"HKCU\\{MENU_KEY}", "/f"], capture_output=True)
        return True
    except Exception:
        return False


# ── UI ─────────────────────────────────────────────────────────────────

class BondWindow:
    def __init__(self, file_arg: str = "") -> None:
        self.root = tk.Tk()
        self.root.title("Bond Layer v1.1")
        self.root.configure(bg=BG)
        self.root.geometry("800x680")
        self.root.minsize(640, 500)
        self._scan_result = None
        self._build_ui()
        if file_arg:
            self.file_path_entry.delete(0, "end")
            self.file_path_entry.insert(0, file_arg)

    def _build_ui(self) -> None:
        header = tk.Frame(self.root, bg=CHOCO_D, padx=16, pady=10)
        header.pack(fill="x")
        tk.Label(header, text="Bond Layer v1.1", bg=CHOCO_D, fg="#ffffff", font=("Georgia", 14)).pack(side="left")
        tk.Label(header, text="POGLS GEOMETRY BOND", bg=CHOCO_D, fg=CHOCO_L, font=("Consolas", 8)).pack(side="left", padx=(10, 0), pady=2)

        # File picker
        picker = tk.Frame(self.root, bg=BG, padx=16, pady=12)
        picker.pack(fill="x")
        row = tk.Frame(picker, bg=BG)
        row.pack(fill="x")
        self.file_path_entry = tk.Entry(row, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.file_path_entry.pack(side="left", fill="x", expand=True)
        self._button(row, "Browse", self._on_browse).pack(side="left", padx=(6, 0))
        self._button(row, "Scan File", self._on_scan).pack(side="left", padx=(6, 0))
        self.scan_info = tk.Label(picker, text="", bg=BG, fg=MUTED, font=FONT_SM, anchor="w")
        self.scan_info.pack(fill="x", pady=(4, 0))

        # Notebook tabs
        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=8, pady=8)

        style = ttk.Style()
        style.theme_use("default")
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=CREAM, foreground=CHOCO, font=FONT_SM, padding=[10, 4])
        style.map("TNotebook.Tab", background=[("selected", SURFACE)])

        self._tab_piece(nb)
        self._tab_verify(nb)
        self._tab_wallet(nb)
        self._tab_health(nb)
        self._tab_about(nb)

        status = tk.Frame(self.root, bg=CREAM, padx=12, pady=6)
        status.pack(fill="x")
        self.status_var = tk.StringVar(value="Ready")
        tk.Label(status, textvariable=self.status_var, bg=CREAM, fg=MUTED, font=FONT_SM).pack(side="left")

    def _tab_piece(self, nb: ttk.Notebook) -> None:
        frame = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(frame, text="Create Piece")

        tk.Label(frame, text="topology_fp (16 hex chars)", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w")
        self.fp_entry = tk.Entry(frame, font=FONT_MED, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.fp_entry.insert(0, "a3f0b2c1d4e5f6a7")
        self.fp_entry.pack(fill="x", pady=(2, 8))

        row = tk.Frame(frame, bg=BG)
        row.pack(fill="x")
        tk.Label(row, text="Axis (0-8)", bg=BG, fg=MUTED, font=FONT_SM).pack(side="left")
        self.axis_var = tk.IntVar(value=1)
        tk.Spinbox(row, from_=0, to=8, textvariable=self.axis_var, width=4, font=FONT_MED, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1).pack(side="left", padx=(8, 0))

        self._button(frame, "Create Piece", self._on_create).pack(anchor="w", pady=(12, 8))

        self.piece_out = tk.Text(frame, height=10, bg=SURFACE, fg=TEXT, font=FONT_SM, wrap="word", relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.piece_out.pack(fill="both", expand=True)

    def _tab_verify(self, nb: ttk.Notebook) -> None:
        frame = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(frame, text="Verify Bond")

        g = tk.Frame(frame, bg=BG)
        g.pack(fill="x")
        l = tk.Frame(g, bg=BG)
        l.pack(side="left", fill="x", expand=True)
        r = tk.Frame(g, bg=BG)
        r.pack(side="right", fill="x", expand=True, padx=(10, 0))

        tk.Label(l, text="Agent A — fp", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w")
        self.vfp_a = tk.Entry(l, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.vfp_a.insert(0, "a3f0b2c1d4e5f6a7")
        self.vfp_a.pack(fill="x", pady=(2, 4))
        tk.Label(l, text="Axis", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w")
        self.vax_a = tk.Spinbox(l, from_=0, to=8, width=4, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.vax_a.delete(0, "end")
        self.vax_a.insert(0, "1")
        self.vax_a.pack(anchor="w", pady=(2, 0))

        tk.Label(r, text="Agent B — fp", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w")
        self.vfp_b = tk.Entry(r, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.vfp_b.insert(0, "a3f0b2c1d4e5f6a7")
        self.vfp_b.pack(fill="x", pady=(2, 4))
        tk.Label(r, text="Axis", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w")
        self.vax_b = tk.Spinbox(r, from_=0, to=8, width=4, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.vax_b.delete(0, "end")
        self.vax_b.insert(0, "3")
        self.vax_b.pack(anchor="w", pady=(2, 0))

        self._button(frame, "Verify", self._on_verify).pack(anchor="w", pady=(12, 8))

        self.verify_out = tk.Text(frame, height=10, bg=SURFACE, fg=TEXT, font=FONT_SM, wrap="word", relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.verify_out.pack(fill="both", expand=True)

    def _tab_wallet(self, nb: ttk.Notebook) -> None:
        frame = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(frame, text="Wallet")

        tk.Label(frame, text="Each line:  fp,axis,agent_id", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w")
        self.wallet_input = tk.Text(frame, height=5, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.wallet_input.insert("1.0", "a3f0b2c1d4e5f6a7,1,1\na3f0b2c1d4e5f6a7,3,2\ndeadbeefcafe0002,0,3")
        self.wallet_input.pack(fill="x", pady=(4, 8))

        self._button(frame, "Build Wallet", self._on_wallet).pack(anchor="w", pady=(0, 8))

        self.wallet_out = tk.Text(frame, height=12, bg=SURFACE, fg=TEXT, font=FONT_SM, wrap="word", relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.wallet_out.pack(fill="both", expand=True)

    def _tab_health(self, nb: ttk.Notebook) -> None:
        frame = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(frame, text="Health")

        self._button(frame, "Check Health", self._on_health).pack(anchor="w", pady=(0, 8))

        self.health_out = tk.Text(frame, height=10, bg=SURFACE, fg=TEXT, font=FONT_SM, wrap="word", relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.health_out.pack(fill="both", expand=True)

    def _tab_about(self, nb: ttk.Notebook) -> None:
        frame = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(frame, text="About")
        lines = [
            "Bond Layer v1.1",
            "POGLS geometry-native bond gateway",
            "",
            "Context Menu:",
            "  Right-click a file > 'Bond Layer' to open directly",
            "",
            "Usage:",
            "  1. Click 'Browse' to pick a file, then 'Scan File'",
            "  2. Scan auto-generates topology_fp from file content",
            "  3. Switch to 'Create Piece' tab, adjust axis, click Create",
            "  4. Use 'Verify Bond' to compare two agents",
        ]
        for line in lines:
            tk.Label(frame, text=line, bg=BG, fg=TEXT if not line.startswith("  ") else MUTED, font=FONT_SM, anchor="w", justify="left").pack(fill="x", pady=1)

    def _button(self, parent, text, command, primary=False):
        bg = CHOCO if primary else SURFACE
        fg = "#fff" if primary else CHOCO
        hover = CHOCO_D if primary else CREAM
        btn = tk.Button(parent, text=text, command=command, bg=bg, fg=fg, font=FONT_SM, relief="flat", bd=0, padx=12, pady=5, activebackground=hover, activeforeground=fg, cursor="hand2")
        btn.bind("<Enter>", lambda _: btn.config(bg=hover))
        btn.bind("<Leave>", lambda _: btn.config(bg=bg))
        return btn

    def _out(self, widget, data):
        widget.delete("1.0", "end")
        if isinstance(data, dict) and "error" in data:
            widget.insert("1.0", f"ERROR: {data['error']}")
        else:
            widget.insert("1.0", json.dumps(data, indent=2))

    def _set_status(self, msg):
        self.status_var.set(msg)

    def _on_browse(self):
        path = filedialog.askopenfilename(title="Select a file for topology scan")
        if path:
            self.file_path_entry.delete(0, "end")
            self.file_path_entry.insert(0, path)
            self._on_scan()

    def _on_scan(self):
        path = self.file_path_entry.get().strip()
        if not path or not os.path.isfile(path):
            self.scan_info.config(text="File not found", fg=WARN_FG)
            return
        self._set_status("Scanning file...")
        try:
            fp, topo = _scan_fp(path)
            self._scan_result = (fp, topo)
            self.fp_entry.delete(0, "end")
            self.fp_entry.insert(0, fp)
            ct = topo.get("content_type", "?")
            rh = topo.get("routing_hint", "?")
            cs = topo.get("best_scale", "?")
            self.scan_info.config(text=f"fp={fp}  type={ct}  hint={rh}  scale={cs}", fg=GREEN)
            self._set_status(f"Scanned: {Path(path).name} → fp={fp}")
        except Exception as e:
            self.scan_info.config(text=f"Scan failed: {e}", fg=WARN_FG)
            self._set_status("Scan failed")

    def _on_create(self):
        self._set_status("Creating piece...")
        fp = self.fp_entry.get().strip()
        axis = self.axis_var.get()
        result = api_post("/bond/create-piece", {"fp": fp, "axis": axis})
        self._out(self.piece_out, result)
        if "error" not in result:
            self._set_status(f"Piece: shape={result.get('shape')} key={result.get('bond_key','')[:18]}")

    def _on_verify(self):
        self._set_status("Verifying...")
        body = {"fp_a": self.vfp_a.get().strip(), "axis_a": int(self.vax_a.get()), "fp_b": self.vfp_b.get().strip(), "axis_b": int(self.vax_b.get())}
        result = api_post("/bond/verify", body)
        self._out(self.verify_out, result)
        if "error" not in result:
            ok = result.get("bond_valid", False)
            self._set_status(f"Bond {'VALID' if ok else 'INVALID'}")

    def _on_wallet(self):
        self._set_status("Building wallet...")
        lines = self.wallet_input.get("1.0", "end").strip().split("\n")
        agents = []
        for i, line in enumerate(lines):
            line = line.strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            agents.append({"fp": parts[0], "axis": int(parts[1]) if len(parts) > 1 else 1, "agent_id": int(parts[2]) if len(parts) > 2 else i})
        result = api_post("/bond/wallet", {"agents": agents})
        self._out(self.wallet_out, result)
        self._set_status(f"Wallet: {len(result.get('agents', []))} agents")

    def _on_health(self):
        self._set_status("Checking health...")
        result = api_post("/bond/health", {})
        self._out(self.health_out, result)
        if "error" not in result:
            ver = result.get("version", "?")
            self._set_status(f"Bond v{ver} — {'OK' if result.get('loaded') else 'FAIL'}")

    def run(self) -> None:
        self.root.mainloop()


if __name__ == "__main__":
    file_arg = sys.argv[1] if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]) else ""
    if not file_arg:
        # Also try context menu "Install" / "Uninstall"
        if len(sys.argv) > 1 and sys.argv[1].lower() in ("install", "--install"):
            if install_context_menu():
                print("Bond Layer context menu installed!")
                tk.messagebox.showinfo("Bond Layer", "Context menu installed!\nRight-click any file > Bond Layer")
            else:
                print("Install failed (not Windows?)")
            sys.exit(0)
        if len(sys.argv) > 1 and sys.argv[1].lower() in ("uninstall", "--uninstall"):
            if uninstall_context_menu():
                print("Context menu removed!")
            else:
                print("Uninstall failed")
            sys.exit(0)
    app = BondWindow(file_arg=file_arg)
    app.run()
