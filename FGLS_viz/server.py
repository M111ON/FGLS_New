#!/usr/bin/env python3
"""Simple FGLS_viz server — serves files + runs tests"""
import http.server
import json
import os
import subprocess
import threading

PORT = 8081
DIR = os.path.dirname(os.path.abspath(__file__))
FGLS_KIS = 'I:/FGLS_kis'
GGUF = 'I:/model/SmolLM2-360M-Instruct.Q8_0.gguf'

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DIR, **kw)
    
    def do_GET(self):
        if self.path == '/api/status':
            self._json({"ok": True, "port": PORT})
        elif self.path == '/api/run-tests':
            self._json({"status": "started"})
            threading.Thread(target=self._run_tests, daemon=True).start()
        elif self.path.startswith('/api/run-gguf'):
            # Parse GGUF path from query string
            from urllib.parse import urlparse, parse_qs
            qs = parse_qs(urlparse(self.path).query)
            gguf_path = qs.get('path', [None])[0]
            if gguf_path:
                threading.Thread(target=self._run_gguf, args=(gguf_path,), daemon=True).start()
                self._json({"status": "started", "file": gguf_path})
            else:
                self._json({"error": "missing ?path="}, 400)
        elif self.path.startswith('/api/results'):
            self._serve_file('adaptive_data.json', 'application/json')
        else:
            super().do_GET()
    
    def _json(self, data):
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    
    def _serve_file(self, filename, ctype):
        path = os.path.join(DIR, filename)
        if os.path.exists(path):
            with open(path, 'rb') as f:
                body = f.read()
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)
    
    def _run_tests(self):
        tests = [
            (f'{FGLS_KIS}/runner/explore/kis_adaptive_deploy.exe', []),
            (f'{FGLS_KIS}/runner/explore/kis_gguf_bridge.exe', [GGUF]),
            (f'{FGLS_KIS}/runner/explore/kis_real_gguf_test.exe', [GGUF]),
            (f'{FGLS_KIS}/runner/explore/kis_adaptive_export.exe', []),
        ]
        results = []
        for exe, args in tests:
            name = os.path.basename(exe)
            try:
                r = subprocess.run([exe] + args, capture_output=True, text=True, timeout=60, cwd=FGLS_KIS)
                output = r.stdout + r.stderr
                passed = sum(1 for l in output.split('\n') if 'PASS' in l and ':' in l)
                failed = sum(1 for l in output.split('\n') if 'FAIL' in l and ':' in l)
                results.append({"name": name, "passed": passed, "failed": failed})
            except Exception as e:
                results.append({"name": name, "error": str(e)})
        
        # Save adaptive data
        try:
            r = subprocess.run([f'{FGLS_KIS}/runner/explore/kis_adaptive_export.exe'], 
                             capture_output=True, text=True, timeout=30, cwd=FGLS_KIS)
            if r.returncode == 0:
                with open(os.path.join(DIR, 'adaptive_data.json'), 'w') as f:
                    f.write(r.stdout)
        except:
            pass
        
        print(f"Tests complete: {results}", flush=True)
    
    def _run_gguf(self, gguf_path):
        """Run analysis on real GGUF file via Python analyzer"""
        try:
            analyzer = os.path.join(DIR, 'gguf_analyzer.py')
            r = subprocess.run(
                ['python', analyzer, gguf_path],
                capture_output=True, text=True, timeout=120
            )
            if r.returncode == 0:
                data = json.loads(r.stdout)
                with open(os.path.join(DIR, 'adaptive_data.json'), 'w') as f:
                    json.dump(data, f, indent=2)
                stats = data.get('container_stats', {})
                tiers = data.get('tier_distribution', {})
                print(f"GGUF analysis complete: {os.path.basename(gguf_path)} "
                      f"({stats.get('file_size_mb', '?')}MB, "
                      f"n_tensors={stats.get('n_tensors', '?')}, "
                      f"T0={tiers.get('tier0',0)} T1={tiers.get('tier1',0)} "
                      f"T2={tiers.get('tier2',0)} T3={tiers.get('tier3',0)})", flush=True)
            else:
                print(f"GGUF analyzer failed: {r.stderr[:200]}", flush=True)
        except Exception as e:
            print(f"GGUF analysis error: {e}", flush=True)
if __name__ == '__main__':
    print(f'FGLS_viz at http://localhost:{PORT}')
    server = http.server.ThreadingHTTPServer(('0.0.0.0', PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()
