"""
llm_memory_viewer.py

Small page-based viewer for large text/JSON/JSONL exports.

Goal:
  - open a file
  - move page by page
  - allow text selection/copy in the display area

This is intentionally simple and file-oriented.
It does not parse semantic records; it just pages through bytes.
"""

from __future__ import annotations

import math
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass
class FilePager:
    path: Path
    page_bytes: int = 131072

    def __post_init__(self) -> None:
        self._size = self.path.stat().st_size
        self._page_count = max(1, math.ceil(self._size / self.page_bytes))

    @property
    def size(self) -> int:
        return self._size

    @property
    def page_count(self) -> int:
        return self._page_count

    def read_page(self, page_index: int) -> tuple[str, int, int]:
        page_index = max(0, min(page_index, self._page_count - 1))
        start = page_index * self.page_bytes
        end = min(self._size, start + self.page_bytes)
        with self.path.open("rb") as f:
            f.seek(start)
            raw = f.read(end - start)
        text = raw.decode("utf-8", errors="replace")
        return text, start, end


def launch_gui() -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except Exception as exc:  # pragma: no cover - GUI only
        print(f"tkinter is not available: {exc}", file=sys.stderr)
        return 1

    root = tk.Tk()
    root.title("Memory Viewer")
    root.geometry("1100x760")

    pager: FilePager | None = None
    current_page = 0

    top = ttk.Frame(root, padding=10)
    top.pack(fill="x")

    file_var = tk.StringVar(value="")
    page_var = tk.StringVar(value="1")
    page_size_var = tk.StringVar(value="131072")
    info_var = tk.StringVar(value="Open a file to begin.")
    wrap_var = tk.BooleanVar(value=True)

    def set_info(text: str) -> None:
        info_var.set(text)

    def refresh_text() -> None:
        nonlocal current_page, pager
        if pager is None:
            text.delete("1.0", tk.END)
            set_info("Open a file to begin.")
            return

        current_page = max(0, min(current_page, pager.page_count - 1))
        page_text, start, end = pager.read_page(current_page)

        text.configure(state="normal")
        text.delete("1.0", tk.END)
        text.insert("1.0", page_text)
        text.configure(state="normal")

        page_var.set(str(current_page + 1))
        set_info(
            f"{pager.path.name} | page {current_page + 1}/{pager.page_count} | "
            f"bytes {start:,}-{end:,} of {pager.size:,}"
        )

    def open_file() -> None:
        nonlocal pager, current_page
        picked = filedialog.askopenfilename(
            title="Open text / JSON / JSONL file",
            filetypes=[
                ("Text exports", "*.json *.jsonl *.ndjson *.md *.markdown *.txt"),
                ("All files", "*.*"),
            ],
        )
        if not picked:
            return
        try:
            page_bytes = int(page_size_var.get().strip())
            if page_bytes <= 0:
                raise ValueError
        except ValueError:
            messagebox.showerror("Memory Viewer", "Page size must be a positive integer.")
            return

        path = Path(picked)
        try:
            pager = FilePager(path=path, page_bytes=page_bytes)
        except Exception as exc:
            messagebox.showerror("Memory Viewer", f"Could not open file: {exc}")
            return
        file_var.set(str(path))
        current_page = 0
        refresh_text()

    def prev_page() -> None:
        nonlocal current_page
        if pager is None:
            return
        current_page = max(0, current_page - 1)
        refresh_text()

    def next_page() -> None:
        nonlocal current_page
        if pager is None:
            return
        current_page = min(pager.page_count - 1, current_page + 1)
        refresh_text()

    def go_to_page() -> None:
        nonlocal current_page
        if pager is None:
            return
        try:
            requested = int(page_var.get().strip()) - 1
        except ValueError:
            messagebox.showerror("Memory Viewer", "Page number must be an integer.")
            return
        current_page = max(0, min(pager.page_count - 1, requested))
        refresh_text()

    def copy_page() -> None:
        if pager is None:
            return
        page_text, _, _ = pager.read_page(current_page)
        root.clipboard_clear()
        root.clipboard_append(page_text)
        root.update()

    def apply_wrap_mode() -> None:
        text.configure(wrap="word" if wrap_var.get() else "none")

    ttk.Button(top, text="Open File", command=open_file).pack(side="left")
    ttk.Button(top, text="Prev", command=prev_page).pack(side="left", padx=(8, 0))
    ttk.Button(top, text="Next", command=next_page).pack(side="left", padx=(8, 0))
    ttk.Button(top, text="Go", command=go_to_page).pack(side="left", padx=(8, 0))
    ttk.Button(top, text="Copy Page", command=copy_page).pack(side="left", padx=(8, 0))

    ttk.Label(top, text="Page size bytes").pack(side="left", padx=(18, 4))
    ttk.Entry(top, width=12, textvariable=page_size_var).pack(side="left")
    ttk.Label(top, text="Page #").pack(side="left", padx=(18, 4))
    ttk.Entry(top, width=8, textvariable=page_var).pack(side="left")
    ttk.Checkbutton(top, text="Wrap lines", variable=wrap_var, command=apply_wrap_mode).pack(side="left", padx=(18, 0))

    ttk.Label(root, textvariable=info_var, padding=(10, 0, 10, 8)).pack(fill="x")

    body = ttk.Frame(root, padding=(10, 0, 10, 10))
    body.pack(fill="both", expand=True)
    body.columnconfigure(0, weight=1)
    body.rowconfigure(0, weight=1)

    yscroll = ttk.Scrollbar(body, orient="vertical")
    xscroll = ttk.Scrollbar(body, orient="horizontal")
    text = tk.Text(
        body,
        wrap="word",
        undo=False,
        yscrollcommand=yscroll.set,
        xscrollcommand=xscroll.set,
        font=("Consolas", 10),
    )
    yscroll.config(command=text.yview)
    xscroll.config(command=text.xview)

    text.grid(row=0, column=0, sticky="nsew")
    yscroll.grid(row=0, column=1, sticky="ns")
    xscroll.grid(row=1, column=0, sticky="ew")

    text.insert("1.0", "Open a file to view it page by page.\n")
    apply_wrap_mode()

    root.mainloop()
    return 0


def self_test() -> int:
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "sample.jsonl"
        path.write_text("\n".join(f'{{"i": {i}, "text": "line {i}"}}' for i in range(200)), encoding="utf-8")
        pager = FilePager(path, page_bytes=128)
        assert pager.page_count > 1
        text, start, end = pager.read_page(0)
        assert "line 0" in text
        assert start == 0
        assert end > 0
        text2, _, _ = pager.read_page(pager.page_count - 1)
        assert text2
    print("[self-test] OK")
    return 0


def main(argv: list[str]) -> int:
    if argv and argv[0] == "--self-test":
        return self_test()
    return launch_gui()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
