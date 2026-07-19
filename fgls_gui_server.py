#!/usr/bin/env python3
"""
FGLS GUI Server — local HTTP backend
═════════════════════════════════════
Serves HTML + API that calls fgls.exe / geofield_full.exe / pro_cli.exe.
File path: user types path OR browser uploads file via /api/upload.
"""

import json, os, socket, subprocess, sys, threading, time, urllib.parse, uuid
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FGLS_EXE  = SCRIPT_DIR / "fgls.exe"
GEO_EXE   = SCRIPT_DIR / "geofield_full.exe"
PRO_EXE   = SCRIPT_DIR / "pro_cli.exe"
UPLOAD_DIR = SCRIPT_DIR / "uploads"
UPLOAD_DIR.mkdir(exist_ok=True)
PORT = 8080

MIME = {'.html':'text/html; charset=utf-8','.css':'text/css; charset=utf-8',
        '.js':'application/javascript','.png':'image/png','.ico':'image/x-icon',
        '.json':'application/json','.svg':'image/svg+xml'}

def find_exec(name):
    c = SCRIPT_DIR / name
    return str(c) if c.is_file() else None

def fmt_size(n):
    if n < 1024: return f"{n:,} B"
    if n < 1048576: return f"{n/1024:.1f} KB"
    return f"{n/1048576:.2f} MB"

def resolve_path(p):
    """Resolve a path — could be upload-relative or absolute."""
    p = p.strip().strip('"').strip("'")
    if (UPLOAD_DIR / p).is_file(): return str(UPLOAD_DIR / p)
    if Path(p).is_file(): return p
    if (SCRIPT_DIR / p).is_file(): return str(SCRIPT_DIR / p)
    return p  # let the caller fail with a clear error

def run_cli(cmd, timeout=300):
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=str(SCRIPT_DIR))
        stdout, stderr = p.communicate(timeout=timeout)
        ok = (p.returncode == 0)
        return ok, stdout.decode('utf-8',errors='replace').strip(), stderr.decode('utf-8',errors='replace').strip()
    except subprocess.TimeoutExpired: p.kill(); return False, '', f'Timed out ({timeout}s)'
    except FileNotFoundError: return False, '', f'Not found: {cmd[0]}'

# ── API handlers ──

def _do_upload(args, body_bytes):
    """Save uploaded file bytes to uploads/ dir."""
    # Parse multipart/form-data
    ct = args.get('_content_type', '')
    boundary = ''
    if 'boundary=' in ct:
        boundary = ct.split('boundary=')[1].split(';')[0].strip()
        if boundary.startswith('"') and boundary.endswith('"'): boundary = boundary[1:-1]
    if not boundary:
        # Fallback: raw body, use first bytes as filename
        fname = f"upload_{uuid.uuid4().hex[:12]}"
        dst = UPLOAD_DIR / fname
        dst.write_bytes(body_bytes)
        return {'ok': True, 'path': fname, 'size': len(body_bytes)}
    
    # Parse multipart
    parts = body_bytes.split(b'--' + boundary.encode())
    saved = None
    for part in parts:
        if b'Content-Disposition' not in part: continue
        # Extract filename
        hdr_end = part.find(b'\r\n\r\n')
        if hdr_end < 0: continue
        headers_raw = part[:hdr_end].decode('utf-8', errors='replace')
        data = part[hdr_end+4:]
        # Chop trailing \r\n and --
        if data.endswith(b'\r\n'): data = data[:-2]
        if data.endswith(b'--'): data = data[:-2]
        if data.endswith(b'\r\n'): data = data[:-2]
        # Get filename
        fname = None
        if 'filename="' in headers_raw:
            fname = headers_raw.split('filename="')[1].split('"')[0]
        if not fname:
            fname = f"upload_{uuid.uuid4().hex[:12]}"
        # Avoid path traversal
        fname = Path(fname).name
        dst = UPLOAD_DIR / fname
        # If exists, add suffix
        if dst.exists():
            stem = dst.stem
            dst = UPLOAD_DIR / f"{stem}_{uuid.uuid4().hex[:6]}{dst.suffix}"
        dst.write_bytes(data)
        saved = {'ok': True, 'path': str(dst.relative_to(UPLOAD_DIR)), 'size': len(data), 'name': fname}
        break
    if saved: return saved
    return {'ok': False, 'error': 'No file data found in upload'}

def _do_encode(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('fgls.exe')
    if not exe: return {'ok': False, 'error': 'fgls.exe not found'}
    dst = src + '.fgls'
    ok, out, err = run_cli([exe, 'encode', src, dst])
    r = {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}
    if os.path.isfile(dst):
        r['output_file'] = os.path.basename(dst)
        r['output_path'] = os.path.abspath(dst)
        r['output_size'] = os.path.getsize(dst)
        if r['input_size']: r['ratio'] = r['output_size'] / r['input_size']
    return r

def _do_decode(args):
    src = resolve_path(args.get('path', ''))
    if not src or not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('fgls.exe')
    if not exe: return {'ok': False, 'error': 'fgls.exe not found'}
    p = Path(src)
    dst = str(p.parent / p.stem)
    ok, out, err = run_cli([exe, 'decode', src, dst])
    r = {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}
    if os.path.isfile(dst):
        r['output_file'] = os.path.basename(dst)
        r['output_path'] = os.path.abspath(dst)
        r['output_size'] = os.path.getsize(dst)
    return r

def _do_info(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('fgls.exe')
    if not exe: return {'ok': False, 'error': 'fgls.exe not found'}
    ok, out, err = run_cli([exe, 'info', src])
    if not ok:
        geo = find_exec('geofield_full.exe')
        if geo: ok, out, err = run_cli([geo, 'info', src])
    return {'ok': ok, 'output': out, 'error': err, 'file': os.path.basename(src), 'size': os.path.getsize(src)}

def _do_bench(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    for exe_name in ('fgls.exe','pro_cli.exe'):
        exe = find_exec(exe_name)
        if exe:
            ok, out, err = run_cli([exe, 'bench', src])
            if ok: return {'ok': True, 'output': out, 'error': err}
    return {'ok': False, 'error': 'No bench exe found'}

def _do_profile(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('fgls.exe')
    if not exe: return {'ok': False, 'error': 'fgls.exe not found'}
    ok, out, err = run_cli([exe, 'profile', src])
    return {'ok': ok, 'output': out, 'error': err}

def _do_geofield_enc(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('geofield_full.exe')
    if not exe: return {'ok': False, 'error': 'geofield_full.exe not found'}
    dst = src + '.gfuf'
    ok, out, err = run_cli([exe, 'encode', src, dst])
    r = {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}
    if os.path.isfile(dst):
        r['output_file'] = os.path.basename(dst)
        r['output_path'] = os.path.abspath(dst)
        r['output_size'] = os.path.getsize(dst)
        if r['input_size']: r['ratio'] = r['output_size'] / r['input_size']
    return r

def _do_geofield_dec(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('geofield_full.exe')
    if not exe: return {'ok': False, 'error': 'geofield_full.exe not found'}
    dst = src + '.dec'
    ok, out, err = run_cli([exe, 'decode', src, dst])
    r = {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}
    if os.path.isfile(dst):
        r['output_file'] = os.path.basename(dst)
        r['output_path'] = os.path.abspath(dst)
        r['output_size'] = os.path.getsize(dst)
    return r

def _do_sid_capture(args):
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('pro_cli.exe')
    if not exe: return {'ok': False, 'error': 'pro_cli.exe not found (Pro)'}
    dst = src + '.twidx'
    ok, out, err = run_cli([exe, 'capture', src, dst])
    r = {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src)}
    if os.path.isfile(dst):
        r['output_file'] = os.path.basename(dst)
        r['output_path'] = os.path.abspath(dst)
    return r

def _do_ping(args):
    return {'ok': True, 'version': 'FGLS GUI v2.0.0', 'pid': os.getpid()}

def _do_download(args):
    """Serve a file from uploads/ for download."""
    path = args.get('path', '')
    if not path: return {'ok': False, 'error': 'No path specified'}
    f = UPLOAD_DIR / Path(path).name
    if not f.is_file(): return {'ok': False, 'error': f'File not found: {f}'}
    return {'ok': True, '_file': str(f)}  # special: served as raw file in handler

ROUTES = {
    'upload': _do_upload, 'download': _do_download,
    'ping': _do_ping, 'info': _do_info,
    'encode': _do_encode, 'decode': _do_decode,
    'bench': _do_bench, 'profile': _do_profile,
    'geofield_enc': _do_geofield_enc, 'geofield_dec': _do_geofield_dec,
    'sid_capture': _do_sid_capture,
}

# ── HTTP ──

def handle_client(conn, addr):
    try:
        conn.settimeout(30)
        data = b''
        # Read headers
        while b'\r\n\r\n' not in data:
            try:
                chunk = conn.recv(65536)
                if not chunk: break
                data += chunk
            except socket.timeout: break
            except: break
        if not data: conn.close(); return
        
        # Parse request line + headers
        text = data.split(b'\r\n\r\n')[0].decode('utf-8', errors='replace')
        lines = text.split('\r\n')
        if not lines: conn.close(); return
        first = lines[0].split(' ')
        if len(first) < 2: conn.close(); return
        method, path_raw = first[0], first[1]
        parsed = urllib.parse.urlparse(path_raw)
        path = parsed.path.rstrip('/')
        
        # Parse Content-Length
        cl = 0
        ct = ''
        for line in lines[1:]:
            l = line.lower()
            if l.startswith('content-length:'): cl = int(line.split(':')[1].strip())
            if l.startswith('content-type:'): ct = line.split(':',1)[1].strip()
        
        # Read body if needed
        body = data.split(b'\r\n\r\n', 1)[1] if b'\r\n\r\n' in data else b''
        while len(body) < cl:
            try:
                chunk = conn.recv(65536)
                if not chunk: break
                body += chunk
            except: break
        
        # ── API ──
        if path.startswith('/api/'):
            endpoint = path[5:]
            
            # Parse args from query + JSON body
            qs = urllib.parse.parse_qs(parsed.query)
            args = {k: v[0] for k, v in qs.items()}
            if 'application/json' in ct and body:
                try: args.update(json.loads(body))
                except: pass
            elif ct: args['_content_type'] = ct
            
            handler = ROUTES.get(endpoint)
            if not handler:
                _send_json(conn, 404, {'error': f'Unknown: {endpoint}'})
                conn.close(); return
            
            try:
                if endpoint == 'upload':
                    result = handler(args, body)
                else:
                    result = handler(args)
                # Special: file download
                if '_file' in result:
                    fpath = result['_file']
                    try:
                        d = Path(fpath).read_bytes()
                        _send_raw(conn, 200, d, 'application/octet-stream')
                    except: _send_raw(conn, 404, b'Not Found')
                else:
                    _send_json(conn, 200, result)
            except Exception as e:
                _send_json(conn, 500, {'ok': False, 'error': str(e)})
            conn.close(); return
        
        # ── Static ──
        if path == '' or path == '/': path = '/fgls_gui.html'
        fpath = SCRIPT_DIR / path.lstrip('/')
        try:
            d = fpath.read_bytes()
            _send_raw(conn, 200, d, MIME.get(fpath.suffix.lower(), 'application/octet-stream'))
        except FileNotFoundError:
            _send_raw(conn, 404, b'Not Found')
        conn.close()
    except Exception as e:
        print(f'[ERR] {addr}: {e}')
        try: _send_raw(conn, 500, b'Internal Error')
        except: pass
        try: conn.close()
        except: pass

def _send_raw(conn, status, body, mime='application/octet-stream'):
    reasons = {200:'OK',400:'Bad Request',404:'Not Found',500:'Internal Server Error'}
    h = (f'HTTP/1.1 {status} {reasons.get(status,"?")}\r\n'
         f'Content-Type: {mime}\r\nContent-Length: {len(body)}\r\n'
         f'Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n').encode()
    conn.sendall(h + body)

def _send_json(conn, status, data):
    _send_raw(conn, status, json.dumps(data, ensure_ascii=False).encode(), 'application/json; charset=utf-8')

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', PORT))
    s.listen(16)
    s.settimeout(1)
    print(f'FGLS GUI at http://localhost:{PORT}')
    try:
        while True:
            try:
                conn, addr = s.accept()
                threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
            except socket.timeout: continue
            except KeyboardInterrupt: break
    finally:
        s.close()
        print('Stopped.')

def open_browser():
    time.sleep(0.8)
    import webbrowser
    webbrowser.open(f'http://localhost:{PORT}')

if __name__ == '__main__':
    print('FGLS GUI v2.0.0')
    for name in ('fgls.exe','geofield_full.exe','pro_cli.exe'):
        print(f'  {name:25s} {"✓" if (SCRIPT_DIR/name).is_file() else "✗"}')
    print()
    threading.Thread(target=open_browser, daemon=True).start()
    serve()
