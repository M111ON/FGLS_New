#!/usr/bin/env python3
"""LLM Launcher GUI — Launch runner or start llama-server with mDNS."""

import tkinter as tk
from tkinter import filedialog, scrolledtext, ttk
import subprocess, threading, os, glob, json, socket, time

CONFIG_PATH = os.path.join(os.path.dirname(__file__), "llm_launcher_config.json")
RUNNER_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "runner")
RUNNER_PATH = os.path.join(RUNNER_DIR, "llama_pogls_runner_sid_v2.exe")
LLAMA_SERVER_DIR = "I:/llama/llama-b9733-bin-win-vulkan-x64"
LLAMA_SERVER_PATH = os.path.join(LLAMA_SERVER_DIR, "llama-server.exe")
MODEL_DIR = "I:/model"
DEF_PORT = 8080
CHAT_HTML = os.path.join(os.path.dirname(__file__), "chat.html")


class LLMLauncher:
    def __init__(self, root):
        self.root = root
        self.root.title("LLM Launcher")
        self.root.geometry("700x680")
        self.root.minsize(580, 500)

        self.server_proc = None
        self.mdns_service = None
        self._scan_models()
        self._build_ui()
        self._load_config()

    def _scan_models(self):
        self.models = []
        for f in glob.glob(os.path.join(MODEL_DIR, "**", "*.gguf"), recursive=True):
            name = os.path.basename(f)
            if name.startswith(("mmproj-", "tokenizer-", "vocoder-", "Kokoro_")):
                continue
            self.models.append((f"{name}  [{os.path.getsize(f)//1024//1024}MB]", f))
        for d in glob.glob(os.path.join(MODEL_DIR, "**", "config.json"), recursive=True):
            parent = os.path.dirname(d)
            name = os.path.basename(parent)
            if not any(p == parent for _, p in self.models):
                self.models.append((f"{name}  (HF)", parent))
        self.models.sort(key=lambda x: x[0])

    def _default_model(self):
        if not self.models:
            return ""
        for label, path in self.models:
            name = os.path.basename(path)
            if "lfm" in name.lower() or "qwen" in name.lower() or "smol" in name.lower():
                return label
        return self.models[0][0]

    def _build_ui(self):
        top = tk.Frame(self.root)
        top.pack(fill=tk.X, padx=6, pady=(6, 2))

        tk.Label(top, text="Model:", width=5, anchor=tk.W).pack(side=tk.LEFT)
        model_vals = [m[0] for m in self.models]
        self.cb_model = ttk.Combobox(top, values=model_vals, state="readonly", width=45)
        if model_vals:
            self.cb_model.set(self._default_model())
        self.cb_model.pack(side=tk.LEFT, padx=2)
        self.btn_browse = tk.Button(top, text="Browse...", command=self._browse_model)
        self.btn_browse.pack(side=tk.LEFT, padx=2)
        tk.Button(top, text="\u21bb", command=self._refresh_models, width=3).pack(side=tk.LEFT)

        self.note = ttk.Notebook(self.root)
        self.note.pack(fill=tk.BOTH, expand=1, padx=6, pady=4)

        # Tab 1: Server
        srv = tk.Frame(self.note, padx=8, pady=6)
        self.note.add(srv, text="Server")

        self.var_host = tk.StringVar(value="0.0.0.0")
        tk.Label(srv, text="Host:", width=14, anchor=tk.W).grid(row=0, column=0, sticky=tk.W, pady=2)
        tk.Entry(srv, textvariable=self.var_host, width=20).grid(row=0, column=1, sticky=tk.W, pady=2)

        self.var_port = tk.StringVar(value=str(DEF_PORT))
        tk.Label(srv, text="Port:", width=14, anchor=tk.W).grid(row=1, column=0, sticky=tk.W, pady=2)
        tk.Spinbox(srv, from_=1024, to=65535, textvariable=self.var_port, width=8).grid(row=1, column=1, sticky=tk.W, pady=2)

        self.var_mdns = tk.BooleanVar(value=True)
        tk.Checkbutton(srv, text="mDNS (aventador.local:PORT)", variable=self.var_mdns).grid(row=2, column=0, columnspan=2, sticky=tk.W, pady=2)

        self.var_ngl = tk.StringVar(value="0")
        tk.Label(srv, text="GPU layers:", width=14, anchor=tk.W).grid(row=3, column=0, sticky=tk.W, pady=2)
        tk.Spinbox(srv, from_=0, to=99, textvariable=self.var_ngl, width=6).grid(row=3, column=1, sticky=tk.W, pady=2)

        self.var_ctx = tk.StringVar(value="2048")
        tk.Label(srv, text="Context size:", width=14, anchor=tk.W).grid(row=4, column=0, sticky=tk.W, pady=2)
        tk.Spinbox(srv, from_=512, to=65536, textvariable=self.var_ctx, width=8).grid(row=4, column=1, sticky=tk.W, pady=2)

        self.var_mmproj = tk.StringVar(value="")
        tk.Label(srv, text="MMProj (vision):", width=14, anchor=tk.W).grid(row=5, column=0, sticky=tk.W, pady=2)
        tk.Entry(srv, textvariable=self.var_mmproj, width=30).grid(row=5, column=1, sticky=tk.W, pady=2)

        # Tab 2: Runner Flags
        run = tk.Frame(self.note, padx=8, pady=6)
        self.note.add(run, text="Runner Flags")
        self.flag_vars = {}
        runner_flags = [
            ("--sid-face",       "SID face swap (0=off)",     False, 0, 1, 0),
            ("--dramtile",       "DRamTile store",             True),
            ("--ngl",            "GPU layers",                False, 0, 99, 0),
            ("--max-new",        "Max tokens",                False, 1, 4096, 256),
            ("--temp",           "Temperature (0..2)",       False, 0, 200, 70, "float"),
            ("--remap",          "KV remap",                   True),
            ("--shadow",         "Shadow zone",                True),
            ("--sid-force",      "Force SID on GPU",          True),
            ("--sid-verbose",    "SID debug",                 True),
            ("--kv-page",        "KV page store",              True),
        ]
        for i, f in enumerate(runner_flags):
            name, label, is_check = f[0], f[1], f[2]
            if is_check:
                var = tk.BooleanVar(value=False)
                tk.Checkbutton(run, text=f"{name}  ({label})", variable=var, anchor=tk.W)\
                    .grid(row=i, column=0, sticky=tk.W, pady=1)
                self.flag_vars[name] = var
            else:
                min_v, max_v, default = f[3], f[4], f[5]
                is_float = len(f) > 6 and f[6] == "float"
                var = tk.StringVar(value=str(default))
                tk.Label(run, text=f"{name} ({label}):", width=20, anchor=tk.W)\
                    .grid(row=i, column=0, sticky=tk.W, pady=1)
                tk.Spinbox(run, from_=min_v, to=max_v, textvariable=var, width=8)\
                    .grid(row=i, column=1, sticky=tk.W, pady=1)
                self.flag_vars[name] = var

        extra_frame = tk.Frame(self.root)
        extra_frame.pack(fill=tk.X, padx=6, pady=(0, 2))
        tk.Label(extra_frame, text="Extra args:").pack(side=tk.LEFT)
        self.var_extra = tk.StringVar(value="")
        tk.Entry(extra_frame, textvariable=self.var_extra).pack(side=tk.LEFT, fill=tk.X, expand=1, padx=4)

        # Buttons
        btn_frame = tk.Frame(self.root)
        btn_frame.pack(fill=tk.X, padx=6, pady=4)

        self.btn_launch = tk.Button(btn_frame, text="\u25b6 Launch (console)", command=self._launch_runner,
                                    bg="#4a90d9", fg="white", padx=10, font=("Segoe UI", 10))
        self.btn_launch.pack(side=tk.LEFT, padx=2)

        self.btn_server = tk.Button(btn_frame, text="\u25b6 Start Server", command=self._start_server,
                                    bg="#5cb85c", fg="white", padx=10, font=("Segoe UI", 10, "bold"))
        self.btn_server.pack(side=tk.LEFT, padx=2)

        self.btn_stop = tk.Button(btn_frame, text="\u25a0 Stop", command=self._stop_server,
                                  bg="#d9534f", fg="white", padx=10, state=tk.DISABLED)
        self.btn_stop.pack(side=tk.LEFT, padx=2)

        tk.Button(btn_frame, text="Save", command=self._save_config).pack(side=tk.RIGHT, padx=2)
        tk.Button(btn_frame, text="Load", command=self._load_config).pack(side=tk.RIGHT, padx=2)
        tk.Button(btn_frame, text="Open Chat", command=self._open_chat).pack(side=tk.RIGHT, padx=2)

        # Output
        out_frame = tk.Frame(self.root)
        out_frame.pack(fill=tk.BOTH, expand=1, padx=6, pady=2)
        tk.Label(out_frame, text="Output", font=("Segoe UI", 9, "bold"), anchor=tk.W).pack(fill=tk.X)
        self.txt_out = scrolledtext.ScrolledText(out_frame, wrap=tk.WORD, font=("Consolas", 9),
                                                  bg="#1e1e1e", fg="#d4d4d4", state=tk.DISABLED)
        self.txt_out.pack(fill=tk.BOTH, expand=1)

        self.lbl_status = tk.Label(self.root, text="Ready", font=("Segoe UI", 9),
                                   anchor=tk.W, bd=1, relief=tk.SUNKEN)
        self.lbl_status.pack(side=tk.BOTTOM, fill=tk.X)

    def _get_model_path(self):
        sel = self.cb_model.get()
        for label, path in self.models:
            if label == sel:
                return path
        return ""

    def _browse_model(self):
        path = filedialog.askopenfilename(title="Select model",
                                          filetypes=[("GGUF", "*.gguf"), ("All", "*.*")])
        if path:
            self.cb_model.set(os.path.basename(path))

    def _refresh_models(self):
        self._scan_models()
        self.cb_model["values"] = [m[0] for m in self.models]

    def _hostname(self):
        return socket.gethostname().lower().replace(" ", "-")

    # ── Launch (custom runner) ──

    def _launch_runner(self):
        model = self._get_model_path()
        if not model or not os.path.exists(RUNNER_PATH):
            self._status("No model or runner not found", "#d9534f")
            return
        args = [RUNNER_PATH, model]
        for name, var in self.flag_vars.items():
            if isinstance(var, tk.BooleanVar):
                if var.get():
                    args.append(name)
            else:
                val = var.get().strip()
                if val:
                    args.append(name)
                    args.append(val)
        extra = self.var_extra.get().strip()
        if extra:
            args.extend(extra.split())
        args.append("--chat")
        cmd = " ".join(args)
        self._log(f"> {cmd}\n")
        try:
            subprocess.Popen(args, creationflags=subprocess.CREATE_NEW_CONSOLE, cwd=RUNNER_DIR)
            self._status("Launched in console", "#5cb85c")
        except Exception as e:
            self._status(f"Launch failed: {e}", "#d9534f")

    # ── llama-server ──

    def _start_server(self):
        if self.server_proc is not None:
            self._log("[Server already running]\n")
            return

        model = self._get_model_path()
        if not model or not os.path.exists(LLAMA_SERVER_PATH):
            self._status("No model or llama-server not found", "#d9534f")
            return

        host = self.var_host.get().strip() or "0.0.0.0"
        port = int(self.var_port.get().strip() or DEF_PORT)
        ngl = self.var_ngl.get().strip() or "0"
        ctx = self.var_ctx.get().strip() or "2048"

        args = [
            LLAMA_SERVER_PATH,
            "-m", model,
            "--host", host,
            "--port", str(port),
            "--ctx-size", ctx,
        ]

        if ngl != "0":
            args += ["--gpu-layers", ngl]

        mmproj = self.var_mmproj.get().strip()
        if mmproj:
            args += ["--mmproj", mmproj]

        extra = self.var_extra.get().strip()
        if extra:
            args.extend(extra.split())

        self._log(f"> Starting llama-server on {host}:{port}...\n")
        self._log(f">   {' '.join(args)}\n")
        self.btn_server.config(state=tk.DISABLED, text="Starting...")
        self.btn_stop.config(state=tk.NORMAL)
        self._status("Starting server...", "#f0ad4e")

        threading.Thread(target=self._run_server, args=(args, host, port), daemon=True).start()

    def _run_server(self, args, host, port):
        try:
            proc = subprocess.Popen(
                args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
                cwd=LLAMA_SERVER_DIR  # for impl DLL resolution
            )
            self.server_proc = proc

            url = f"http://{host}:{port}"
            started = False

            for line in iter(proc.stdout.readline, ""):
                self.root.after(0, lambda l=line: self._log(l))
                if not started and "server listening" in line.lower():
                    started = True
                    self.root.after(0, lambda u=url: self._on_server_ready(u, port))
                if not line:
                    break

        except Exception as e:
            self.root.after(0, lambda: self._log(f"[Error] {e}\n"))
        finally:
            self.server_proc = None
            self.root.after(0, self._on_server_stop)

    def _on_server_ready(self, url, port):
        self._log(f"\n>>> Server ready: {url}\n")
        self._log(f">>> OpenAI API: {url}/v1/chat/completions\n")
        self._log(f">>> Web UI: {url} (built-in)\n\n")
        self.btn_server.config(state=tk.NORMAL, text="\u25b6 Start Server")
        self.btn_stop.config(state=tk.NORMAL)
        self._status(f"Server running on {url}", "#5cb85c")

        if self.var_mdns.get():
            threading.Thread(target=self._start_mdns, args=(port,), daemon=True).start()

    def _start_mdns(self, port):
        try:
            from zeroconf import Zeroconf, ServiceInfo
            hostname = self._hostname()
            ip = "127.0.0.1"
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.connect(("8.8.8.8", 80))
                ip = s.getsockname()[0]
                s.close()
            except:
                pass
            info = ServiceInfo(
                "_openai._tcp.local.",
                f"LLM Server on {hostname}._openai._tcp.local.",
                addresses=[socket.inet_aton(ip)],
                port=port,
                properties={"path": "/v1", "model": os.path.basename(self._get_model_path())},
                server=f"{hostname}.local.",
            )
            zc = Zeroconf()
            zc.register_service(info)
            self.mdns_service = (zc, info)
            self.root.after(0, lambda: self._log(f">>> mDNS: {hostname}.local:{port} ({ip})\n"))
        except Exception as e:
            self.root.after(0, lambda: self._log(f"[mDNS] {e}\n"))

    def _open_chat(self):
        if os.path.exists(CHAT_HTML):
            os.startfile(CHAT_HTML)

    def _stop_server(self):
        if self.mdns_service:
            try:
                zc, info = self.mdns_service
                zc.unregister_service(info)
                zc.close()
            except:
                pass
            self.mdns_service = None
        if self.server_proc:
            try:
                self.server_proc.terminate()
                self.server_proc.wait(5)
            except:
                self.server_proc.kill()
            self.server_proc = None
        self._on_server_stop()

    def _on_server_stop(self):
        self.server_proc = None
        self.btn_server.config(state=tk.NORMAL, text="\u25b6 Start Server")
        self.btn_stop.config(state=tk.DISABLED)
        self._status("Server stopped", "#d9534f")
        self._log("[Server stopped]\n")

    # ── Config ──

    def _save_config(self):
        data = {
            "model": self.cb_model.get(),
            "host": self.var_host.get(),
            "port": self.var_port.get(),
            "mdns": self.var_mdns.get(),
            "ngl": self.var_ngl.get(),
            "ctx": self.var_ctx.get(),
            "mmproj": self.var_mmproj.get(),
        }
        for name, var in self.flag_vars.items():
            if isinstance(var, tk.BooleanVar):
                data[name] = var.get()
            else:
                data[name] = var.get()
        data["extra"] = self.var_extra.get()
        try:
            with open(CONFIG_PATH, "w") as f:
                json.dump(data, f, indent=2)
            self._status("Config saved", "#5cb85c")
        except Exception as e:
            self._status(f"Save failed: {e}", "#d9534f")

    def _load_config(self, event=None):
        if not os.path.exists(CONFIG_PATH):
            return
        try:
            with open(CONFIG_PATH) as f:
                d = json.load(f)
            self.cb_model.set(d.get("model", ""))
            self.var_host.set(d.get("host", "0.0.0.0"))
            self.var_port.set(d.get("port", str(DEF_PORT)))
            self.var_mdns.set(d.get("mdns", True))
            self.var_ngl.set(d.get("ngl", "0"))
            self.var_ctx.set(d.get("ctx", "2048"))
            self.var_mmproj.set(d.get("mmproj", ""))
            for name, var in self.flag_vars.items():
                val = d.get(name)
                if val is not None:
                    if isinstance(var, tk.BooleanVar):
                        var.set(bool(val))
                    else:
                        var.set(str(val))
            self.var_extra.set(d.get("extra", ""))
            self._status("Config loaded", "#5cb85c")
        except Exception as e:
            self._status(f"Load failed: {e}", "#d9534f")

    # ── Utilities ──

    def _log(self, text):
        self.txt_out.config(state=tk.NORMAL)
        self.txt_out.insert(tk.END, text)
        self.txt_out.see(tk.END)
        self.txt_out.config(state=tk.DISABLED)

    def _status(self, msg, color="#666"):
        self.lbl_status.config(text=msg, fg=color)


def main():
    root = tk.Tk()
    app = LLMLauncher(root)
    root.mainloop()


if __name__ == "__main__":
    main()
