#!/usr/bin/env python3
"""
fg-session.py — Live Hermes session monitor
Reads from Hermes state.db and shows agent activity in terminal.

Usage:
  python fg-session.py           - List recent sessions
  python fg-session.py live      - Monitor latest session (live)
  python fg-session.py live <N>  - Monitor session index N
"""
import sqlite3, time, os, sys, json
from datetime import datetime

DB_PATH = "I:\\hermes\\state.db"
POLL_SEC = 2
INITIAL_LINES = 25  # show last N on startup

def connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def get_sessions(conn, limit=15):
    return conn.execute("""
        SELECT id, title, started_at, ended_at, message_count, git_branch, cwd
        FROM sessions WHERE source != 'cron'
        ORDER BY started_at DESC LIMIT ?
    """, (limit,)).fetchall()

def get_messages(conn, session_id, since_id=0, limit=200):
    return conn.execute("""
        SELECT id, role, content, tool_name, tool_calls, timestamp,
               effect_disposition
        FROM messages
        WHERE session_id = ? AND id > ? AND active = 1
        ORDER BY id ASC LIMIT ?
    """, (session_id, since_id, limit)).fetchall()

def ts(t):
    return datetime.fromtimestamp(t).strftime("%H:%M:%S")

def fmt_line(row):
    """Return (formatted_line, sort_key) -- ASCII-only for git-bash"""
    role = row["role"]
    t = ts(row["timestamp"])
    content = to_ascii(row["content"] or "")
    tool = row["tool_name"] or ""
    tc_raw = row["tool_calls"]
    eff = row["effect_disposition"] or ""

    if role == "user":
        return f"  \033[33m{t}\033[0m \033[36m>\033[0m {trunc(content, 100)}", t

    if role == "assistant":
        if tc_raw:
            try:
                calls = json.loads(tc_raw)
                for c in calls if isinstance(calls, list) else [calls]:
                    fn = c.get("function", {}).get("name", "?")
                    return f"  \033[33m{t}\033[0m \033[35m#\033[0m {fn}", t
            except:
                pass
        if content:
            return f"  \033[33m{t}\033[0m \033[32m|\033[0m {trunc(content, 100)}", t
        return "", t

    if role == "tool":
        if tool == "terminal":
            try:
                out = json.loads(content) if content else {}
                out_txt = out.get("output","") if isinstance(out,dict) else str(content)[:200]
                ec = out.get("exit_code") if isinstance(out,dict) else None
                # Show first line of output (usually the most informative)
                first_line = out_txt.split("\n")[0][:120] if out_txt else ""
                status = "\033[32mOK\033[0m" if ec == 0 else \
                         f"\033[31m!{ec}\033[0m" if ec else "\033[90m--\033[0m"
                return f"  \033[33m{t}\033[0m [{status}] {trunc(first_line, 120)}", t
            except:
                return f"  \033[33m{t}\033[0m \033[90m~>\033[0m {trunc(content, 120)}", t
        elif tool in ("read_file","write_file","patch","search_files"):
            return f"  \033[33m{t}\033[0m \033[36m[EDIT]\033[0m {tool}: {trunc(content, 120)}", t
        elif eff == "error":
            return f"  \033[33m{t}\033[0m \033[31m[ERR]\033[0m {tool}: {trunc(content,120)}", t
        else:
            return f"  \033[33m{t}\033[0m \033[90m~>\033[0m {tool}: {trunc(content, 120)}", t
    return "", t

def to_ascii(s):
    """Strip non-ASCII characters (git-bash can't render Thai/Unicode)."""
    if not s: return ""
    return s.encode("ascii", "ignore").decode("ascii")

def trunc(s, n=120):
    if not s: return ""
    s = s.replace("\r\n"," ").replace("\n"," ").replace("\r"," ")
    return s if len(s) <= n else s[:n-3]+"..."

def header(session):
    title = to_ascii(session["title"]) or "(no title)"
    cwd = session["cwd"] or "?"
    started = ts(session["started_at"])
    count = session["message_count"] or 0
    print(f"\033[1;36m{'='*60}\033[0m")
    print(f"\033[1;37m  {title}\033[0m")
    print(f"  \033[90m{started}  |  {cwd}  |  {count} msgs\033[0m")
    print(f"\033[1;36m{'='*60}\033[0m")

def live_mode(session_id):
    conn = connect()
    last_id = 0
    lines = []

    os.system("cls" if os.name == "nt" else "clear")
    sess = conn.execute("SELECT * FROM sessions WHERE id=?", (session_id,)).fetchone()
    if sess: header(sess)
    print(f"  \033[90mLIVE: poll {POLL_SEC}s  |  Ctrl+C to stop\033[0m\n")

    # Load initial batch (last N messages)
    all_msgs = conn.execute("""
        SELECT id FROM messages
        WHERE session_id=? AND active=1
        ORDER BY id DESC LIMIT ?
    """, (session_id, INITIAL_LINES)).fetchall()
    
    if all_msgs:
        earliest_id = all_msgs[-1]["id"]
        msgs = get_messages(conn, session_id, since_id=earliest_id-1)
        for row in msgs:
            line, _ = fmt_line(row)
            if line.strip():
                lines.append(line)
            last_id = row["id"]
    
    for line in lines:
        print(line)

    try:
        while True:
            msgs = get_messages(conn, session_id, since_id=last_id)
            if msgs:
                for row in msgs:
                    line, _ = fmt_line(row)
                    if line.strip():
                        lines.append(line)
                    last_id = row["id"]
                if len(lines) > 500:
                    lines = lines[-500:]
                # Re-render
                os.system("cls" if os.name == "nt" else "clear")
                sess = conn.execute("SELECT * FROM sessions WHERE id=?", (session_id,)).fetchone()
                if sess: header(sess)
                print(f"  \033[90mLIVE: poll {POLL_SEC}s  |  {len(lines)} events  |  Ctrl+C to stop\033[0m\n")
                for line in lines[-40:]:
                    print(line)
            time.sleep(POLL_SEC)
    except KeyboardInterrupt:
        print("\n\033[33m[stopped]\033[0m")

def list_sessions():
    conn = connect()
    sessions = get_sessions(conn, 15)
    os.system("cls" if os.name == "nt" else "clear")
    print(f"\033[1;36m{'='*60}\033[0m")
    print(f"\033[1;37m  Hermes Sessions  (\033[33m{len(sessions)}\033[37m recent)\033[0m")
    print(f"\033[1;36m{'='*60}\033[0m\n")
    for i, s in enumerate(sessions):
        title = to_ascii(s["title"]) or "(no title)"
        started = ts(s["started_at"])
        branch = s["git_branch"] or ""
        count = s["message_count"] or 0
        marker = "\033[32m>\033[0m" if i == 0 else " "
        print(f"  {marker} \033[33m[{i}]\033[0m \033[1;37m{title}\033[0m")
        print(f"      \033[90m{started}  {count} msgs  {branch}\033[0m\n")
    print(f"  \033[33m>>> python fg-session.py live [N]\033[0m\n")

def main():
    if len(sys.argv) < 2:
        return list_sessions()
    cmd = sys.argv[1]
    if cmd == "live":
        conn = connect()
        session_id = None
        if len(sys.argv) > 2:
            arg = sys.argv[2]
            if arg.isdigit():
                sessions = get_sessions(conn, 20)
                idx = int(arg)
                session_id = sessions[idx]["id"] if idx < len(sessions) else None
            else:
                session_id = arg
        else:
            row = conn.execute("SELECT id FROM sessions WHERE source!='cron' ORDER BY started_at DESC LIMIT 1").fetchone()
            session_id = row["id"] if row else None
        if not session_id:
            print("No session found.")
            return
        live_mode(session_id)
    elif cmd == "list":
        list_sessions()
    else:
        print("Usage: python fg-session.py [list|live [N]]")

if __name__ == "__main__":
    main()
