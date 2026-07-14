#!/usr/bin/env python3
"""
GeoPixel One-Stop Service — tkinter GUI (v2)
════════════════════════════════════════════

Encode any file through: GeoField → Skeleton → GPXL v2
Decode .geopixel back to original file.

Features:
  - Auto-detect input type (raw file vs .geopixel)
  - Auto-fix extension
  - Swap input↔output
  - GPXL v2 format (xxh64, CoordRecords)
  - Dynamic cube sizing (base 4)
  - Fallback to raw if ratio < 0.5x
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import os, sys, time, threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from geopixel_pipeline import (
    pipeline_encode, pipeline_decode, decode_gpxl, encode_gpxl,
    GPXL_MAGIC, GPXL_HEADER_SZ, SKEL_NAMES,
    xxh64, struct, CHUNK_SZ
)

class GeoPixelApp:
    def __init__(self, root):
        self.root = root
        self.root.title("GeoPixel Service v2")
        self.root.geometry("650x580")
        self.root.resizable(True, True)

        self.input_path = tk.StringVar()
        self.output_path = tk.StringVar()
        self.mode = tk.StringVar(value="auto")

        self._build_ui()

    def _build_ui(self):
        pad = {'padx': 8, 'pady': 4}

        # ── Input ──
        ttk.Label(self.root, text="Input:").grid(row=0, column=0, sticky='w', **pad)
        ttk.Entry(self.root, textvariable=self.input_path, width=55).grid(row=0, column=1, sticky='ew', **pad)
        ttk.Button(self.root, text="Browse", command=self._browse_input).grid(row=0, column=2, **pad)

        # ── Output ──
        ttk.Label(self.root, text="Output:").grid(row=1, column=0, sticky='w', **pad)
        ttk.Entry(self.root, textvariable=self.output_path, width=55).grid(row=1, column=1, sticky='ew', **pad)
        ttk.Button(self.root, text="Browse", command=self._browse_output).grid(row=1, column=2, **pad)

        # ── Mode ──
        mode_frame = ttk.Frame(self.root)
        mode_frame.grid(row=2, column=0, columnspan=3, sticky='w', **pad)
        ttk.Radiobutton(mode_frame, text="Auto-detect", variable=self.mode, value="auto").pack(side='left')
        ttk.Radiobutton(mode_frame, text="Encode", variable=self.mode, value="encode").pack(side='left')
        ttk.Radiobutton(mode_frame, text="Decode", variable=self.mode, value="decode").pack(side='left')

        # ── Buttons row ──
        btn_frame = ttk.Frame(self.root)
        btn_frame.grid(row=3, column=0, columnspan=3, **pad)
        ttk.Button(btn_frame, text="Swap ↔", command=self._swap).pack(side='left', padx=4)
        self.process_btn = ttk.Button(btn_frame, text="Process", command=self._process_thread)
        self.process_btn.pack(side='left', padx=4)

        # ── Status ──
        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(self.root, textvariable=self.status_var).grid(row=4, column=0, columnspan=3, sticky='w', **pad)

        # ── Log ──
        self.log = tk.Text(self.root, height=22, width=75, state='disabled', font=('Consolas', 9))
        self.log.grid(row=5, column=0, columnspan=3, sticky='nsew', **pad)
        self.root.grid_rowconfigure(5, weight=1)
        self.root.grid_columnconfigure(1, weight=1)

        # ── Bind input path change ──
        self.input_path.trace_add('write', self._on_input_change)

    def _browse_input(self):
        path = filedialog.askopenfilename(title="Select file")
        if path:
            self.input_path.set(path)
            self._auto_output(path)

    def _browse_output(self):
        path = filedialog.asksaveasfilename(title="Save as")
        if path:
            self.output_path.set(path)

    def _swap(self):
        inp = self.input_path.get()
        out = self.output_path.get()
        self.input_path.set(out)
        self.output_path.set(inp)
        self._detect_mode()

    def _on_input_change(self, *args):
        self._auto_output(self.input_path.get())
        self._detect_mode()

    def _auto_output(self, inp):
        if not inp or self.output_path.get():
            return
        if inp.endswith('.geopixel'):
            self.output_path.set(inp[:-len('.geopixel')])
        else:
            self.output_path.set(inp + '.geopixel')

    def _detect_mode(self):
        if self.mode.get() != "auto":
            return
        inp = self.input_path.get()
        if not inp:
            return
        try:
            with open(inp, 'rb') as f:
                magic = f.read(4)
            if magic == GPXL_MAGIC:
                self.mode.set("decode")
            else:
                self.mode.set("encode")
        except:
            pass

    def _log(self, msg):
        self.log.config(state='normal')
        self.log.insert('end', msg + '\n')
        self.log.see('end')
        self.log.config(state='disabled')

    def _log_clear(self):
        self.log.config(state='normal')
        self.log.delete('1.0', 'end')
        self.log.config(state='disabled')

    def _process_thread(self):
        self.process_btn.config(state='disabled')
        self.status_var.set("Processing...")
        self._log_clear()
        threading.Thread(target=self._process, daemon=True).start()

    def _process(self):
        try:
            inp = self.input_path.get()
            out = self.output_path.get()

            if not inp or not os.path.exists(inp):
                self._log("ERROR: Input file not found")
                return

            mode = self.mode.get()
            if mode == "auto":
                with open(inp, 'rb') as f:
                    magic = f.read(4)
                mode = "decode" if magic == GPXL_MAGIC else "encode"

            if mode == "encode":
                self._do_encode(inp, out)
            else:
                self._do_decode(inp, out)

        except Exception as e:
            self._log(f"ERROR: {e}")
        finally:
            self.root.after(0, lambda: self.process_btn.config(state='normal'))
            self.root.after(0, lambda: self.status_var.set("Done"))

    def _do_encode(self, inp, out):
        data = open(inp, 'rb').read()
        digest = xxh64(data)
        self._log(f"═══ GeoPixel Encode ═══")
        self._log(f"  Input:  {os.path.basename(inp)} ({len(data):,} bytes)")
        self._log(f"  xxh64:  0x{digest:016x}")

        t0 = time.perf_counter()
        result = pipeline_encode(data)
        t_enc = time.perf_counter() - t0

        s = result.stats
        skel_str = ' '.join(f'{n}={c}' for n, c in zip(SKEL_NAMES, result.skel_stats) if c > 0)
        self._log(f"  Chunks:   {s['n_chunks']}  gp_level={s['gp_level']}  base={s['base']}  side={s['cube_side']}")
        self._log(f"  Skeleton: {skel_str}")
        self._log(f"  Output:   {s['encoded_size']:,} bytes ({s['encoded_size']/1024:.1f} KB, ratio={result.ratio:.2f}x)")
        self._log(f"  Encode:   {t_enc*1000:.1f} ms")

        with open(out, 'wb') as f:
            f.write(result.encoded)

        t0 = time.perf_counter()
        with open(out, 'rb') as f:
            encoded = f.read()
        dec = pipeline_decode(encoded)
        t_dec = time.perf_counter() - t0

        ok = dec['data'] == data
        self._log(f"  Verify:   {'PASS' if ok else 'FAIL'} ({t_dec*1000:.1f} ms)")
        self._log(f"  Written:  {out}")

    def _do_decode(self, inp, out):
        data = open(inp, 'rb').read()
        self._log(f"═══ GeoPixel Decode ═══")
        self._log(f"  Input:  {os.path.basename(inp)} ({len(data):,} bytes)")

        t0 = time.perf_counter()
        dec = pipeline_decode(data)
        t_dec = time.perf_counter() - t0

        self._log(f"  Chunks:   {dec['n_chunks']}  verified: {dec['verified']}/{dec['n_chunks']}  failed: {dec['failed']}")
        if dec.get('digest'):
            self._log(f"  xxh64:    0x{dec['digest']:016x}")

        with open(out, 'wb') as f:
            f.write(dec['data'])

        self._log(f"  Decode:   {t_dec*1000:.1f} ms")
        self._log(f"  Written:  {out} ({len(dec['data']):,} bytes)")


if __name__ == '__main__':
    root = tk.Tk()
    app = GeoPixelApp(root)
    root.mainloop()
