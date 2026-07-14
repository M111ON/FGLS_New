#!/usr/bin/env python3
"""
GeoPixel One-Stop Service
═══════════════════════════

Simple tkinter UI for encoding/decoding files through the full pipeline:
  Input → GeoField structuring → Geopixel encoding → Output

Usage:
    python geopixel_service.py
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import os, sys, time, hashlib, threading

# ── Pipeline imports ──
sys.path.insert(0, os.path.dirname(__file__))

from hamburger_codec_poc_v5 import (
    create_timeline_cube, unfold_cube, 
    reconstruct_from_timeline_vectorized, FRAME_CYCLE, FRAME_STRIDE
)

class GeoPixelService:
    """One-stop service for GeoPixel encoding/decoding."""
    
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("GeoPixel Service")
        self.root.geometry("600x500")
        self.root.configure(bg='#1a1a2e')
        
        self.input_path = tk.StringVar()
        self.output_path = tk.StringVar()
        self.status = tk.StringVar(value="Ready")
        
        self.setup_ui()
    
    def setup_ui(self):
        """Setup the UI."""
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('TFrame', background='#1a1a2e')
        style.configure('TLabel', background='#1a1a2e', foreground='white')
        style.configure('TButton', background='#16213e', foreground='white')
        style.configure('TEntry', background='#0f3460', foreground='white')
        
        # Main frame
        main_frame = ttk.Frame(self.root, padding="20")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Title
        title = ttk.Label(main_frame, text="GeoPixel Service", 
                         font=('Helvetica', 16, 'bold'))
        title.pack(pady=(0, 20))
        
        # Input section
        input_frame = ttk.Frame(main_frame)
        input_frame.pack(fill=tk.X, pady=5)
        
        ttk.Label(input_frame, text="Input:").pack(side=tk.LEFT)
        ttk.Entry(input_frame, textvariable=self.input_path, width=50).pack(side=tk.LEFT, padx=5)
        ttk.Button(input_frame, text="Browse", command=self.browse_input).pack(side=tk.LEFT)
        
        # Output section
        output_frame = ttk.Frame(main_frame)
        output_frame.pack(fill=tk.X, pady=5)
        
        ttk.Label(output_frame, text="Output:").pack(side=tk.LEFT)
        ttk.Entry(output_frame, textvariable=self.output_path, width=50).pack(side=tk.LEFT, padx=5)
        ttk.Button(output_frame, text="Browse", command=self.browse_output).pack(side=tk.LEFT)
        
        # Options
        options_frame = ttk.Frame(main_frame)
        options_frame.pack(fill=tk.X, pady=10)
        
        self.encode_var = tk.BooleanVar(value=True)
        ttk.Radiobutton(options_frame, text="Encode", variable=self.encode_var, 
                       value=True).pack(side=tk.LEFT, padx=10)
        ttk.Radiobutton(options_frame, text="Decode", variable=self.encode_var, 
                       value=False).pack(side=tk.LEFT, padx=10)
        
        # Process button
        self.process_btn = ttk.Button(main_frame, text="Process", command=self.process)
        self.process_btn.pack(pady=20)
        
        # Status
        ttk.Label(main_frame, textvariable=self.status, 
                 font=('Helvetica', 10)).pack(pady=10)
        
        # Log area
        self.log = tk.Text(main_frame, height=10, bg='#0f3460', fg='white',
                          font=('Consolas', 9))
        self.log.pack(fill=tk.BOTH, expand=True, pady=10)
    
    def browse_input(self):
        """Browse for input file."""
        path = filedialog.askopenfilename(
            title="Select Input File",
            filetypes=[("All files", "*.*")]
        )
        if path:
            self.input_path.set(path)
            # Auto-set output path
            base = os.path.splitext(path)[0]
            self.output_path.set(base + ".geopixel")
    
    def browse_output(self):
        """Browse for output file."""
        path = filedialog.asksaveasfilename(
            title="Select Output File",
            defaultextension=".geopixel",
            filetypes=[("GeoPixel files", "*.geopixel"), ("All files", "*.*")]
        )
        if path:
            self.output_path.set(path)
    
    def log_message(self, msg):
        """Add message to log."""
        self.log.insert(tk.END, msg + "\n")
        self.log.see(tk.END)
        self.root.update_idletasks()
    
    def process(self):
        """Process the file."""
        input_path = self.input_path.get()
        output_path = self.output_path.get()
        
        if not input_path or not os.path.exists(input_path):
            messagebox.showerror("Error", "Please select a valid input file")
            return
        
        if not output_path:
            messagebox.showerror("Error", "Please select an output file")
            return
        
        # Run in thread to keep UI responsive
        threading.Thread(target=self._process_thread, 
                        args=(input_path, output_path), 
                        daemon=True).start()
    
    def _process_thread(self, input_path, output_path):
        """Process in background thread."""
        try:
            self.status.set("Processing...")
            self.process_btn.config(state='disabled')
            
            # Read input
            self.log_message(f"Reading: {input_path}")
            t0 = time.perf_counter()
            data = open(input_path, 'rb').read()
            t_read = time.perf_counter() - t0
            self.log_message(f"  Size: {len(data):,} bytes ({len(data)/1024:.1f} KB)")
            self.log_message(f"  Time: {t_read*1000:.1f} ms")
            
            if self.encode_var.get():
                # Encode
                self.log_message("\n--- ENCODE ---")
                self._encode(data, output_path)
            else:
                # Decode
                self.log_message("\n--- DECODE ---")
                self._decode(data, output_path)
            
            self.status.set("Done!")
            self.log_message("\n✓ Complete!")
            
        except Exception as e:
            self.status.set(f"Error: {e}")
            self.log_message(f"\n✗ Error: {e}")
        finally:
            self.process_btn.config(state='normal')
    
    def _encode(self, data, output_path):
        """Encode file to GeoPixel format."""
        import numpy as np
        
        # Convert to cube
        self.log_message("Converting to cube...")
        t0 = time.perf_counter()
        
        n_bytes = len(data)
        side = 100  # Fixed size for simplicity
        total_voxels = side ** 3
        
        # Pad data
        padded = data + b'\x00' * (total_voxels - n_bytes)
        arr = np.frombuffer(padded, dtype=np.uint8)
        cube = arr.reshape(side, side, side).astype(np.float32)
        
        t_convert = time.perf_counter() - t0
        self.log_message(f"  Cube: {side}×{side}×{side}")
        self.log_message(f"  Time: {t_convert*1000:.1f} ms")
        
        # Unfold to 6 faces
        self.log_message("Unfolding to 6 faces...")
        t0 = time.perf_counter()
        faces = unfold_cube(cube)
        t_unfold = time.perf_counter() - t0
        
        face_bytes = sum(f.nbytes for f in faces.values())
        self.log_message(f"  Faces: 6 × {side}×{side} = {face_bytes/1024:.1f} KB")
        self.log_message(f"  Time: {t_unfold*1000:.1f} ms")
        
        # Save
        self.log_message("Saving...")
        t0 = time.perf_counter()
        
        # Simple format: header + 6 faces
        with open(output_path, 'wb') as f:
            # Header
            f.write(b'GPIX')  # Magic
            f.write(np.uint32(side).tobytes())  # Side length
            f.write(np.uint32(n_bytes).tobytes())  # Original size
            
            # Write 6 faces
            for face_name in ['front', 'back', 'left', 'right', 'top', 'bottom']:
                f.write(faces[face_name].astype(np.float32).tobytes())
        
        t_save = time.perf_counter() - t0
        self.log_message(f"  Saved: {output_path}")
        self.log_message(f"  Time: {t_save*1000:.1f} ms")
        
        # Summary
        total_time = t_convert + t_unfold + t_save
        self.log_message(f"\n--- Summary ---")
        self.log_message(f"Original: {n_bytes:,} bytes")
        self.log_message(f"Stored:   {face_bytes:,} bytes (6 faces)")
        self.log_message(f"Ratio:    {cube.nbytes/face_bytes:.1f}x")
        self.log_message(f"Total:    {total_time*1000:.1f} ms")
    
    def _decode(self, data, output_path):
        """Decode GeoPixel format to file."""
        import numpy as np
        
        # Read header
        self.log_message("Reading header...")
        magic = data[:4]
        if magic != b'GPIX':
            self.log_message("✗ Invalid format (bad magic)")
            return
        
        side = int(np.frombuffer(data[4:8], dtype=np.uint32)[0])
        orig_size = int(np.frombuffer(data[8:12], dtype=np.uint32)[0])
        
        self.log_message(f"  Side: {side}")
        self.log_message(f"  Original size: {orig_size:,} bytes")
        
        # Read 6 faces
        self.log_message("Reading 6 faces...")
        t0 = time.perf_counter()
        
        faces = {}
        offset = 12
        for face_name in ['front', 'back', 'left', 'right', 'top', 'bottom']:
            face_data = data[offset:offset + side*side*4]
            faces[face_name] = np.frombuffer(face_data, dtype=np.float32).reshape(side, side)
            offset += side * side * 4
        
        t_read = time.perf_counter() - t0
        self.log_message(f"  Time: {t_read*1000:.1f} ms")
        
        # Reconstruct
        self.log_message("Reconstructing...")
        t0 = time.perf_counter()
        reconstructed = reconstruct_from_timeline_vectorized(faces, side)
        t_recon = time.perf_counter() - t0
        self.log_message(f"  Time: {t_recon*1000:.1f} ms")
        
        # Convert back to bytes
        self.log_message("Converting to file...")
        arr = reconstructed.astype(np.uint8).flatten()
        output_data = arr[:orig_size].tobytes()
        
        # Save
        with open(output_path, 'wb') as f:
            f.write(output_data)
        
        self.log_message(f"  Saved: {output_path}")
        self.log_message(f"  Size: {len(output_data):,} bytes")
        
        # Summary
        total_time = t_read + t_recon
        self.log_message(f"\n--- Summary ---")
        self.log_message(f"Input:    {len(data):,} bytes")
        self.log_message(f"Output:   {len(output_data):,} bytes")
        self.log_message(f"Total:    {total_time*1000:.1f} ms")
    
    def run(self):
        """Run the app."""
        self.root.mainloop()

if __name__ == '__main__':
    app = GeoPixelService()
    app.run()
