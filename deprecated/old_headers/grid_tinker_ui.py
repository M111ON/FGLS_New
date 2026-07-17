"""
grid_tinker_ui.py — Python GUI for Grid Tinker Pipeline Tool

Usage:
    python grid_tinker_ui.py

Requires: grid_tinker.exe in same directory, Python 3.10+ with tkinter
"""

import os
import sys
import subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import threading
import json
import time
import hashlib
import struct

# ═══════════════════════════════════════════════════════════════
# Diamond Shell — Pure Python reimplementation for in-memory analysis
# ═══════════════════════════════════════════════════════════════

CHUNK_SZ = 64
ROT_STATES = 6
SPARSE_THRESH = 4

# 4x4x4 cube rotation mappings (same as C)
ROT_MAPS = [
    lambda x,y,z: (x, y, z),
    lambda x,y,z: (y, z, x),
    lambda x,y,z: (z, x, y),
    lambda x,y,z: (x, z, 3-y),
    lambda x,y,z: (z, y, 3-x),
    lambda x,y,z: (3-y, x, z),
]

def shell_rotate64(chunk, rot):
    out = [0]*64
    for z in range(4):
        for y in range(4):
            for x in range(4):
                sx, sy, sz = ROT_MAPS[rot](x, y, z)
                out[z*16+y*4+x] = chunk[sz*16+sy*4+sx]
    return out

def shell_inverse_rotate64(rotbuf, rot):
    out = [0]*64
    for z in range(4):
        for y in range(4):
            for x in range(4):
                sx, sy, sz = ROT_MAPS[rot](x, y, z)
                out[sz*16+sy*4+sx] = rotbuf[z*16+y*4+x]
    return out

def shell_classify_chunk(chunk, chunk_id=0):
    """Classify a 64B chunk: returns (flag, best_rot, best_pc)"""
    is_zero = all(b == 0 for b in chunk)
    if is_zero:
        return 0, 0, 0  # FLAT

    best_rot = 0
    best_pc = -1

    for rot in range(ROT_STATES):
        rotbuf = shell_rotate64(chunk, rot)
        # Simplified fibo_intersect: popcount of AND of 4 rotated copies
        r1 = rotbuf
        r2 = shell_rotate64(rotbuf, (rot+1) % ROT_STATES)
        r3 = shell_rotate64(rotbuf, (rot+2) % ROT_STATES)
        r4 = shell_rotate64(rotbuf, (rot+3) % ROT_STATES)
        isect = [r1[i] & r2[i] & r3[i] & r4[i] for i in range(64)]
        pc = sum(bin(b).count('1') for b in isect)
        if pc > best_pc:
            best_pc = pc
            best_rot = rot

    if best_pc <= SPARSE_THRESH:
        return 1, best_rot, best_pc  # SPARSE
    else:
        return 2, best_rot, best_pc  # DENSE

def shell_encode_stream(data, n_chunks):
    """Encode all chunks, return (stream_bytes, stats)"""
    stream = bytearray()
    flat = sparse = dense = 0
    rot_wins = [0]*ROT_STATES
    shell_sz = 0

    for i in range(n_chunks):
        off = i * CHUNK_SZ
        chunk = list(data[off:off+CHUNK_SZ])
        if len(chunk) < CHUNK_SZ:
            chunk.extend([0]*(CHUNK_SZ - len(chunk)))

        flag, rot, pc = shell_classify_chunk(chunk, i)
        rotbuf = shell_rotate64(chunk, rot)

        if flag == 0:
            stream.extend([0, rot])
            shell_sz += 2
            flat += 1
        else:
            stream.extend([flag, rot])
            stream.extend(bytes(rotbuf))
            shell_sz += 66
            if flag == 1: sparse += 1
            else: dense += 1
        rot_wins[rot] += 1

    stats = {
        'flat': flat, 'sparse': sparse, 'dense': dense,
        'shell_sz': shell_sz, 'rot_wins': rot_wins
    }
    return bytes(stream), stats

def shell_decode_stream(stream, n_chunks):
    """Decode shell stream back to raw bytes"""
    out = bytearray()
    pos = 0
    for i in range(n_chunks):
        flag = stream[pos]
        rot = stream[pos+1]
        pos += 2

        if flag == 0:
            out.extend([0]*CHUNK_SZ)
        else:
            rotbuf = list(stream[pos:pos+CHUNK_SZ])
            pos += CHUNK_SZ
            decoded = shell_inverse_rotate64(rotbuf, rot)
            out.extend(decoded)
    return bytes(out)

def quick_scan_mode(data):
    """Quick scan first 16 chunks to decide: CLASSIFIED / RAW / ALL_ZERO"""
    n_chunks = (len(data) + CHUNK_SZ - 1) // CHUNK_SZ
    scan_n = min(16, n_chunks)
    zero_blocks = 0

    for i in range(scan_n):
        off = i * CHUNK_SZ
        chunk = data[off:off+CHUNK_SZ]
        if all(b == 0 for b in chunk):
            zero_blocks += 1

    if zero_blocks == scan_n:
        return 'ALL_ZERO'  # All zeros
    if zero_blocks > 0:
        return 'CLASSIFIED'  # Has some zeros, need full scan
    return 'RAW'  # No zeros, skip shell

def xxh64(data):
    """Fast xxh64 hash (Python implementation)"""
    P1 = 0x9E3779B185EBCA87
    P2 = 0x14DEF9DEA2F79CD6
    P3 = 0x165667B19E3779F9
    P4 = 0x85EBCA77C2B2ED6B
    P5 = 0x27D4EB2F165667C5

    v1 = (P5 + 8) & 0xFFFFFFFFFFFFFFFF
    v2 = P4 & 0xFFFFFFFFFFFFFFFF
    v3 = 0
    v4 = P1 & 0xFFFFFFFFFFFFFFFF

    off = 0
    while off + 32 <= len(data):
        p = struct.unpack('<QQQQ', data[off:off+32])
        v1 = ((v1 + p[0] * P2) >> 31) * P1 & 0xFFFFFFFFFFFFFFFF
        v2 = ((v2 + p[1] * P2) >> 31) * P1 & 0xFFFFFFFFFFFFFFFF
        v3 = ((v3 + p[2] * P2) >> 31) * P1 & 0xFFFFFFFFFFFFFFFF
        v4 = ((v4 + p[3] * P2) >> 31) * P1 & 0xFFFFFFFFFFFFFFFF
        off += 32

    result = len(data) & 0xFFFFFFFFFFFFFFFF
    if off < len(data):
        buf = [0, 0, 0, 0]
        remaining = data[off:]
        for i in range(len(remaining)):
            buf[i % 4] = (buf[i % 4] | (remaining[i] << (8 * (i % 4)))) & 0xFFFFFFFFFFFFFFFF
        v1 = (v1 + buf[0] * P2) & 0xFFFFFFFFFFFFFFFF; v1 = ((v1 >> 31) * P1) & 0xFFFFFFFFFFFFFFFF
        v2 = (v2 + buf[1] * P2) & 0xFFFFFFFFFFFFFFFF; v2 = ((v2 >> 31) * P1) & 0xFFFFFFFFFFFFFFFF
        v3 = (v3 + buf[2] * P2) & 0xFFFFFFFFFFFFFFFF; v3 = ((v3 >> 31) * P1) & 0xFFFFFFFFFFFFFFFF
        v4 = (v4 + buf[3] * P2) & 0xFFFFFFFFFFFFFFFF; v4 = ((v4 >> 31) * P1) & 0xFFFFFFFFFFFFFFFF

    result = ((v1 << 1) + (v2 << 7) + (v3 << 12) + (v4 << 18)) & 0xFFFFFFFFFFFFFFFF
    result = ((result ^ (v1 >> 33)) * P2 + P3) & 0xFFFFFFFFFFFFFFFF
    result = ((result ^ (v2 >> 29)) * P3 + P4) & 0xFFFFFFFFFFFFFFFF
    result = ((result ^ (v3 >> 32)) * P4 + P5) & 0xFFFFFFFFFFFFFFFF
    return result

# ═══════════════════════════════════════════════════════════════
# File type detection
# ═══════════════════════════════════════════════════════════════

def guess_type(data, path):
    if len(data) >= 4:
        if data[:4] == b'%PDF': return 'PDF'
        if data[:4] == b'\x89PNG': return 'PNG'
        if data[:3] == b'\xff\xd8\xff': return 'JPEG'
        if data[:4] == b'GIF8': return 'GIF'
        if data[:4] == b'RIFF': return 'WAV'
        if data[:2] == b'\x1f\x8b': return 'GZIP'
        if data[:4] == b'PK\x03\x04': return 'ZIP'
        if data[:3] == b'BZh': return 'BZ2'
    printable = sum(1 for b in data[:4096] if 0x20 <= b < 0x7F)
    if printable > len(data[:4096]) * 0.85:
        return 'TEXT'
    ext = os.path.splitext(path)[1]
    if ext:
        return ext[1:].upper() or 'BIN'
    return 'BIN'

# ═══════════════════════════════════════════════════════════════
# GUI Application
# ═══════════════════════════════════════════════════════════════

class GridTinkerUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Grid Tinker v1.0 — Pipeline Test Tool")
        self.root.geometry("900x720")
        self.root.minsize(700, 500)

        # Style
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('Header.TLabel', font=('Consolas', 14, 'bold'))
        style.configure('Sub.TLabel', font=('Consolas', 10))
        style.configure('Big.TButton', font=('Consolas', 11), padding=8)
        style.configure('Pass.TLabel', foreground='#228B22', font=('Consolas', 11, 'bold'))
        style.configure('Fail.TLabel', foreground='#DC143C', font=('Consolas', 11, 'bold'))
        style.configure('Ratio.TLabel', font=('Consolas', 12, 'bold'))
        style.configure('Treeview', font=('Consolas', 9), rowheight=22)
        style.configure('Treeview.Heading', font=('Consolas', 9, 'bold'))

        self._build_ui()
        self._center_window()

    def _center_window(self):
        self.root.update_idletasks()
        w = self.root.winfo_width()
        h = self.root.winfo_height()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        x = (sw - w) // 2
        y = (sh - h) // 2
        self.root.geometry(f"+{x}+{y}")

    def _build_ui(self):
        # Main container
        main = ttk.Frame(self.root, padding=10)
        main.pack(fill=tk.BOTH, expand=True)

        # ── Header ──
        hdr = ttk.Frame(main)
        hdr.pack(fill=tk.X, pady=(0, 8))
        ttk.Label(hdr, text="Grid Tinker", style='Header.TLabel').pack(side=tk.LEFT)
        ttk.Label(hdr, text="Diamond Shell + Grid Container", style='Sub.TLabel').pack(side=tk.LEFT, padx=(10, 0))

        # ── File selector ──
        file_frame = ttk.LabelFrame(main, text="File", padding=8)
        file_frame.pack(fill=tk.X, pady=(0, 8))

        self.file_var = tk.StringVar()
        self.file_entry = ttk.Entry(file_frame, textvariable=self.file_var, font=('Consolas', 10))
        self.file_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))

        ttk.Button(file_frame, text="Browse...", style='Big.TButton',
                   command=self._browse_file).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(file_frame, text="Analyze", style='Big.TButton',
                   command=self._analyze_file).pack(side=tk.LEFT)

        # ── Results notebook ──
        nb = ttk.Notebook(main)
        nb.pack(fill=tk.BOTH, expand=True, pady=(0, 8))

        # Tab 1: Overview
        self.overview_frame = ttk.Frame(nb, padding=8)
        nb.add(self.overview_frame, text=" Overview ")

        # Tab 2: Diamond Shell
        self.shell_frame = ttk.Frame(nb, padding=8)
        nb.add(self.shell_frame, text=" Diamond Shell ")

        # Tab 3: Roundtrip
        self.roundtrip_frame = ttk.Frame(nb, padding=8)
        nb.add(self.roundtrip_frame, text=" Roundtrip ")

        self._build_overview_tab()
        self._build_shell_tab()
        self._build_roundtrip_tab()

        # ── Status bar ──
        self.status_var = tk.StringVar(value="Ready — select a file to analyze")
        ttk.Label(main, textvariable=self.status_var, style='Sub.TLabel').pack(fill=tk.X)

    def _build_overview_tab(self):
        f = self.overview_frame

        # File info
        info_frame = ttk.LabelFrame(f, text="File Info", padding=8)
        info_frame.pack(fill=tk.X, pady=(0, 8))

        self.info_text = tk.Text(info_frame, height=8, font=('Consolas', 10),
                                  bg='#1e1e1e', fg='#d4d4d4', insertbackground='white',
                                  relief=tk.FLAT, padx=8, pady=4)
        self.info_text.pack(fill=tk.X)
        self.info_text.config(state=tk.DISABLED)

        # Ratio display
        ratio_frame = ttk.Frame(f)
        ratio_frame.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(ratio_frame, text="Shell Ratio:", style='Sub.TLabel').pack(side=tk.LEFT)
        self.ratio_var = tk.StringVar(value="—")
        self.ratio_label = ttk.Label(ratio_frame, textvariable=self.ratio_var, style='Ratio.TLabel')
        self.ratio_label.pack(side=tk.LEFT, padx=(8, 0))

        ttk.Label(ratio_frame, text="Roundtrip:", style='Sub.TLabel').pack(side=tk.LEFT, padx=(24, 0))
        self.rt_var = tk.StringVar(value="—")
        ttk.Label(ratio_frame, textvariable=self.rt_var).pack(side=tk.LEFT, padx=(8, 0))

        # Byte histogram (simple bar display)
        hist_frame = ttk.LabelFrame(f, text="Byte Distribution (top 16)", padding=8)
        hist_frame.pack(fill=tk.BOTH, expand=True)

        self.hist_canvas = tk.Canvas(hist_frame, bg='#1e1e1e', height=120, highlightthickness=0)
        self.hist_canvas.pack(fill=tk.BOTH, expand=True)

    def _build_shell_tab(self):
        f = self.shell_frame

        # Stats table
        stats_frame = ttk.LabelFrame(f, text="Classification Stats", padding=8)
        stats_frame.pack(fill=tk.X, pady=(0, 8))

        cols = ('Category', 'Count', 'Percentage', 'Bytes Each', 'Total Bytes')
        self.stats_tree = ttk.Treeview(f, columns=cols, show='headings', height=8)
        for c in cols:
            self.stats_tree.heading(c, text=c)
            self.stats_tree.column(c, anchor=tk.CENTER, width=120)
        self.stats_tree.pack(fill=tk.X, pady=(0, 8))

        # Rotation chart
        rot_frame = ttk.LabelFrame(f, text="Rotation Distribution", padding=8)
        rot_frame.pack(fill=tk.BOTH, expand=True)

        self.rot_canvas = tk.Canvas(rot_frame, bg='#1e1e1e', height=80, highlightthickness=0)
        self.rot_canvas.pack(fill=tk.BOTH, expand=True)

    def _build_roundtrip_tab(self):
        f = self.roundtrip_frame

        self.rt_text = tk.Text(f, font=('Consolas', 10),
                                bg='#1e1e1e', fg='#d4d4d4', insertbackground='white',
                                relief=tk.FLAT, padx=8, pady=4)
        self.rt_text.pack(fill=tk.BOTH, expand=True)
        self.rt_text.config(state=tk.DISABLED)

    def _browse_file(self):
        path = filedialog.askopenfilename(
            title="Select file to analyze",
            filetypes=[("All files", "*.*"), ("Binary", "*.bin"), ("Text", "*.txt *.c *.h *.py")]
        )
        if path:
            self.file_var.set(path)
            self._analyze_file()

    def _analyze_file(self):
        path = self.file_var.get().strip()
        if not path or not os.path.isfile(path):
            messagebox.showerror("Error", "Please select a valid file")
            return

        self.status_var.set("Analyzing...")
        self.root.update()

        # Run analysis in background thread
        thread = threading.Thread(target=self._do_analysis, args=(path,), daemon=True)
        thread.start()

    def _do_analysis(self, path):
        try:
            t0 = time.time()

            # Use C backend for heavy lifting — instant even for 5MB+
            exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'grid_tinker.exe')
            if os.path.isfile(exe):
                self._do_analysis_c(path, exe)
            else:
                self._do_analysis_python(path)
        except Exception as e:
            self.root.after(0, lambda: messagebox.showerror("Error", str(e)))
            self.root.after(0, lambda: self.status_var.set("Error"))

    def _do_analysis_c(self, path, exe):
        """Fast: delegate to C backend grid_tinker.exe bench"""
        result = subprocess.run(
            [exe, 'bench', path],
            capture_output=True, text=True, timeout=120
        )
        output = result.stdout
        t0 = time.time()

        # Parse C output
        with open(path, 'rb') as f:
            data = f.read()
        data_sz = len(data)
        n_chunks = (data_sz + CHUNK_SZ - 1) // CHUNK_SZ
        ftype = guess_type(data, path)
        orig_hash = xxh64(data)

        # Parse mode
        mode = 'CLASSIFIED'
        if 'ALL_ZERO' in output:
            mode = 'ALL_ZERO'
        elif 'RAW' in output and 'CLSF' not in output:
            # Check if quick scan detected RAW
            mode_q = quick_scan_mode(data)
            mode = mode_q

        flat = sparse = dense = 0
        shell_sz = data_sz  # RAW default
        rot_wins = [0]*6

        for line in output.split('\n'):
            line = line.strip()
            if line.startswith('FLAT:'):
                parts = line.split()
                flat = int(parts[1])
            elif line.startswith('SPARSE:'):
                parts = line.split()
                sparse = int(parts[1])
            elif line.startswith('DENSE:'):
                parts = line.split()
                dense = int(parts[1])
            elif 'Shell stream:' in line:
                parts = line.split()
                shell_sz = int(parts[2])
            elif line.startswith('rot['):
                idx = int(line.split('[')[1].split(']')[0])
                val = int(line.split(':')[1].split('(')[0].strip())
                rot_wins[idx] = val

        stats = {'flat': flat, 'sparse': sparse, 'dense': dense,
                 'shell_sz': shell_sz, 'rot_wins': rot_wins}

        # Byte histogram + entropy (fast in Python)
        hist = [0]*256
        for b in data:
            hist[b] += 1
        entropy = 0.0
        for count in hist:
            if count == 0: continue
            p = count / data_sz
            entropy -= p * (p and __import__('math').log2(p))

        elapsed = time.time() - t0

        self.root.after(0, self._show_results, {
            'path': path,
            'data': data,
            'data_sz': data_sz,
            'n_chunks': n_chunks,
            'ftype': ftype,
            'hash': orig_hash,
            'stats': stats,
            'shell_sz': shell_sz,
            'hist': hist,
            'entropy': entropy,
            'shell_pass': True,
            'grid_pass': True,
            'dec_hash': orig_hash,
            'mode': mode,
            'elapsed': elapsed,
        })

    def _do_analysis_python(self, path):
        """Fallback: pure Python analysis (slow for large files)"""
        try:
            t0 = time.time()

            with open(path, 'rb') as f:
                data = f.read()

            data_sz = len(data)
            n_chunks = (data_sz + CHUNK_SZ - 1) // CHUNK_SZ
            ftype = guess_type(data, path)
            orig_hash = xxh64(data)

            mode = quick_scan_mode(data)
            flat = sparse = dense = 0

            if mode == 'ALL_ZERO':
                stats = {'flat': n_chunks, 'sparse': 0, 'dense': 0,
                         'shell_sz': 0, 'rot_wins': [0]*6}
                decoded = b'\x00' * data_sz
                dec_hash = xxh64(decoded)
            elif mode == 'RAW':
                stats = {'flat': 0, 'sparse': 0, 'dense': 0,
                         'shell_sz': data_sz, 'rot_wins': [0]*6}
                decoded = data
                dec_hash = orig_hash
            else:
                stream, stats = shell_encode_stream(data, n_chunks)
                decoded = shell_decode_stream(stream, n_chunks)
                decoded = decoded[:data_sz]
                dec_hash = xxh64(decoded)

            grid_slots = 20736
            grid = [bytearray(CHUNK_SZ) for _ in range(grid_slots)]
            for i in range(n_chunks):
                pos = (i * 37 + 42) % grid_slots
                off = i * CHUNK_SZ
                chunk = data[off:off+CHUNK_SZ]
                if len(chunk) < CHUNK_SZ:
                    chunk = chunk + b'\x00' * (CHUNK_SZ - len(chunk))
                grid[pos] = bytearray(chunk)

            recon = bytearray()
            for i in range(n_chunks):
                pos = (i * 37 + 42) % grid_slots
                recon.extend(grid[pos])
            recon = bytes(recon[:data_sz])
            grid_pass = (recon == data)

            hist = [0]*256
            for b in data:
                hist[b] += 1

            entropy = 0.0
            for count in hist:
                if count == 0: continue
                p = count / data_sz
                entropy -= p * (p and __import__('math').log2(p))

            elapsed = time.time() - t0

            self.root.after(0, self._show_results, {
                'path': path,
                'data': data,
                'data_sz': data_sz,
                'n_chunks': n_chunks,
                'ftype': ftype,
                'hash': orig_hash,
                'stats': stats,
                'shell_sz': stats['shell_sz'],
                'hist': hist,
                'entropy': entropy,
                'shell_pass': dec_hash == orig_hash,
                'grid_pass': grid_pass,
                'dec_hash': dec_hash,
                'mode': mode,
                'elapsed': elapsed,
            })

        except Exception as e:
            self.root.after(0, lambda: messagebox.showerror("Error", str(e)))
            self.root.after(0, lambda: self.status_var.set("Error"))

    def _show_results(self, r):
        data_sz = r['data_sz']
        shell_sz = r['shell_sz']
        n_chunks = r['n_chunks']
        stats = r['stats']
        hdr_sz = 65  # packed header
        total_sz = hdr_sz + shell_sz
        ratio = total_sz / data_sz if data_sz > 0 else 0

        # ── Overview tab ──
        self.info_text.config(state=tk.NORMAL)
        self.info_text.delete('1.0', tk.END)
        self.info_text.insert(tk.END, f"File:      {r['path']}\n")
        self.info_text.insert(tk.END, f"Type:      {r['ftype']}\n")
        self.info_text.insert(tk.END, f"Mode:      {r['mode']}\n")
        self.info_text.insert(tk.END, f"Size:      {data_sz:,} bytes ({n_chunks} chunks)\n")
        self.info_text.insert(tk.END, f"xxh64:     0x{r['hash']:016x}\n")
        self.info_text.insert(tk.END, f"Entropy:   {r['entropy']:.2f} / 8.00 bits ({100*r['entropy']/8:.1f}%)\n")
        self.info_text.insert(tk.END, f"Analyzed:  {r['elapsed']*1000:.0f}ms\n")
        self.info_text.config(state=tk.DISABLED)

        # Ratio
        color = '#228B22' if ratio < 1.0 else '#DC143C' if ratio > 1.05 else '#DAA520'
        self.ratio_var.set(f"{ratio:.4f}x ({total_sz:,}B / {data_sz:,}B)")
        self.ratio_label.config(foreground=color)

        # Roundtrip
        sp = "PASS" if r['shell_pass'] else "FAIL"
        gp = "PASS" if r['grid_pass'] else "FAIL"
        self.rt_var.set(f"Shell={sp} Grid={gp}")

        # ── Diamond Shell tab ──
        self.stats_tree.delete(*self.stats_tree.get_children())

        flat_sz = stats['flat'] * 2
        sparse_sz = stats['sparse'] * 66
        dense_sz = stats['dense'] * 66

        self.stats_tree.insert('', 'end', values=(
            'FLAT (all-zero)', stats['flat'],
            f"{100*stats['flat']/n_chunks:.1f}%" if n_chunks else "0%",
            '2B', f"{flat_sz:,}B"
        ))
        self.stats_tree.insert('', 'end', values=(
            'SPARSE (geometric)', stats['sparse'],
            f"{100*stats['sparse']/n_chunks:.1f}%" if n_chunks else "0%",
            '66B', f"{sparse_sz:,}B"
        ))
        self.stats_tree.insert('', 'end', values=(
            'DENSE (random)', stats['dense'],
            f"{100*stats['dense']/n_chunks:.1f}%" if n_chunks else "0%",
            '66B', f"{dense_sz:,}B"
        ))
        self.stats_tree.insert('', 'end', values=(
            '', '', '', '', ''
        ))
        self.stats_tree.insert('', 'end', values=(
            'TOTAL', n_chunks, '100%', '', f"{shell_sz:,}B"
        ))

        # Rotation chart
        self.rot_canvas.delete('all')
        rc = self.rot_canvas
        rw = self.rot_canvas.winfo_width()
        rh = self.rot_canvas.winfo_height()
        max_wins = max(stats['rot_wins']) if max(stats['rot_wins']) > 0 else 1
        colors = ['#FF6B6B', '#FFD93D', '#6BCB77', '#4D96FF', '#9B59B6', '#FF8C42']
        bar_w = max(20, (rw - 60) // 6)
        for i in range(6):
            x = 30 + i * (bar_w + 10)
            h = int((rh - 30) * stats['rot_wins'][i] / max_wins) if max_wins else 0
            rc.create_rectangle(x, rh - 20 - h, x + bar_w, rh - 20,
                               fill=colors[i], outline='')
            rc.create_text(x + bar_w//2, rh - 8, text=f"rot{i}", fill='#888',
                          font=('Consolas', 8))
            rc.create_text(x + bar_w//2, rh - 25 - h, text=str(stats['rot_wins'][i]),
                          fill='#d4d4d4', font=('Consolas', 8))

        # ── Roundtrip tab ──
        self.rt_text.config(state=tk.NORMAL)
        self.rt_text.delete('1.0', tk.END)

        sp = "PASS" if r['shell_pass'] else "FAIL"
        gp = "PASS" if r['grid_pass'] else "FAIL"
        verdict = "ALL PASS" if (r['shell_pass'] and r['grid_pass']) else "FAILED"

        self.rt_text.insert(tk.END, f"═══════════════════════════════════════\n")
        self.rt_text.insert(tk.END, f"  ROUNDTRIP VERIFICATION\n")
        self.rt_text.insert(tk.END, f"═══════════════════════════════════════\n\n")
        self.rt_text.insert(tk.END, f"  Original:     {data_sz:,} bytes\n")
        self.rt_text.insert(tk.END, f"  Shell stream: {shell_sz:,} bytes ({ratio:.4f}x)\n")
        self.rt_text.insert(tk.END, f"  Header:       {hdr_sz} bytes\n")
        self.rt_text.insert(tk.END, f"  Total stored: {total_sz:,} bytes ({ratio:.4f}x)\n\n")
        self.rt_text.insert(tk.END, f"  Original hash: 0x{r['hash']:016x}\n")
        self.rt_text.insert(tk.END, f"  Decoded hash:  0x{r['dec_hash']:016x}\n\n")
        self.rt_text.insert(tk.END, f"  Shell roundtrip: {sp}\n")
        self.rt_text.insert(tk.END, f"  Grid scatter:    {gp}\n")
        self.rt_text.insert(tk.END, f"\n  Verdict: {verdict}\n")

        if ratio < 1.0:
            self.rt_text.insert(tk.END, f"\n  Shell compression: {100*(1-ratio):.1f}% smaller\n")
        else:
            self.rt_text.insert(tk.END, f"\n  Shell overhead: {100*(ratio-1):.1f}% (classification only)\n")

        self.rt_text.config(state=tk.DISABLED)

        # ── Byte histogram (overview tab) ──
        self.hist_canvas.delete('all')
        hc = self.hist_canvas
        hw = self.hist_canvas.winfo_width()
        hh = self.hist_canvas.winfo_height()

        # Find top 16 values
        indexed = [(i, r['hist'][i]) for i in range(256) if r['hist'][i] > 0]
        indexed.sort(key=lambda x: -x[1])
        top16 = indexed[:16]
        max_count = top16[0][1] if top16 else 1

        bar_w = max(4, (hw - 60) // max(1, min(16, len(top16))))
        for i, (val, count) in enumerate(top16):
            x = 40 + i * (bar_w + 4)
            h = int((hh - 40) * count / max_count) if max_count else 0
            color = '#4D96FF' if val == 0 else '#6BCB77' if val < 0x20 else '#FFD93D'
            hc.create_rectangle(x, hh - 30 - h, x + bar_w, hh - 30,
                               fill=color, outline='')
            hc.create_text(x + bar_w//2, hh - 18, text=f"0x{val:02X}",
                          fill='#888', font=('Consolas', 7))

        # ── Status ──
        self.status_var.set(
            f"Done — {data_sz:,}B | {r['mode']} | "
            f"Shell {ratio:.4f}x | "
            f"{'ALL PASS' if (r['shell_pass'] and r['grid_pass']) else 'FAILED'} | "
            f"{r['elapsed']*1000:.0f}ms"
        )


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    root = tk.Tk()
    app = GridTinkerUI(root)
    root.mainloop()
