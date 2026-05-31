from __future__ import annotations

import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
import tkinter as tk
from datetime import datetime, timedelta
from pathlib import Path
from tkinter import filedialog, ttk
from typing import Any, Optional

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

TRASH_EXTS = {
    ".tmp", ".temp", ".log", ".cache", ".bak", ".old",
    ".etl", ".etx", ".blf", ".regtrans-ms", ".log1", ".log2",
    ".chk", ".dat", ".wer", ".hdmp", ".mdmp", ".minidmp",
}

SKIP_EXTS = {".lnk", ".pyc", ".pyo", ".pyd", ".idx", ".pack"}

# Cache directory names — only these trigger "cache" category
CACHE_DIRS = {"__pycache__", ".cache", "cache", "cached", "caches"}

TRASH_DIRS = {"temp", "tmp", "crashdumps", "wer", "recent"}

# Directories that suggest a file is a copy/backup, not the original
COPY_DIR_MARKERS = {".vault", "backup", "backups", "copies", "duplicate", "old"}


def fmt_bytes(n: int) -> str:
    v = float(n)
    for u in ("B", "KB", "MB", "GB", "TB"):
        if v < 1024:
            return f"{v:.1f} {u}"
        v /= 1024
    return f"{v:.1f} PB"


try:
    from smart_folder_mvp.pogls_bridge import topology_scan_multiscale
    _CAN_SCAN = True
except Exception:
    _CAN_SCAN = False


def _stable_id(text: str, length: int = 16) -> str:
    return hashlib.sha256(text.encode()).hexdigest()[:length]


def _prefer_keep(flist: list) -> int:
    """Pick best file to keep: prefer non-vault/backup path, else first."""
    best = 0
    for i, f in enumerate(flist):
        parts = Path(f.path).parts
        if not any(m in p.lower() for p in parts for m in COPY_DIR_MARKERS):
            best = i
            break
    return best


def send2recycle(path: str) -> bool:
    """Move file to Windows Recycle Bin (no permanent delete)."""
    try:
        subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"Add-Type -AssemblyName Microsoft.VisualBasic;"
             f"[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile("
             f"'{path.replace(chr(39), chr(39)+chr(39)+chr(39))}',"
             f"'OnlyErrorDialogs','SendToRecycleBin')"],
            capture_output=True, timeout=10, creationflags=0x08000000,
        )
        return True
    except Exception:
        return False


def load_exclude_patterns(path: str) -> list[str]:
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
        return [l.strip() for l in lines if l.strip() and not l.strip().startswith("#")]
    except Exception:
        return []


def matches_exclude(path: str, patterns: list[str]) -> bool:
    for pat in patterns:
        if pat in path:
            return True
        try:
            if re.search(pat.replace("*", ".*").replace("?", "."), path, re.I):
                return True
        except Exception:
            pass
    return False


class FileInfo:
    __slots__ = ("path", "size", "mtime", "ext", "category", "fp", "topo", "checked")
    def __init__(self, path: str, size: int, mtime: float, ext: str):
        self.path = path
        self.size = size
        self.mtime = mtime
        self.ext = ext.lower()
        self.category = self._classify()
        self.fp = ""
        self.topo = None
        self.checked = False

    def _dir_names(self) -> list[str]:
        return [p.lower() for p in Path(self.path).parts[:-1]]

    def _classify(self) -> str:
        dirs = self._dir_names()
        name = Path(self.path).name.lower()
        ext = self.ext

        if ext in TRASH_EXTS:
            return "trash"
        if ext in (".tmp", ".temp", ".bak", ".old"):
            return "trash"
        if any(d in TRASH_DIRS for d in dirs):
            return "trash"
        if any(d in CACHE_DIRS for d in dirs):
            return "cache"
        if ext == ".log":
            return "log"
        if "crash" in name or "dump" in name:
            return "trash"
        if name.endswith(".dmp") or name.endswith(".hdmp") or name.endswith(".mdmp"):
            return "trash"
        return "normal"

    def scan(self):
        if not _CAN_SCAN:
            return
        try:
            data = Path(self.path).read_bytes()[:65536]
            if not data:
                return
            h = hashlib.sha256(data).hexdigest()
            self.fp = _stable_id(f"{self.size}:{h}", length=16)
            self.topo = topology_scan_multiscale(data)
        except Exception:
            pass


class SortableTree(ttk.Treeview):
    """Treeview with click-to-sort on headers."""
    def __init__(self, master=None, **kw):
        super().__init__(master, **kw)
        self._sort_col = None
        self._sort_rev = False
        self._orig_data: list[tuple] = []
        self._heading_labels: dict[str, str] = {}

    def _on_heading_click(self, col_key: str):
        if self._sort_col == col_key:
            self._sort_rev = not self._sort_rev
        else:
            self._sort_col = col_key
            self._sort_rev = False
        self._do_sort(col_key, self._sort_rev)

    def _sort_key(self, val: str):
        """Parse formatted values for numeric sorting."""
        v = val.strip()
        # byte format: "9.0 B", "1.2 KB", "3.5 MB", etc.
        units = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3, "TB": 1024**4}
        for u, m in units.items():
            if v.endswith(" " + u):
                try:
                    return float(v[:-len(u)-1]) * m
                except ValueError:
                    break
        # plain number
        try:
            return float(v)
        except ValueError:
            return v.lower()

    def _do_sort(self, col_key: str, reverse: bool):
        items = [(self.set(k, col_key), k) for k in self.get_children("")]
        items.sort(key=lambda x: self._sort_key(x[0]), reverse=reverse)
        for idx, (_, k) in enumerate(items):
            self.move(k, "", idx)
        self._refresh_heading()

    def _refresh_heading(self):
        for col in self["columns"]:
            base = self._heading_labels.get(col, col.replace("_", " ").title())
            txt = base + (" ▲" if col == self._sort_col and not self._sort_rev
                          else " ▼" if col == self._sort_col and self._sort_rev
                          else "")
            self.heading(col, text=txt)

    def setup_columns(self, cols: list[str], col_widths: list[int] | None = None):
        """Call once after creation to set headings + sort commands."""
        self["columns"] = cols
        for i, c in enumerate(cols):
            label = c.replace("_", " ").title()
            self._heading_labels[c] = label
            kw = {"text": label, "command": lambda k=c: self._on_heading_click(k)}
            self.heading(c, **kw)
            if col_widths and i < len(col_widths):
                self.column(c, width=col_widths[i])

    def populate(self, rows: list[tuple], filter_text: str = ""):
        self._orig_data = rows
        self._apply_filter(filter_text)
        self._refresh_heading()

    def _apply_filter(self, text: str = ""):
        for i in self.get_children():
            self.delete(i)
        for row in self._orig_data:
            if not text or text.lower() in " ".join(str(v) for v in row).lower():
                self.insert("", "end", values=row)


class StorageCleaner:
    def __init__(self) -> None:
        self.root = tk.Tk()
        self.root.title("Storage Cleaner")
        self.root.configure(bg=BG)
        self.root.geometry("1020x760")
        self.root.minsize(720, 520)
        self.dirs: list[str] = []
        self.files: list[FileInfo] = []
        self.exclude_patterns: list[str] = []
        self._stop = False
        self._build_ui()

    def _build_ui(self) -> None:
        header = tk.Frame(self.root, bg=CHOCO_D, padx=16, pady=10)
        header.pack(fill="x")
        tk.Label(header, text="Storage Cleaner", bg=CHOCO_D, fg="#ffffff", font=("Georgia", 14)).pack(side="left")
        tk.Label(header, text="TOPOLOGY STORAGE ANALYZER", bg=CHOCO_D, fg=CHOCO_L, font=("Consolas", 8)).pack(side="left", padx=(10, 0), pady=2)

        # ── Control bar ──────────────────────────────────────────
        ctrl = tk.Frame(self.root, bg=BG, padx=16, pady=6)
        ctrl.pack(fill="x")
        row1 = tk.Frame(ctrl, bg=BG)
        row1.pack(fill="x")
        self._btn(row1, "Add Directory", self._on_add_dir).pack(side="left", padx=(0, 4))
        self._btn(row1, "Clear All", self._on_clear).pack(side="left", padx=4)
        self._btn(row1, "Start Scan", self._on_scan, primary=True).pack(side="left", padx=4)
        self._btn(row1, "Clean Selected", self._on_clean).pack(side="left", padx=4)
        self._btn(row1, "Undo All", self._on_undo).pack(side="left", padx=4)
        self._btn(row1, "Exclude", self._on_exclude).pack(side="right", padx=4)

        row2 = tk.Frame(ctrl, bg=BG)
        row2.pack(fill="x", pady=(4, 0))
        self.dir_list = tk.Listbox(row2, height=3, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1, bg=SURFACE)
        self.dir_list.pack(side="left", fill="x", expand=True)

        # ── Filter + Progress ────────────────────────────────────
        ftop = tk.Frame(self.root, bg=BG, padx=16)
        ftop.pack(fill="x")
        tk.Label(ftop, text="Filter:", bg=BG, fg=MUTED, font=FONT_SM).pack(side="left", padx=(0, 6))
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", lambda *_: self._apply_filter())
        tk.Entry(ftop, textvariable=self.filter_var, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1).pack(side="left", fill="x", expand=True, padx=(0, 8))
        self.result_label = tk.Label(ftop, text="", bg=BG, fg=MUTED, font=FONT_SM)
        self.result_label.pack(side="right")

        self.progress = ttk.Progressbar(self.root, mode="determinate")
        self.progress.pack(fill="x", padx=16)

        # ── Tabs ─────────────────────────────────────────────────
        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=8, pady=4)
        style = ttk.Style()
        style.theme_use("default")
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=CREAM, foreground=CHOCO, font=FONT_SM, padding=[10, 4])
        style.map("TNotebook.Tab", background=[("selected", SURFACE)])

        self._tab_summary(nb)
        self._tab_dupes(nb)
        self._tab_trash(nb)
        self._tab_files(nb)

        # ── Status ────────────────────────────────────────────────
        st = tk.Frame(self.root, bg=CREAM, padx=12, pady=6)
        st.pack(fill="x")
        self.status_var = tk.StringVar(value="Ready")
        tk.Label(st, textvariable=self.status_var, bg=CREAM, fg=MUTED, font=FONT_SM).pack(side="left")
        self.status2_var = tk.StringVar(value="")
        tk.Label(st, textvariable=self.status2_var, bg=CREAM, fg=MUTED, font=FONT_SM).pack(side="right")

    def _btn(self, parent, text, command, primary=False):
        bg = CHOCO if primary else SURFACE
        fg = "#fff" if primary else CHOCO
        hover = CHOCO_D if primary else CREAM
        b = tk.Button(parent, text=text, command=command, bg=bg, fg=fg, font=FONT_SM, relief="flat", bd=0, padx=12, pady=5, activebackground=hover, activeforeground=fg, cursor="hand2")
        b.bind("<Enter>", lambda _: b.config(bg=hover))
        b.bind("<Leave>", lambda _: b.config(bg=bg))
        return b

    def _make_table(self, parent, cols: list[str], col_widths: Optional[list[int]] = None) -> SortableTree:
        t = SortableTree(parent, show="headings", selectmode="extended")
        t.setup_columns(cols, col_widths)
        return t

    # ── Tabs ─────────────────────────────────────────────────────

    def _tab_summary(self, nb: ttk.Notebook) -> None:
        f = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(f, text="Summary")
        self.summary_text = tk.Text(f, height=18, bg=SURFACE, fg=TEXT, font=FONT_MED, wrap="word", relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        self.summary_text.pack(fill="both", expand=True)
        self.summary_text.insert("1.0", "Add directories and click 'Start Scan'")

    def _tab_dupes(self, nb: ttk.Notebook) -> None:
        f = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(f, text="Duplicates")
        cols = ("sel", "name", "size", "count", "wasted", "fp", "paths")
        w = [35, 180, 80, 55, 90, 130, 400]
        self.dupe_tree = self._make_table(f, cols, w)
        self.dupe_tree.pack(fill="both", expand=True)
        self.dupe_tree.bind("<Double-1>", self._on_dupe_click)
        self.dupe_tree.bind("<ButtonRelease-1>", self._on_dupe_toggle)

    def _tab_trash(self, nb: ttk.Notebook) -> None:
        f = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(f, text="Trash & Temp")
        cols = ("sel", "size", "category", "age", "path")
        w = [35, 80, 80, 70, 490]
        self.trash_tree = self._make_table(f, cols, w)
        self.trash_tree.pack(fill="both", expand=True)
        self.trash_tree.bind("<ButtonRelease-1>", self._on_trash_toggle)

    def _tab_files(self, nb: ttk.Notebook) -> None:
        f = tk.Frame(nb, bg=BG, padx=16, pady=16)
        nb.add(f, text="All Files")
        cols = ("size", "ext", "type", "hint", "fp", "path")
        w = [80, 45, 60, 60, 120, 380]
        self.file_tree = self._make_table(f, cols, w)
        self.file_tree.pack(fill="both", expand=True)

    # ── Actions ──────────────────────────────────────────────────

    def _on_add_dir(self):
        d = filedialog.askdirectory(title="Select directory to scan")
        if d and d not in self.dirs:
            self.dirs.append(d)
            self.dir_list.insert("end", d)

    def _on_clear(self):
        self.dirs.clear()
        self.dir_list.delete(0, "end")
        self.files.clear()
        for t in (self.dupe_tree, self.trash_tree, self.file_tree):
            t.delete(*t.get_children())
        self.summary_text.delete("1.0", "end")
        self.summary_text.insert("1.0", "Add directories and click 'Start Scan'")

    def _on_exclude(self):
        if not self.dirs:
            self._set_status("Add a directory first")
            return
        ep = Path(self.dirs[0]) / ".storage_cleaner_exclude"
        patterns = load_exclude_patterns(str(ep)) if ep.exists() else list(self.exclude_patterns)
        w = tk.Toplevel(self.root)
        w.title("Exclude Patterns")
        w.geometry("560x400")
        w.configure(bg=BG)
        top = tk.Frame(w, bg=BG, padx=10, pady=6)
        top.pack(fill="x")
        tk.Label(top, text=f"Exclude for: {self.dirs[0]}", bg=BG, fg=MUTED, font=FONT_SM, wraplength=520).pack(anchor="w")
        tk.Label(top, text="One pattern per line (matches path substring):", bg=BG, fg=MUTED, font=FONT_SM).pack(anchor="w", pady=(4, 0))
        mid = tk.Frame(w, bg=BG, padx=10, pady=4)
        mid.pack(fill="both", expand=True)
        btn_frame = tk.Frame(mid, bg=BG)
        btn_frame.pack(side="right", fill="y", padx=(8, 0))
        list_frame = tk.Frame(mid, bg=SURFACE, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        list_frame.pack(fill="both", expand=True, side="left")
        scroll = tk.Scrollbar(list_frame, orient="vertical")
        lb = tk.Listbox(list_frame, font=FONT_SM, bg=SURFACE, fg=TEXT, relief="flat", bd=0,
                        highlightthickness=0, yscrollcommand=scroll.set)
        scroll.config(command=lb.yview)
        scroll.pack(side="right", fill="y")
        lb.pack(fill="both", expand=True)
        for pat in patterns:
            lb.insert("end", pat)
        entry = tk.Entry(w, font=FONT_SM, relief="flat", bd=1, highlightbackground=BORDER, highlightthickness=1)
        entry.pack(fill="x", padx=10, pady=(0, 4))
        def _add():
            val = entry.get().strip()
            if val and val not in lb.get(0, "end"):
                lb.insert("end", val)
                entry.delete(0, "end")
        def _remove():
            sel = lb.curselection()
            if sel:
                lb.delete(sel[0])
        def _save():
            self.exclude_patterns = list(lb.get(0, "end"))
            ep.parent.mkdir(parents=True, exist_ok=True)
            ep.write_text("\n".join(self.exclude_patterns))
            w.destroy()
        self._btn(btn_frame, "Add", _add).pack(fill="x", pady=(0, 4))
        self._btn(btn_frame, "Remove", _remove).pack(fill="x", pady=(0, 4))
        self._btn(btn_frame, "Save", _save, primary=True).pack(fill="x", pady=(0, 4))
        self._btn(btn_frame, "Cancel", w.destroy).pack(fill="x")

    def _set_status(self, msg):
        self.status_var.set(msg)
        self.root.update_idletasks()

    def _apply_filter(self):
        txt = self.filter_var.get()
        for t in (self.dupe_tree, self.trash_tree, self.file_tree):
            if hasattr(t, "_apply_filter"):
                t._apply_filter(txt)

    def _on_scan(self):
        if not self.dirs:
            self._set_status("Add at least one directory first")
            return
        self.files.clear()
        self._stop = False
        self.progress["value"] = 0
        self._set_status("Scanning...")
        # Load exclude patterns
        self.exclude_patterns = []
        for d in self.dirs:
            ep = Path(d) / ".storage_cleaner_exclude"
            if ep.exists():
                self.exclude_patterns.extend(load_exclude_patterns(str(ep)))
        t = threading.Thread(target=self._scan_thread, daemon=True)
        t.start()

    def _scan_thread(self):
        all_files: list[FileInfo] = []
        total_walked = 0

        # Pass 1 — walk
        for d in self.dirs:
            if self._stop:
                break
            for dirpath, _, filenames in os.walk(d):
                if self._stop:
                    break
                for fn in filenames:
                    ext = Path(fn).suffix.lower()
                    if ext in SKIP_EXTS:
                        continue
                    fp = os.path.join(dirpath, fn)
                    if self.exclude_patterns and matches_exclude(fp, self.exclude_patterns):
                        continue
                    try:
                        st = os.stat(fp)
                        if st.st_size < 256:
                            continue
                        all_files.append(FileInfo(fp, st.st_size, st.st_mtime, ext))
                        total_walked += 1
                        if total_walked % 5000 == 0:
                            self._set_status(f"Walking: {total_walked} files...")
                    except Exception:
                        pass
        if self._stop or not all_files:
            self.files = all_files
            self.root.after(0, self._show_results)
            return

        # Pass 2 — topology scan (parallel, only potential dupes)
        size_ext: dict[tuple[int, str], list[int]] = {}
        for i, f in enumerate(all_files):
            size_ext.setdefault((f.size, f.ext), []).append(i)
        scan_indices = set()
        for key, indices in size_ext.items():
            if len(indices) > 1:
                scan_indices.update(indices)

        scanned, total_scan = 0, len(scan_indices)
        self._set_status(f"Walking done ({total_walked} files). Scanning topology ({total_scan} candidates)...")
        if total_scan > 0:
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                futs = {pool.submit(all_files[i].scan): i for i in scan_indices}
                for fut in concurrent.futures.as_completed(futs):
                    if self._stop:
                        break
                    scanned += 1
                    if scanned % 200 == 0:
                        pct = int(scanned / total_scan * 100)
                        self.progress["value"] = int(scanned / total_scan * 100)
                        self.status2_var.set(f"Topology: {scanned}/{total_scan} ({pct}%)")
                        self.root.update_idletasks()
        self.files = all_files
        self.root.after(0, self._show_results)

    def _show_results(self):
        self.progress["value"] = 100
        self._set_status(f"Done — {len(self.files)} files")
        self.status2_var.set("")
        self._populate_summary()
        self._populate_dupes()
        self._populate_trash()
        self._populate_files()
        self.result_label.config(text=f"{len(self.files)} files")

    def _populate_summary(self):
        t = self.summary_text
        t.delete("1.0", "end")
        total = sum(f.size for f in self.files)
        trash = [f for f in self.files if f.category in ("trash", "temp", "log", "cache")]
        trash_sz = sum(f.size for f in trash)
        by_fp: dict[tuple[int, str], list[FileInfo]] = {}
        for f in self.files:
            if f.fp:
                by_fp.setdefault((f.size, f.fp), []).append(f)
        dupes = {k: v for k, v in by_fp.items() if len(v) > 1}
        wasted = sum((len(v) - 1) * v[0].size for v in dupes.values())
        by_ext: dict[str, int] = {}
        for f in self.files:
            by_ext[f.ext] = by_ext.get(f.ext, 0) + f.size
        top_ext = sorted(by_ext.items(), key=lambda x: -x[1])[:10]
        lines = [
            f"Scanned:     {len(self.dirs)} directories, {len(self.files)} files",
            f"Total size:  {fmt_bytes(total)}",
            "",
            f"Trash:       {len(trash)} files  {fmt_bytes(trash_sz)}",
            f"Duplicates:  {len(dupes)} groups  {fmt_bytes(wasted)} wasted ({(wasted/total*100 if total else 0):.1f}%)",
            "",
        ]
        for ext, sz in top_ext:
            pct = sz / total * 100 if total else 0
            lines.append(f"  {ext or '(no ext)':>12s}  {fmt_bytes(sz):>10s}  ({pct:.1f}%)")
        t.insert("1.0", "\n".join(lines))

    def _populate_dupes(self):
        t = self.dupe_tree
        for i in t.get_children():
            t.delete(i)
        by_key: dict[tuple[int, str], list[FileInfo]] = {}
        for f in self.files:
            if f.fp:
                by_key.setdefault((f.size, f.fp), []).append(f)
        rows = []
        for (fsize, fp), flist in sorted(by_key.items(), key=lambda x: -len(x[1])):
            if len(flist) < 2:
                continue
            wasted = (len(flist) - 1) * fsize
            name = Path(flist[0].path).name
            shown = "\n".join(f.path for f in flist[:3])
            if len(flist) > 3:
                shown += f"\n... +{len(flist)-3} more"
            rows.append(("☐", name, fmt_bytes(fsize), len(flist), fmt_bytes(wasted), f"{fsize}:{fp}", shown))
        t.populate(rows)

    def _populate_trash(self):
        t = self.trash_tree
        for i in t.get_children():
            t.delete(i)
        now = time.time()
        rows = []
        for f in sorted(self.files, key=lambda x: -x.size):
            if f.category not in ("trash", "temp", "log", "cache"):
                continue
            age = now - f.mtime
            age_s = f"{int(age/86400)}d" if age > 86400 else f"{int(age/3600)}h"
            rows.append(("☐", fmt_bytes(f.size), f.category, age_s, f.path))
        t.populate(rows)

    def _populate_files(self):
        t = self.file_tree
        for i in t.get_children():
            t.delete(i)
        rows = []
        for f in sorted(self.files, key=lambda x: -x.size)[:10000]:
            ct = f.topo.get("content_type", "?") if f.topo else "?"
            rh = f.topo.get("routing_hint", "?") if f.topo else "?"
            rows.append((fmt_bytes(f.size), f.ext, ct[:8], rh[:8], f.fp or "", f.path))
        t.populate(rows)

    # ── Checkbox toggle ──────────────────────────────────────────

    def _on_dupe_toggle(self, e):
        if self.dupe_tree.identify_region(e.x, e.y) != "cell":
            return
        col = int(self.dupe_tree.identify_column(e.x).replace("#", "")) - 1
        if col != 0:
            return
        sel = self.dupe_tree.identify_row(e.y)
        if not sel:
            return
        cur = self.dupe_tree.item(sel, "values")
        new = "☑" if cur[0] == "☐" else "☐"
        self.dupe_tree.item(sel, values=(new, *cur[1:]))

    def _on_trash_toggle(self, e):
        if self.trash_tree.identify_region(e.x, e.y) != "cell":
            return
        col = int(self.trash_tree.identify_column(e.x).replace("#", "")) - 1
        if col != 0:
            return
        sel = self.trash_tree.identify_row(e.y)
        if not sel:
            return
        cur = self.trash_tree.item(sel, "values")
        new = "☑" if cur[0] == "☐" else "☐"
        self.trash_tree.item(sel, values=(new, *cur[1:]))

    # ── Clean ────────────────────────────────────────────────────

    def _selected_checked(self, tree) -> list[str]:
        paths = []
        for item in tree.get_children():
            vals = tree.item(item, "values")
            if vals and vals[0] == "☑":
                paths.append(vals[-1])
        return paths

    def _on_clean(self):
        trash_paths = self._selected_checked(self.trash_tree)
        p = self.dupe_tree.get_children()
        # Handle trash
        if trash_paths:
            total = sum(os.path.getsize(p) for p in trash_paths if os.path.exists(p))
            if not tk.messagebox.askyesno("Confirm", f"Send {len(trash_paths)} files ({fmt_bytes(total)}) to Recycle Bin?"):
                return
            ok = fail = 0
            for p in trash_paths:
                if send2recycle(p):
                    ok += 1
                else:
                    fail += 1
            self._set_status(f"Sent {ok} to Recycle Bin{f' ({fail} failed)' if fail else ''}")

        # Handle dupes (checked items → hardlink)
        for item in self.dupe_tree.get_children():
            vals = self.dupe_tree.item(item, "values")
            if not vals or vals[0] != "☑":
                continue
            parts = vals[5].split(":", 1)
            if len(parts) != 2:
                continue
            fsize, fp = int(parts[0]), parts[1]
            flist = [f for f in self.files if f.fp == fp and f.size == fsize]
            if len(flist) < 2:
                continue
            # Check if already same inode (already hardlinked)
            inodes = set()
            exist = [f for f in flist if os.path.exists(f.path)]
            for f in exist:
                try:
                    inodes.add(os.stat(f.path).st_ino)
                except Exception:
                    pass
            if len(inodes) <= 1:
                self._set_status(f"All {len(flist)} files already on same inode, nothing to do")
                continue
            # Pick best keeper (prefer non-vault/backup path)
            filtered = [f for f in exist]
            keep_idx = _prefer_keep(filtered)
            keep_path = filtered[keep_idx].path
            rest = [f for f in exist if f.path != keep_path]
            saved = sum(f.size for f in rest)
            paths_preview = "\n".join(f.path for f in rest[:5])
            if len(rest) > 5:
                paths_preview += f"\n... +{len(rest)-5} more"
            if not tk.messagebox.askyesno("Confirm",
                f"Keep (original):\n  {keep_path}\n\n"
                f"Replace {len(rest)} copies with hardlinks (save {fmt_bytes(saved)}):\n{paths_preview}"):
                continue
            ok = fail = 0
            for f in rest:
                try:
                    os.remove(f.path)
                    os.link(keep_path, f.path)
                    ok += 1
                except Exception:
                    fail += 1
            self._set_status(f"Hardlinked {ok} files{f' ({fail} failed)' if fail else ''}")
        self._on_scan()

    def _on_undo(self):
        """Open Recycle Bin for manual restore."""
        subprocess.run(["explorer", "shell:RecycleBinFolder"])

    def _on_dupe_click(self, e):
        sel = self.dupe_tree.selection()
        if not sel:
            return
        vals = self.dupe_tree.item(sel[0], "values")
        parts = vals[5].split(":", 1)
        if len(parts) != 2:
            return
        fsize, fp = int(parts[0]), parts[1]
        files = [f for f in self.files if f.fp == fp and f.size == fsize]
        msg = f"Duplicate group: size={fsize} fp={fp}\nName: {vals[1]}\nCount: {vals[3]}  Wasted: {vals[4]}\n\nFiles:\n"
        for f in files:
            msg += f"\n  {f.path}"
        tk.messagebox.showinfo("Duplicate Details", msg)

    def run(self) -> None:
        self.root.mainloop()


if __name__ == "__main__":
    app = StorageCleaner()
    app.run()
