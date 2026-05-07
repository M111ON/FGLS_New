"""
browser_chat_server.py

Minimal local-browser chat UI for the POGLS / llama runner stack.

It serves:
  - /chat          : single-page browser chat UI
  - /chat/api/turn : POST a user message and stream the assistant reply
  - /chat/api/history/{session_id}
  - /chat/api/reset
  - /health

The server does not run llama directly. It proxies each turn to the existing
core/pogls_runner.exe streaming runner and prepends memory context from
goldberg_field_core/llm_memory_search.py when enabled.
"""

from __future__ import annotations

import asyncio
import codecs
import html
import json
import logging
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional
from uuid import uuid4

from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel


log = logging.getLogger("pogls.browser_chat")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-8s %(name)s — %(message)s",
)

CORE_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = CORE_DIR.parent
DEFAULT_RUNNER = CORE_DIR / "pogls_runner.exe"
DEFAULT_MODEL = os.environ.get(
    "POGLS_CHAT_MODEL",
    r"I:\Vault\models\qwen2.5-0.5b-instruct-q4_k_m.gguf",
)
DEFAULT_PORT = int(os.environ.get("POGLS_CHAT_PORT", "8766"))
DEFAULT_HOST = os.environ.get("POGLS_CHAT_HOST", "127.0.0.1")
DEFAULT_TOKENS = max(1, int(os.environ.get("POGLS_CHAT_TOKENS", "160")))
MEMORY_TOP = max(1, int(os.environ.get("POGLS_CHAT_MEMORY_TOP", "3")))
MEMORY_PREVIEW = max(64, int(os.environ.get("POGLS_CHAT_MEMORY_PREVIEW", "220")))
TURN_LIMIT = 8


class ChatTurnBody(BaseModel):
    session_id: str
    message: str
    model_path: Optional[str] = None
    use_memory: bool = True
    stream_tokens: Optional[int] = None


class ResetBody(BaseModel):
    session_id: str


class SessionUpdateBody(BaseModel):
    session_id: str
    model_path: Optional[str] = None
    use_memory: Optional[bool] = None


@dataclass
class ChatSession:
    session_id: str
    model_path: str = DEFAULT_MODEL
    use_memory: bool = True
    created_at: float = field(default_factory=time.time)
    turns: list[dict[str, str]] = field(default_factory=list)
    runner_proc: object | None = None
    runner_stderr_task: object | None = None
    runner_model_path: str = ""
    runner_lock: object | None = None

    def append_turn(self, user_msg: str, assistant_msg: str) -> None:
        self.turns.append({"user": user_msg, "assistant": assistant_msg})
        if len(self.turns) > TURN_LIMIT:
            self.turns[:] = self.turns[-TURN_LIMIT:]


sessions: dict[str, ChatSession] = {}


def _get_session(session_id: str, model_path: Optional[str] = None) -> ChatSession:
    session_id = (session_id or "").strip() or uuid4().hex
    rec = sessions.get(session_id)
    if rec is None:
        rec = ChatSession(session_id=session_id, model_path=(model_path or DEFAULT_MODEL).strip())
        sessions[session_id] = rec
        log.info("new chat session: %s", session_id)
    elif model_path and model_path.strip():
        rec.model_path = model_path.strip()
    return rec


def _ensure_lock(rec: ChatSession) -> asyncio.Lock:
    lock = rec.runner_lock
    if isinstance(lock, asyncio.Lock):
        return lock
    lock = asyncio.Lock()
    rec.runner_lock = lock
    return lock


def _discover_models() -> list[str]:
    roots = []
    env_models = os.environ.get("POGLS_CHAT_MODELS_DIR", "").strip()
    if env_models:
        roots.append(Path(env_models))
    roots.extend([
        Path(r"I:\Vault\models"),
        Path(r"I:\llama\models"),
    ])

    seen: set[str] = set()
    models: list[str] = []
    for root in roots:
        if not root.exists():
            continue
        for pattern in ("*.gguf", "*.GGUF"):
            for path in root.rglob(pattern):
                s = os.fspath(path)
                if s not in seen:
                    seen.add(s)
                    models.append(s)
    models.sort(key=lambda s: (Path(s).name.lower(), len(s)))
    if DEFAULT_MODEL not in seen:
        models.insert(0, DEFAULT_MODEL)
    return models[:48]


def _resolve_runner_path() -> Path:
    candidates = []
    env_runner = os.environ.get("POGLS_CHAT_RUNNER", "").strip()
    if env_runner:
        candidates.append(Path(env_runner))
    candidates.extend([
        DEFAULT_RUNNER,
        Path("core") / "pogls_runner.exe",
    ])
    for candidate in candidates:
        if candidate and candidate.exists():
            return candidate
    return DEFAULT_RUNNER


def _resolve_memory_search_script() -> Path | None:
    candidates = [
        CORE_DIR / "goldberg_field_core" / "llm_memory_search.py",
        REPO_ROOT / "core" / "goldberg_field_core" / "llm_memory_search.py",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def _run_memory_search(query: str) -> str:
    query = (query or "").strip()
    if not query:
        return ""

    script = _resolve_memory_search_script()
    if script is None:
        return ""

    env = os.environ.copy()
    env.setdefault("POGLS_MEMORY_STORE", str(CORE_DIR / "goldberg_field_core" / "memory_store"))

    cmd = [
        sys.executable,
        os.fspath(script),
        "--query",
        query,
        "--top",
        str(MEMORY_TOP),
        "--max-preview",
        str(MEMORY_PREVIEW),
    ]

    try:
        proc = subprocess.run(
            cmd,
            cwd=os.fspath(CORE_DIR),
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except Exception as exc:
        log.warning("memory search failed: %s", exc)
        return ""

    if proc.returncode != 0:
        log.warning("memory search rc=%s stderr=%s", proc.returncode, (proc.stderr or "").strip())
        return ""
    return (proc.stdout or "").strip()


def _build_prompt(turns: list[dict[str, str]], user_msg: str, memory_context: str) -> str:
    parts: list[str] = [
        "You are a local assistant running inside this repo.",
        "Keep answers concise, direct, and grounded in memory only when relevant.",
        "If memory is unrelated, ignore it.",
        "",
    ]

    if memory_context.strip():
        parts.append(memory_context.strip())
        parts.append("")

    history = turns[-TURN_LIMIT:] if len(turns) > TURN_LIMIT else turns
    if history:
        parts.append("Conversation history:")
        for turn in history:
            parts.append(f"User: {turn.get('user', '').strip()}")
            parts.append(f"Assistant: {turn.get('assistant', '').strip()}")
        parts.append("")

    parts.append(f"User: {user_msg.strip()}")
    parts.append("Assistant:")
    return "\n".join(parts)


def _normalize_model_path(model_path: Optional[str]) -> str:
    model_path = (model_path or "").strip() or DEFAULT_MODEL
    if not Path(model_path).exists():
        raise HTTPException(400, f"model not found: {model_path}")
    return model_path


async def _stream_runner_output(model_path: str, prompt: str, stream_tokens: int):
    runner = _resolve_runner_path()
    if not runner.exists():
        raise HTTPException(500, f"runner not found: {runner}")

    cmd = [os.fspath(runner), model_path, str(stream_tokens), prompt]
    kwargs: dict[str, object] = {
        "stdout": asyncio.subprocess.PIPE,
        "stderr": asyncio.subprocess.PIPE,
        "cwd": os.fspath(CORE_DIR),
    }
    if os.name == "nt" and hasattr(subprocess, "CREATE_NO_WINDOW"):
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW

    proc = await asyncio.create_subprocess_exec(*cmd, **kwargs)

    async def drain_stderr() -> None:
        if proc.stderr is None:
            return
        while True:
            chunk = await proc.stderr.read(1024)
            if not chunk:
                break
            text = chunk.decode("utf-8", errors="replace").strip()
            if text:
                log.info("[runner] %s", text)

    stderr_task = asyncio.create_task(drain_stderr()) if proc.stderr is not None else None
    try:
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        first_chunk = True
        if proc.stdout is not None:
            while True:
                chunk = await proc.stdout.read(256)
                if not chunk:
                    break
                text = decoder.decode(chunk)
                if first_chunk:
                    text = text.replace("[stream] ", "", 1)
                    first_chunk = False
                yield text
        tail = decoder.decode(b"", final=True)
        if tail:
            yield tail
        await proc.wait()
    finally:
        if stderr_task is not None:
            await stderr_task


async def _stop_runner_process(rec: ChatSession) -> None:
    proc = rec.runner_proc
    if proc is not None:
        try:
            if proc.returncode is None:
                proc.terminate()
                try:
                    await asyncio.wait_for(proc.wait(), timeout=5.0)
                except Exception:
                    proc.kill()
                    await proc.wait()
        except Exception as exc:
            log.warning("runner stop failed: %s", exc)
    task = rec.runner_stderr_task
    if isinstance(task, asyncio.Task) and not task.done():
        task.cancel()
        try:
            await task
        except Exception:
            pass
    rec.runner_proc = None
    rec.runner_stderr_task = None
    rec.runner_model_path = ""


async def _ensure_runner_process(rec: ChatSession) -> None:
    model_path = _normalize_model_path(rec.model_path)
    if rec.runner_proc is not None and getattr(rec.runner_proc, "returncode", None) is None:
        if rec.runner_model_path == model_path:
            return
        await _stop_runner_process(rec)

    runner = _resolve_runner_path()
    if not runner.exists():
        raise HTTPException(500, f"runner not found: {runner}")

    cmd = [os.fspath(runner), model_path, "--browser-chat"]
    kwargs: dict[str, object] = {
        "stdin": asyncio.subprocess.PIPE,
        "stdout": asyncio.subprocess.PIPE,
        "stderr": asyncio.subprocess.PIPE,
        "cwd": os.fspath(CORE_DIR),
    }
    if os.name == "nt" and hasattr(subprocess, "CREATE_NO_WINDOW"):
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW

    proc = await asyncio.create_subprocess_exec(*cmd, **kwargs)
    if proc.stdout is None or proc.stdin is None:
        raise HTTPException(500, "runner pipes unavailable")

    async def drain_stderr() -> None:
        if proc.stderr is None:
            return
        while True:
            chunk = await proc.stderr.read(1024)
            if not chunk:
                break
            text = chunk.decode("utf-8", errors="replace").strip()
            if text:
                log.info("[runner] %s", text)

    rec.runner_proc = proc
    rec.runner_model_path = model_path
    rec.runner_stderr_task = asyncio.create_task(drain_stderr()) if proc.stderr is not None else None

    ready = await proc.stdout.readline()
    if not ready:
        await _stop_runner_process(rec)
        raise HTTPException(500, "runner exited before ready")
    log.info("runner ready for session %s: %s", rec.session_id, ready.decode("utf-8", errors="replace").strip())


async def _read_runner_turn(rec: ChatSession, prompt: str, stream_tokens: int):
    proc = rec.runner_proc
    if proc is None or proc.stdout is None or proc.stdin is None:
        raise HTTPException(500, "runner process unavailable")

    delimiter = "[[[TURN_END]]]\n"
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    pending = ""
    keep = max(1, len(delimiter))

    prompt_bytes = prompt.encode("utf-8")
    header = f"TURN {stream_tokens} {len(prompt_bytes)}\n".encode("utf-8")
    proc.stdin.write(header)
    proc.stdin.write(prompt_bytes)
    await proc.stdin.drain()

    while True:
        chunk = await proc.stdout.read(256)
        if not chunk:
            break
        pending += decoder.decode(chunk)
        while True:
            idx = pending.find(delimiter)
            if idx >= 0:
                if idx > 0:
                    yield pending[:idx]
                return
            if len(pending) > keep:
                emit = pending[:-keep]
                if emit:
                    yield emit
                pending = pending[-keep:]
            break

    pending += decoder.decode(b"", final=True)
    if pending:
        idx = pending.find(delimiter)
        if idx >= 0:
            if idx > 0:
                yield pending[:idx]
        else:
            yield pending


CHAT_PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>POGLS Browser Chat</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #0b1020;
      --panel: rgba(14, 20, 38, 0.82);
      --border: rgba(255, 255, 255, 0.10);
      --text: #eef2ff;
      --muted: #9aa7c7;
      --accent: #7dd3fc;
      --danger: #fb7185;
      --user: #19314f;
      --assistant: #171f31;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      color: var(--text);
      background:
        radial-gradient(circle at top left, rgba(125, 211, 252, 0.16), transparent 32%),
        radial-gradient(circle at top right, rgba(167, 139, 250, 0.16), transparent 28%),
        linear-gradient(180deg, #08101f, #0b1020 28%, #090d17 100%);
      font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    .shell {
      max-width: 1080px;
      margin: 0 auto;
      padding: 24px;
      display: grid;
      gap: 16px;
    }
    .hero, .panel {
      border: 1px solid var(--border);
      background: var(--panel);
      backdrop-filter: blur(14px);
      border-radius: 18px;
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.22);
    }
    .hero {
      padding: 20px 22px;
      display: grid;
      gap: 8px;
    }
    h1 {
      margin: 0;
      font-size: 1.45rem;
      letter-spacing: 0.01em;
    }
    .sub {
      color: var(--muted);
      line-height: 1.5;
      font-size: 0.96rem;
    }
    .controls {
      display: grid;
      grid-template-columns: 1fr 150px auto auto;
      gap: 10px;
      align-items: center;
      margin-top: 8px;
    }
    input, textarea, button {
      font: inherit;
      color: var(--text);
      border: 1px solid var(--border);
      background: rgba(10, 15, 26, 0.72);
      border-radius: 12px;
      outline: none;
    }
    input, textarea {
      padding: 12px 14px;
    }
    textarea {
      resize: vertical;
      min-height: 84px;
      width: 100%;
    }
    button {
      padding: 12px 16px;
      cursor: pointer;
      transition: transform 0.08s ease, border-color 0.15s ease;
    }
    button:hover { border-color: rgba(125, 211, 252, 0.45); }
    button:active { transform: translateY(1px); }
    .chip {
      display: inline-flex;
      gap: 8px;
      align-items: center;
      padding: 8px 12px;
      border-radius: 999px;
      background: rgba(125, 211, 252, 0.10);
      color: #d9f3ff;
      border: 1px solid rgba(125, 211, 252, 0.16);
      font-size: 0.9rem;
      width: fit-content;
    }
    .row {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      align-items: center;
    }
    .panel {
      padding: 14px;
    }
    .log {
      display: grid;
      gap: 12px;
      min-height: 52vh;
      max-height: 66vh;
      overflow: auto;
      padding-right: 4px;
    }
    .msg {
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 14px 16px;
      white-space: pre-wrap;
      line-height: 1.55;
      animation: fadein 0.14s ease-out;
    }
    .msg.user { background: var(--user); margin-left: 8%; }
    .msg.assistant { background: var(--assistant); margin-right: 8%; }
    .meta {
      display: flex;
      justify-content: space-between;
      align-items: center;
      color: var(--muted);
      font-size: 0.86rem;
      margin-bottom: 8px;
    }
    .composer {
      display: grid;
      gap: 10px;
    }
    .status {
      color: var(--muted);
      font-size: 0.92rem;
    }
    .danger { color: var(--danger); }
    .footer {
      color: var(--muted);
      font-size: 0.84rem;
      padding: 0 4px 16px;
    }
    @keyframes fadein {
      from { opacity: 0; transform: translateY(3px); }
      to { opacity: 1; transform: translateY(0); }
    }
    @media (max-width: 760px) {
      .controls { grid-template-columns: 1fr; }
      .msg.user, .msg.assistant { margin-left: 0; margin-right: 0; }
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <div class="chip">POGLS Browser Chat</div>
      <h1>Local model chat in the browser</h1>
      <div class="sub">
        Streams from the local runner, keeps per-session history, and pulls relevant repo memory on each turn.
      </div>
        <div class="controls">
        <select id="modelPath"></select>
        <input id="tokenCount" type="number" min="1" max="512" value="160" />
        <button id="resetBtn" type="button">Reset chat</button>
        <button id="clearBtn" type="button">Clear view</button>
      </div>
      <div class="row">
        <label class="chip"><input id="useMemory" type="checkbox" checked /> Use memory</label>
        <button id="exportBtn" type="button">Export transcript</button>
        <span id="sessionInfo" class="status"></span>
        <span id="netInfo" class="status"></span>
      </div>
    </section>

    <section class="panel">
      <div id="log" class="log"></div>
    </section>

    <section class="panel composer">
      <textarea id="input" placeholder="Type a message. Enter sends, Shift+Enter makes a new line."></textarea>
      <div class="row">
        <button id="sendBtn" type="button">Send</button>
        <span id="status" class="status">Ready.</span>
      </div>
    </section>

    <div class="footer">
      Endpoint: <code>/chat/api/turn</code> · Session history stays in memory on the server.
    </div>
  </div>

  <script>
    const log = document.getElementById('log');
    const input = document.getElementById('input');
    const sendBtn = document.getElementById('sendBtn');
    const resetBtn = document.getElementById('resetBtn');
    const clearBtn = document.getElementById('clearBtn');
    const exportBtn = document.getElementById('exportBtn');
    const status = document.getElementById('status');
    const netInfo = document.getElementById('netInfo');
    const sessionInfo = document.getElementById('sessionInfo');
    const modelPath = document.getElementById('modelPath');
    const tokenCount = document.getElementById('tokenCount');
    const useMemory = document.getElementById('useMemory');
    const sessionKey = 'pogls_chat_session_id';
    const memoryKey = 'pogls_chat_use_memory';
    const modelKey = 'pogls_chat_model_path';
    let sessionId = localStorage.getItem(sessionKey);
    if (!sessionId) {
      sessionId = crypto.randomUUID();
      localStorage.setItem(sessionKey, sessionId);
    }
    const rememberedMemory = localStorage.getItem(memoryKey);
    if (rememberedMemory !== null) {
      useMemory.checked = rememberedMemory === '1';
    }
    sessionInfo.textContent = `session ${sessionId}`;

    function setStatus(text, isError = false) {
      status.textContent = text;
      status.classList.toggle('danger', !!isError);
    }

    function appendMessage(role, text) {
      const wrap = document.createElement('div');
      wrap.className = `msg ${role}`;
      const meta = document.createElement('div');
      meta.className = 'meta';
      meta.textContent = role === 'user' ? 'You' : 'Assistant';
      const body = document.createElement('div');
      body.className = 'body';
      body.textContent = text || '';
      wrap.appendChild(meta);
      wrap.appendChild(body);
      log.appendChild(wrap);
      log.scrollTop = log.scrollHeight;
      return body;
    }

    async function loadHistory() {
      try {
        const res = await fetch(`/chat/api/history/${sessionId}`);
        if (!res.ok) return;
        const data = await res.json();
        if (typeof data.use_memory === 'boolean') {
          useMemory.checked = data.use_memory;
        }
        if (data.model_path) {
          modelPath.value = data.model_path;
        }
        log.innerHTML = '';
        for (const turn of data.turns || []) {
          appendMessage('user', turn.user || '');
          appendMessage('assistant', turn.assistant || '');
        }
      } catch (err) {
        netInfo.textContent = `history load failed: ${err.message}`;
      }
    }

    async function loadModels() {
      try {
        const res = await fetch('/chat/api/models');
        if (!res.ok) return;
        const data = await res.json();
        const models = data.models || [];
        modelPath.innerHTML = '';
        for (const model of models) {
          const opt = document.createElement('option');
          opt.value = model;
          opt.textContent = model;
          modelPath.appendChild(opt);
        }
        const saved = localStorage.getItem(modelKey);
        const selected = saved && models.includes(saved) ? saved : data.default_model;
        if (selected && models.includes(selected)) {
          modelPath.value = selected;
        }
      } catch (err) {
        netInfo.textContent = `model load failed: ${err.message}`;
      }
    }

    async function resetChat() {
      await fetch('/chat/api/reset', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ session_id: sessionId })
      });
      log.innerHTML = '';
      setStatus('Chat reset.');
    }

    async function syncSession() {
      await fetch('/chat/api/session', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          session_id: sessionId,
          model_path: modelPath.value.trim(),
          use_memory: useMemory.checked,
        })
      });
    }

    async function exportTranscript() {
      const res = await fetch(`/chat/api/export/${sessionId}`);
      if (!res.ok) {
        throw new Error(await res.text());
      }
      const blob = await res.blob();
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `pogls-chat-${sessionId}.json`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      URL.revokeObjectURL(url);
      setStatus('Transcript exported.');
    }

    async function sendMessage() {
      const message = input.value.trim();
      if (!message) return;
      input.value = '';
      appendMessage('user', message);
      const assistantBody = appendMessage('assistant', '');
      assistantBody.textContent = 'Thinking...';
      setStatus('Streaming response...');
      sendBtn.disabled = true;
      input.disabled = true;
      try {
        localStorage.setItem(memoryKey, useMemory.checked ? '1' : '0');
        localStorage.setItem(modelKey, modelPath.value.trim());
        await syncSession();
        const res = await fetch('/chat/api/turn', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            session_id: sessionId,
            message,
            model_path: modelPath.value.trim(),
            use_memory: useMemory.checked,
            stream_tokens: Number(tokenCount.value) || 160
          })
        });
        if (!res.ok) {
          const errText = await res.text();
          throw new Error(errText || `HTTP ${res.status}`);
        }
        const reader = res.body.getReader();
        const decoder = new TextDecoder();
        let text = '';
        assistantBody.textContent = '';
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          text += decoder.decode(value, { stream: true });
          assistantBody.textContent = text;
          log.scrollTop = log.scrollHeight;
        }
        assistantBody.textContent = text.trim() || '(empty response)';
        setStatus('Ready.');
      } catch (err) {
        assistantBody.textContent = `Error: ${err.message}`;
        setStatus(err.message, true);
      } finally {
        sendBtn.disabled = false;
        input.disabled = false;
        input.focus();
      }
    }

    sendBtn.addEventListener('click', sendMessage);
    resetBtn.addEventListener('click', resetChat);
    clearBtn.addEventListener('click', () => { log.innerHTML = ''; setStatus('View cleared.'); });
    exportBtn.addEventListener('click', () => exportTranscript().catch((err) => setStatus(err.message, true)));
    modelPath.addEventListener('change', () => {
      localStorage.setItem(modelKey, modelPath.value.trim());
      syncSession().catch((err) => setStatus(err.message, true));
    });
    useMemory.addEventListener('change', () => {
      localStorage.setItem(memoryKey, useMemory.checked ? '1' : '0');
      syncSession().catch((err) => setStatus(err.message, true));
    });
    input.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter' && !ev.shiftKey) {
        ev.preventDefault();
        sendMessage();
      }
    });

    loadModels().catch(() => {});
    loadHistory().catch(() => {});
    netInfo.textContent = 'local backend ready';
    input.focus();
  </script>
</body>
</html>
"""


app = FastAPI(
    title="POGLS Browser Chat",
    version="1.0.0",
    description="Local browser chat over the existing POGLS runner",
)


@app.get("/health")
def health():
    return {
        "status": "ok",
        "runner": str(_resolve_runner_path()),
        "model": DEFAULT_MODEL,
        "sessions": len(sessions),
    }


@app.get("/")
@app.get("/chat")
def chat_page():
    return HTMLResponse(CHAT_PAGE.replace("__DEFAULT_MODEL__", html.escape(DEFAULT_MODEL)))


@app.get("/chat/api/history/{session_id}")
def chat_history(session_id: str):
    rec = sessions.get(session_id)
    if rec is None:
        return JSONResponse({"session_id": session_id, "turns": [], "use_memory": True})
    return JSONResponse(
        {
            "session_id": rec.session_id,
            "model_path": rec.model_path,
            "use_memory": rec.use_memory,
            "created_at": rec.created_at,
            "turns": rec.turns,
        }
    )


@app.get("/chat/api/models")
def chat_models():
    return JSONResponse(
        {
            "default_model": DEFAULT_MODEL,
            "models": _discover_models(),
        }
    )


@app.post("/chat/api/reset")
async def chat_reset(body: ResetBody):
    rec = sessions.get(body.session_id)
    if rec is not None:
        await _stop_runner_process(rec)
    sessions.pop(body.session_id, None)
    return JSONResponse({"ok": True, "session_id": body.session_id})


@app.post("/chat/api/session")
async def chat_session_update(body: SessionUpdateBody):
    rec = _get_session(body.session_id, body.model_path)
    restart = False
    if body.model_path and body.model_path.strip():
        new_model = body.model_path.strip()
        if new_model != rec.model_path:
            rec.model_path = new_model
            restart = True
    if body.use_memory is not None:
        rec.use_memory = bool(body.use_memory)
    if restart:
        await _stop_runner_process(rec)
    return JSONResponse(
        {
            "ok": True,
            "session_id": rec.session_id,
            "model_path": rec.model_path,
            "use_memory": rec.use_memory,
        }
    )


@app.get("/chat/api/export/{session_id}")
def chat_export(session_id: str):
    rec = sessions.get(session_id)
    if rec is None:
        raise HTTPException(404, "session not found")

    payload = {
        "session_id": rec.session_id,
        "model_path": rec.model_path,
        "use_memory": rec.use_memory,
        "created_at": rec.created_at,
        "turns": rec.turns,
    }
    return JSONResponse(payload, media_type="application/json")


@app.post("/chat/api/turn")
async def chat_turn(body: ChatTurnBody):
    message = body.message.strip()
    if not message:
        raise HTTPException(400, "message is empty")

    rec = _get_session(body.session_id, body.model_path)
    stream_tokens = max(1, min(int(body.stream_tokens or DEFAULT_TOKENS), 512))

    rec.use_memory = bool(body.use_memory)
    memory_context = _run_memory_search(message) if rec.use_memory else ""
    prompt = _build_prompt(rec.turns, message, memory_context)
    lock = _ensure_lock(rec)

    async def streamer():
        assistant_parts: list[str] = []
        async with lock:
            await _ensure_runner_process(rec)
            try:
                async for chunk in _read_runner_turn(rec, prompt, stream_tokens):
                    assistant_parts.append(chunk)
                    yield chunk.encode("utf-8", errors="replace")
            finally:
                assistant = "".join(assistant_parts).strip()
                if assistant:
                    rec.append_turn(message, assistant)

    return StreamingResponse(streamer(), media_type="text/plain; charset=utf-8")


def main() -> int:
    import uvicorn

    uvicorn.run(
        "browser_chat_server:app",
        host=DEFAULT_HOST,
        port=DEFAULT_PORT,
        reload=False,
        log_level="info",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
