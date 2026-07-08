#!/usr/bin/env python3
"""LFM2.5-VL-450M Image Analyzer GUI"""

import tkinter as tk
from tkinter import filedialog, scrolledtext, messagebox
from PIL import Image, ImageTk
import threading
import os
import torch
from transformers import AutoProcessor, AutoModelForImageTextToText

MODEL_PATH = "I:/model/LFM2.5-VL-450M"
MAX_PREVIEW = (480, 480)
SPINNER_FRAMES = 8


class VLImageAnalyzer:
    def __init__(self, root):
        self.root = root
        self.root.title("LFM2.5-VL-450M Image Analyzer")
        self.root.geometry("900x650")
        self.root.minsize(700, 500)

        self.image = None
        self.photo = None
        self.model = None
        self.processor = None
        self.model_loaded = False
        self.loading_lock = threading.Lock()

        self._build_ui()
        self.root.after(100, self._load_model_async)

    def _build_ui(self):
        # ── Top toolbar ──
        toolbar = tk.Frame(self.root, bd=1, relief=tk.RAISED)
        toolbar.pack(side=tk.TOP, fill=tk.X, padx=2, pady=2)

        self.btn_load = tk.Button(toolbar, text="+ Load Image", command=self.load_image,
                                  bg="#e0e0e0", padx=12)
        self.btn_load.pack(side=tk.LEFT, padx=4, pady=4)

        self.btn_analyze = tk.Button(toolbar, text="Analyze", command=self.analyze,
                                     bg="#4a90d9", fg="white", padx=16, state=tk.DISABLED)
        self.btn_analyze.pack(side=tk.LEFT, padx=4, pady=4)

        self.btn_clear = tk.Button(toolbar, text="Clear", command=self.clear_all,
                                   padx=8)
        self.btn_clear.pack(side=tk.LEFT, padx=4, pady=4)

        tk.Label(toolbar, text="Max tokens:").pack(side=tk.LEFT, padx=(20, 2))
        self.var_max_tok = tk.StringVar(value="100")
        tk.Spinbox(toolbar, from_=10, to=500, textvariable=self.var_max_tok,
                   width=5).pack(side=tk.LEFT, padx=2)

        # ── Main split pane ──
        main = tk.PanedWindow(self.root, orient=tk.HORIZONTAL, sashwidth=4)
        main.pack(fill=tk.BOTH, expand=1, padx=4, pady=2)

        # Left: image preview
        left = tk.Frame(main, bg="#f0f0f0")
        main.add(left, width=440, minsize=300)

        self.lbl_placeholder = tk.Label(left, text="Load an image to begin",
                                        bg="#f0f0f0", fg="#999",
                                        font=("Segoe UI", 14))
        self.lbl_placeholder.place(relx=0.5, rely=0.4, anchor=tk.CENTER)

        self.lbl_image = tk.Label(left, bg="#f0f0f0")
        self.lbl_image.pack(fill=tk.BOTH, expand=1, padx=8, pady=8)

        # File info label
        self.lbl_info = tk.Label(left, text="", bg="#f0f0f0", fg="#666",
                                 font=("Segoe UI", 9), anchor=tk.W)
        self.lbl_info.pack(fill=tk.X, padx=8, pady=(0, 4))

        # Right: output
        right = tk.Frame(main)
        main.add(right, width=440, minsize=250)

        tk.Label(right, text="Model Response", font=("Segoe UI", 10, "bold"),
                 anchor=tk.W).pack(fill=tk.X, padx=4, pady=(4, 0))

        self.txt_output = scrolledtext.ScrolledText(right, wrap=tk.WORD,
                                                     font=("Consolas", 10),
                                                     bg="#fafafa", state=tk.DISABLED)
        self.txt_output.pack(fill=tk.BOTH, expand=1, padx=4, pady=4)

        # ── Bottom: prompt + status ──
        bottom = tk.Frame(self.root)
        bottom.pack(side=tk.BOTTOM, fill=tk.X, padx=6, pady=4)

        prompt_frame = tk.Frame(bottom)
        prompt_frame.pack(fill=tk.X, pady=(0, 2))

        tk.Label(prompt_frame, text="Prompt:").pack(side=tk.LEFT)
        self.var_prompt = tk.StringVar(value="What is in this image?")
        tk.Entry(prompt_frame, textvariable=self.var_prompt).pack(side=tk.LEFT,
                                                                  fill=tk.X,
                                                                  expand=1,
                                                                  padx=4)

        status_frame = tk.Frame(bottom, bd=1, relief=tk.SUNKEN)
        status_frame.pack(fill=tk.X)

        self.lbl_status = tk.Label(status_frame, text="Loading model...",
                                   font=("Segoe UI", 9), anchor=tk.W)
        self.lbl_status.pack(side=tk.LEFT, padx=4)

        self.lbl_indicator = tk.Label(status_frame, text="\u25cf", fg="#f0ad4e",
                                      font=("Segoe UI", 9))
        self.lbl_indicator.pack(side=tk.RIGHT, padx=4)

        # Hide preview label, show image label by default
        self.lbl_placeholder.lift()

    def _load_model_async(self):
        self._status("Loading model...", "#f0ad4e")
        threading.Thread(target=self._load_model, daemon=True).start()

    def _load_model(self):
        if self.model_loaded:
            return
        try:
            proc = AutoProcessor.from_pretrained(MODEL_PATH, trust_remote_code=True)
            mdl = AutoModelForImageTextToText.from_pretrained(
                MODEL_PATH, trust_remote_code=True, dtype=torch.float32,
                low_cpu_mem_usage=True
            )
            mdl.eval()
            with self.loading_lock:
                self.processor = proc
                self.model = mdl
                self.model_loaded = True
            self.root.after(0, lambda: self._status(f"Ready \u2014 {MODEL_PATH}", "#5cb85c"))
            self.root.after(0, self._enable_analyze)
        except Exception as e:
            self.root.after(0, lambda: self._status(f"Load failed: {e}", "#d9534f"))

    def _status(self, msg, color="#333"):
        self.lbl_status.config(text=msg, fg=color)
        self.lbl_indicator.config(fg=color)

    def _enable_analyze(self):
        if self.image is not None and self.model_loaded:
            self.btn_analyze.config(state=tk.NORMAL)
        self.btn_load.config(state=tk.NORMAL)

    def load_image(self):
        path = filedialog.askopenfilename(
            title="Select Image",
            filetypes=[("Images", "*.jpg *.jpeg *.png *.bmp *.webp *.gif"), ("All", "*.*")]
        )
        if not path:
            return
        self._set_image(path)

    def _set_image(self, path):
        try:
            img = Image.open(path).convert("RGB")
            self.image = img
            self.img_path = path

            thumb = img.copy()
            thumb.thumbnail(MAX_PREVIEW, Image.LANCZOS)
            self.photo = ImageTk.PhotoImage(thumb)

            self.lbl_image.config(image=self.photo)
            self.lbl_placeholder.lower()
            self.lbl_image.lift()

            size_kb = os.path.getsize(path) // 1024
            self.lbl_info.config(text=f"{os.path.basename(path)} \u2014 "
                                f"{img.size[0]}x{img.size[1]} \u2014 {size_kb}KB")

            if self.model_loaded:
                self.btn_analyze.config(state=tk.NORMAL)
            self._status(f"Loaded: {os.path.basename(path)}", "#5cb85c")
        except Exception as e:
            messagebox.showerror("Error", f"Cannot open image:\n{e}")

    def analyze(self):
        if not self.model_loaded or self.image is None:
            return
        self.btn_analyze.config(state=tk.DISABLED, text="Analyzing...")
        self._clear_output()
        self._status("Analyzing...", "#4a90d9")
        try:
            max_tok = int(self.var_max_tok.get())
        except ValueError:
            max_tok = 100
        threading.Thread(target=self._run_inference, args=(max_tok,),
                         daemon=True).start()

    def _run_inference(self, max_tokens):
        try:
            prompt = self.var_prompt.get().strip()
            messages = [{"role": "user", "content": [
                {"type": "image", "image": self.image},
                {"type": "text", "text": prompt}
            ]}]
            inputs = self.processor.apply_chat_template(
                messages, add_generation_prompt=True, tokenize=True,
                return_dict=True, return_tensors="pt"
            )
            with torch.no_grad():
                outputs = self.model.generate(**inputs, max_new_tokens=max_tokens,
                                              do_sample=False)
            answer = self.processor.decode(
                outputs[0][inputs["input_ids"].shape[-1]:], skip_special_tokens=True
            )
            self.root.after(0, lambda: self._append_output(answer.strip()))
            self.root.after(0, lambda: self._status("Done", "#5cb85c"))
        except Exception as e:
            self.root.after(0, lambda: self._append_output(f"[Error] {e}"))
            self.root.after(0, lambda: self._status(f"Error: {e}", "#d9534f"))
        finally:
            self.root.after(0, lambda: self.btn_analyze.config(state=tk.NORMAL,
                                                               text="Analyze"))
            self.root.after(0, self._enable_analyze)

    def _append_output(self, text):
        self.txt_output.config(state=tk.NORMAL)
        if self.txt_output.get("1.0", tk.END).strip():
            self.txt_output.insert(tk.END, "\n\n---\n\n")
        self.txt_output.insert(tk.END, text)
        self.txt_output.see(tk.END)
        self.txt_output.config(state=tk.DISABLED)

    def _clear_output(self):
        self.txt_output.config(state=tk.NORMAL)
        self.txt_output.delete("1.0", tk.END)
        self.txt_output.config(state=tk.DISABLED)

    def clear_all(self):
        self.image = None
        self.photo = None
        self.lbl_image.config(image="")
        self.lbl_placeholder.lift()
        self.lbl_info.config(text="")
        self._clear_output()
        self.btn_analyze.config(state=tk.DISABLED)
        self._status("Cleared", "#666")


def main():
    root = tk.Tk()
    app = VLImageAnalyzer(root)
    root.mainloop()


if __name__ == "__main__":
    main()
