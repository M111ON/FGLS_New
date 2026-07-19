"""
FGLS GUI — Graphical Interface for FGLS Universal Codec
Double-click to run. Requires Python 3 + tkinter.

Features:
  - Drag & drop files (or browse)
  - Compress / Decompress / Info / Benchmark
  - Progress bar
  - Results display
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import subprocess
import os
import threading

# Paths
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FGLS_EXE = os.path.join(SCRIPT_DIR, "fgls.exe")
PRO_EXE = os.path.join(SCRIPT_DIR, "pro_cli.exe")

class FglsGUI:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("FGLS Universal Codec v2.0.0")
        self.root.geometry("600x500")
        self.root.resizable(False, False)
        
        self.build_ui()
    
    def build_ui(self):
        # Title
        title = tk.Label(self.root, text="FGLS Universal Codec", font=("Arial", 16, "bold"))
        title.pack(pady=10)
        
        # File selection
        file_frame = tk.LabelFrame(self.root, text="File", padx=10, pady=10)
        file_frame.pack(fill="x", padx=10, pady=5)
        
        self.file_var = tk.StringVar()
        self.file_entry = tk.Entry(file_frame, textvariable=self.file_var, width=40)
        self.file_entry.pack(side="left", padx=(0, 5))
        
        browse_btn = tk.Button(file_frame, text="Browse...", command=self.browse_file)
        browse_btn.pack(side="right")
        
        # Action buttons
        btn_frame = tk.Frame(self.root)
        btn_frame.pack(pady=10)
        
        self.compress_btn = tk.Button(btn_frame, text="Compress", command=self.compress, 
                                       bg="#4CAF50", fg="white", font=("Arial", 10, "bold"),
                                       padx=20, pady=5)
        self.compress_btn.pack(side="left", padx=5)
        
        self.decompress_btn = tk.Button(btn_frame, text="Decompress", command=self.decompress,
                                         bg="#2196F3", fg="white", font=("Arial", 10, "bold"),
                                         padx=20, pady=5)
        self.decompress_btn.pack(side="left", padx=5)
        
        self.info_btn = tk.Button(btn_frame, text="Info", command=self.info,
                                   bg="#FF9800", fg="white", font=("Arial", 10, "bold"),
                                   padx=20, pady=5)
        self.info_btn.pack(side="left", padx=5)
        
        self.bench_btn = tk.Button(btn_frame, text="Benchmark", command=self.benchmark,
                                    bg="#9C27B0", fg="white", font=("Arial", 10, "bold"),
                                    padx=20, pady=5)
        self.bench_btn.pack(side="left", padx=5)
        
        # Progress
        self.progress = ttk.Progressbar(self.root, mode='indeterminate')
        self.progress.pack(fill="x", padx=10, pady=5)
        
        # Results
        result_frame = tk.LabelFrame(self.root, text="Results", padx=10, pady=10)
        result_frame.pack(fill="both", expand=True, padx=10, pady=5)
        
        self.result_text = tk.Text(result_frame, height=15, width=70, font=("Consolas", 9))
        scrollbar = ttk.Scrollbar(result_frame, orient="vertical", command=self.result_text.yview)
        self.result_text.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.result_text.pack(fill="both", expand=True)
    
    def browse_file(self):
        filename = filedialog.askopenfilename(
            title="Select file",
            filetypes=[
                ("All files", "*.*"),
                ("FGLS files", "*.fgls"),
                ("Binary files", "*.bin"),
                ("Tensor files", "*.gguf *.ggml *.bin")
            ]
        )
        if filename:
            self.file_var.set(filename)
    
    def log(self, msg):
        self.result_text.insert("end", msg + "\n")
        self.result_text.see("end")
        self.root.update_idletasks()
    
    def clear_log(self):
        self.result_text.delete("1.0", "end")
    
    def set_buttons(self, state):
        for btn in [self.compress_btn, self.decompress_btn, self.info_btn, self.bench_btn]:
            btn.configure(state=state)
    
    def run_cmd(self, cmd, callback=None):
        def thread_func():
            self.set_buttons("disabled")
            self.progress.start()
            self.clear_log()
            self.log(f"Running: {' '.join(cmd)}\n")
            
            try:
                result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
                self.log(result.stdout)
                if result.stderr:
                    self.log("ERRORS:\n" + result.stderr)
                if callback:
                    callback(result.returncode == 0)
            except subprocess.TimeoutExpired:
                self.log("ERROR: Command timed out (5 min limit)")
            except FileNotFoundError:
                self.log(f"ERROR: Executable not found: {cmd[0]}")
            except Exception as e:
                self.log(f"ERROR: {e}")
            
            self.progress.stop()
            self.set_buttons("normal")
        
        threading.Thread(target=thread_func, daemon=True).start()
    
    def compress(self):
        path = self.file_var.get()
        if not path:
            messagebox.showwarning("No file", "Please select a file first")
            return
        
        out = path + ".fgls"
        cmd = [FGLS_EXE, "encode", path, out]
        
        def on_done(success):
            if success:
                orig = os.path.getsize(path)
                comp = os.path.getsize(out)
                ratio = comp / orig * 100
                self.log(f"\n✓ Compressed: {orig:,} → {comp:,} bytes ({ratio:.1f}%)")
        
        self.run_cmd(cmd, on_done)
    
    def decompress(self):
        path = self.file_var.get()
        if not path:
            messagebox.showwarning("No file", "Please select a file first")
            return
        
        if path.endswith(".fgls"):
            out = path[:-5]
        else:
            out = path + ".dec"
        
        cmd = [FGLS_EXE, "decode", path, out]
        
        def on_done(success):
            if success:
                self.log(f"\n✓ Decompressed to: {out}")
        
        self.run_cmd(cmd, on_done)
    
    def info(self):
        path = self.file_var.get()
        if not path:
            messagebox.showwarning("No file", "Please select a file first")
            return
        
        cmd = [FGLS_EXE, "info", path]
        self.run_cmd(cmd)
    
    def benchmark(self):
        path = self.file_var.get()
        if not path:
            messagebox.showwarning("No file", "Please select a file first")
            return
        
        cmd = [FGLS_EXE, "bench", path]
        self.run_cmd(cmd)
    
    def run(self):
        self.root.mainloop()

if __name__ == "__main__":
    app = FglsGUI()
    app.run()
