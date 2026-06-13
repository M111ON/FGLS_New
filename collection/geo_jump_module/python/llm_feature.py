from __future__ import annotations
import sys, logging, threading, time
from pathlib import Path
from typing import Optional
from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
try:
    from .base import EngineFeature
except ImportError:
    class EngineFeature:
        name: str = ""
        description: str = ""
        icon: str = ""
        version: str = "0.1"
        enabled: bool = True
        error: str = ""

        def register_routes(self, app): pass

        def status(self) -> dict:
            return {"name": self.name, "enabled": self.enabled,
                    "description": self.description, "error": self.error}

logger = logging.getLogger("engine.features.llm")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
for p in [str(_COLLECTION), str(_HERE.parent)]:
    if p not in sys.path:
        sys.path.insert(0, p)

import os as _os

_MODEL_DIR_DEFAULT_CANDIDATES = [
    Path(_os.environ.get("GEO_MODEL_DIR", "")),   # env override (highest priority)
    Path("I:/model"),                               # original Windows path
    Path.home() / "models",                        # Linux/Mac home
    Path("/opt/models"),                            # common server path
    _HERE.parent / "models",                       # relative to project
]
_MODEL_DIR = next(
    (p for p in _MODEL_DIR_DEFAULT_CANDIDATES if p != Path("") and p.exists()),
    Path(_os.environ.get("GEO_MODEL_DIR", "I:/model"))  # keep original as last resort
)
_MODEL_CANDIDATES = [
    "SmolLM2-360M-Instruct.Q8_0.gguf",
    "Qwen2.5-0.5B-Instruct-Q8_0.gguf",
    "qwen2.5-coder-1.5b-instruct-q8_0.gguf",
]

_LLM_INSTANCE = None
_LLM_LOCK = threading.Lock()
_LLM_MODEL_PATH = None


def _find_model() -> Optional[str]:
    for name in _MODEL_CANDIDATES:
        p = _MODEL_DIR / name
        if p.exists():
            return str(p.resolve())
    return None


def _load_llm(n_gpu_layers: int = 0, model_path: Optional[str] = None):
    global _LLM_INSTANCE, _LLM_MODEL_PATH
    if _LLM_INSTANCE is not None and model_path is None:
        return _LLM_INSTANCE
    if model_path is None:
        model_path = _find_model()
    if not model_path:
        return None
    try:
        from llama_cpp import Llama
        logger.info(f"Loading model: {model_path} (ngl={n_gpu_layers})")
        t0 = time.perf_counter()
        _LLM_INSTANCE = Llama(
            model_path=model_path,
            n_gpu_layers=n_gpu_layers,
            n_ctx=2048,
            n_threads=4,
            verbose=False,
        )
        _LLM_MODEL_PATH = model_path
        ms = (time.perf_counter() - t0) * 1000
        logger.info(f"Model loaded in {ms:.0f}ms")
    except Exception as e:
        logger.warning(f"Model load failed: {e}")
    return _LLM_INSTANCE


def _get_llm():
    return _LLM_INSTANCE


class ChatRequest(BaseModel):
    messages: list[dict]
    max_tokens: int = 256
    temperature: float = 0.7
    top_p: float = 0.9
    stream: bool = False


class GenerateRequest(BaseModel):
    prompt: str
    max_tokens: int = 256
    temperature: float = 0.7
    top_p: float = 0.9
    stop: list[str] = []


class LoadRequest(BaseModel):
    model_idx: int = 0
    n_gpu_layers: int = 0


_HTML_CHAT = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>LLM Chat</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{font-family:'Segoe UI',system-ui,sans-serif;background:#111;color:#e0e0e0;height:100vh;display:flex;flex-direction:column}
  #header{padding:12px 20px;background:#1a1a2e;border-bottom:1px solid #333;display:flex;align-items:center;gap:12px}
  #header h1{font-size:16px;font-weight:600;color:#7c3aed}
  #header .info{font-size:12px;color:#666;margin-left:auto}
  select#model{background:#222;color:#ccc;border:1px solid #444;border-radius:4px;padding:2px 6px;font-size:12px}
  #chat{flex:1;overflow-y:auto;padding:16px 20px;display:flex;flex-direction:column;gap:12px}
  .msg{max-width:80%;padding:10px 14px;border-radius:12px;line-height:1.5;font-size:14px;white-space:pre-wrap}
  .user{background:#1e3a5f;align-self:flex-end;border-bottom-right-radius:4px}
  .assistant{background:#1e1e2e;align-self:flex-start;border-bottom-left-radius:4px}
  .msg.loading{opacity:.6}
  #input_row{padding:12px 20px;background:#1a1a2e;border-top:1px solid #333;display:flex;gap:8px}
  #input{flex:1;background:#222;border:1px solid #444;border-radius:8px;padding:10px 14px;color:#e0e0e0;font-size:14px;outline:none;resize:none;font-family:inherit}
  #input:focus{border-color:#7c3aed}
  #send{background:#7c3aed;color:#fff;border:none;border-radius:8px;padding:10px 20px;font-size:14px;cursor:pointer;font-weight:600}
  #send:hover{background:#6d28d9}
  #send:disabled{opacity:.5;cursor:not-allowed}
  #status{font-size:11px;color:#555;padding:4px 20px;text-align:center}
  .typing::after{content:'...';animation:dots 1.5s steps(4,end) infinite}
  @keyframes dots{0%,20%{content:''}40%{content:'.'}60%{content:'..'}80%,100%{content:'...'}}
</style>
</head>
<body>
<div id=header>
  <h1>&#9670; LLM Chat</h1>
  <select id=model>
    <option value=0>SmolLM2-360M</option>
    <option value=1>Qwen2.5-0.5B</option>
    <option value=2>Qwen2.5-Coder-1.5B</option>
  </select>
  <span class=info id=statusText>idle</span>
</div>
<div id=chat></div>
<div id=status></div>
<div id=input_row>
  <textarea id=input rows=1 placeholder="Type a message..." style="height:42px"></textarea>
  <button id=send>Send</button>
</div>
<script>
const models=%MODELS%;
let currentModel=0;

document.getElementById('model').onchange=function(){currentModel=parseInt(this.value)};

function addMsg(role,text,loading){
  const d=document.getElementById('chat');
  const m=document.createElement('div');
  m.className='msg '+role;
  if(loading)m.classList.add('loading');
  m.textContent=text||(role==='assistant'?'thinking...':'');
  d.appendChild(m);
  d.scrollTop=d.scrollHeight;
  return m;
}

document.getElementById('send').onclick=sendMsg;
document.getElementById('input').onkeydown=function(e){
  if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendMsg()}
};

document.getElementById('model').onchange=async function(){
  const idx=parseInt(this.value);
  const btn=document.getElementById('send');
  btn.disabled=true;
  document.getElementById('statusText').textContent='loading...';
  try{
    const r=await fetch('/api/llm/load',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({model_idx:idx,n_gpu_layers:0})
    });
    const j=await r.json();
    if(j.status==='ok')document.getElementById('statusText').textContent=models[idx];
    else document.getElementById('statusText').textContent='load failed';
  }catch(e){
    document.getElementById('statusText').textContent='error: '+e.message;
  }
  btn.disabled=false;
};

async function sendMsg(){
  const inp=document.getElementById('input');
  const txt=inp.value.trim();
  if(!txt)return;
  inp.value='';
  inp.style.height='42px';
  document.getElementById('send').disabled=true;
  document.getElementById('statusText').textContent='generating...';

  addMsg('user',txt);
  const resp=addMsg('assistant','',true);

  try{
    const r=await fetch('/api/llm/v1/chat/completions',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({
        model:models[currentModel],
        messages:[{role:'user',content:txt}],
        max_tokens:256,
        temperature:0.7,
        stream:false
      })
    });
    const j=await r.json();
    const reply=j.choices?.[0]?.message?.content||'(empty)';
    const mdl=j.model||models[currentModel];
    const mdlShort=mdl.split('/').pop().replace('.gguf','');
    resp.textContent=reply+'\n\n— '+mdlShort;
    resp.classList.remove('loading');
    document.getElementById('statusText').textContent='idle';
  }catch(e){
    resp.textContent='Error: '+e.message;
    resp.classList.remove('loading');
    document.getElementById('statusText').textContent='error';
  }
  document.getElementById('send').disabled=false;
}
</script>
</body>
</html>"""


class LlmFeature(EngineFeature):
    name = "LLM Inference"
    description = "Chat inference backend via llama.cpp"
    icon = "&#9670;"
    version = "0.1"

    def __init__(self):
        self._model_path = None
        self._n_gpu_layers = 0
        _load_llm(self._n_gpu_layers)

    def register_routes(self, app):
        router = APIRouter(prefix="/api/llm", tags=["llm"])

        @router.get("/status")
        def llm_status():
            llm = _get_llm()
            return {
                "loaded": llm is not None,
                "model": str(_LLM_MODEL_PATH or ""),
                "n_gpu_layers": self._n_gpu_layers,
                "models": _MODEL_CANDIDATES,
            }

        @router.post("/load")
        def llm_load(req: LoadRequest):
            global _LLM_INSTANCE, _LLM_MODEL_PATH
            if req.model_idx < 0 or req.model_idx >= len(_MODEL_CANDIDATES):
                raise HTTPException(400, "invalid model index")
            mp = _MODEL_DIR / _MODEL_CANDIDATES[req.model_idx]
            if not mp.exists():
                raise HTTPException(404, f"model not found: {mp}")
            with _LLM_LOCK:
                if _LLM_INSTANCE:
                    del _LLM_INSTANCE
                _LLM_INSTANCE = None
                _LLM_MODEL_PATH = None
            _load_llm(req.n_gpu_layers, str(mp))
            if not _get_llm():
                raise HTTPException(500, "model load failed")
            return {"status": "ok", "model": str(mp)}

        @router.post("/v1/chat/completions")
        async def chat_completions(req: ChatRequest):
            llm = _get_llm()
            if not llm:
                raise HTTPException(503, "model not loaded")
            try:
                resp = llm.create_chat_completion(
                    messages=req.messages,
                    max_tokens=req.max_tokens,
                    temperature=req.temperature,
                    top_p=req.top_p,
                    stream=False,
                )
                return resp
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/generate")
        def generate(req: GenerateRequest):
            llm = _get_llm()
            if not llm:
                raise HTTPException(503, "model not loaded")
            try:
                resp = llm(
                    req.prompt,
                    max_tokens=req.max_tokens,
                    temperature=req.temperature,
                    top_p=req.top_p,
                    stop=req.stop or None,
                )
                return resp
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.get("/ui", response_class=HTMLResponse)
        def chat_ui():
            models_json = str(_MODEL_CANDIDATES).replace("'", '"')
            html = _HTML_CHAT.replace("%MODELS%", models_json)
            return HTMLResponse(html)

        app.include_router(router)
