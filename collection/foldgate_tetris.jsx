import { useState, useEffect, useRef, useCallback } from "react";

// ═══════════════════════════════════════════════════════════════
// LAYER 0 — CONSTANTS & MAPPINGS
// ═══════════════════════════════════════════════════════════════

const SHAPES = {
  I: { label:"I", role:"pipe",       color:"#00ff9f", desc:"linear passthrough", axes:[1] },
  O: { label:"O", role:"latch",      color:"#00cfff", desc:"buffer / hold",      axes:[2] },
  T: { label:"T", role:"splitter",   color:"#a78bfa", desc:"broadcast fan-out",  axes:[3] },
  S: { label:"S", role:"transpose",  color:"#fbbf24", desc:"cross-swap",         axes:[4] },
  Z: { label:"Z", role:"invert",     color:"#f87171", desc:"reverse-swap",       axes:[5] },
  L: { label:"L", role:"fork-L",     color:"#fb923c", desc:"left branch",        axes:[6] },
  J: { label:"J", role:"fork-R",     color:"#34d399", desc:"right branch",       axes:[7] },
};

// fold_axis → piece shape (FoldGate ↔ TetrisRouter mapping)
const AXIS_TO_SHAPE = { 1:"I", 2:"O", 3:"T", 4:"S", 5:"Z", 6:"L", 7:"J" };

// Ω fallback nodes → piece shape
const OMEGA = {
  Ω_compress:    { shape:"I", color:"#00ff9f", desc:"summarize → repack → continue" },
  Ω_retry:       { shape:"O", color:"#00cfff", desc:"hold + respawn same geo_key"   },
  Ω_quarantine:  { shape:"L", color:"#fb923c", desc:"fork to dead-letter lane"      },
  Ω_wait:        { shape:"T", color:"#a78bfa", desc:"fan-in, hold until upstream"   },
};

// Agent slots — each IS a piece instance
const AGENT_DEFS = [
  { id:"A", role:"Reader",   fold_axis:1, token_cap:300, task:"parse_data"      },
  { id:"B", role:"Analyst",  fold_axis:3, token_cap:400, task:"validate_schema" },
  { id:"C", role:"Writer",   fold_axis:1, token_cap:300, task:"write_output"    },
  { id:"D", role:"Auditor",  fold_axis:6, token_cap:200, task:"audit_log"       },
];

const PROJECT_TASKS = {
  parse_data:      "Extract: Fibonacci closure=144 cycles, Dodeca face=3, spoke=2, slot=17",
  validate_schema: "Validate POGLS fields (face,spoke,slot,cycles) are non-zero integers",
  write_output:    "Write 2-sentence executive summary of geometric analysis",
  audit_log:       "Generate audit: timestamp, task_count=4, integrity=pass",
};

// ═══════════════════════════════════════════════════════════════
// LAYER 1 — GEOMETRY ENGINE (pure functions, stateless)
// ═══════════════════════════════════════════════════════════════

// fibo sequence for address generation
const FIBO = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987];

function fiboAddr(seed) {
  let h = seed;
  for (let i = 0; i < 8; i++) h = ((h ^ FIBO[i % 16]) * 0x9e3779b9) >>> 0;
  return h.toString(16).padStart(8,"0");
}

// wallet topology fingerprint → geo_key (8B hex)
function walletToGeoKey(topologyFp) {
  const bytes = [...topologyFp].reduce((a,c) => a ^ c.charCodeAt(0), 0);
  return fiboAddr(bytes ^ 0xdeadbeef);
}

// intrinsic bond: XOR of two coordinate addresses
function intrinsicBond(addrA, addrB) {
  const a = parseInt(addrA.slice(0,8), 16);
  const b = parseInt(addrB.slice(0,8), 16);
  return (a ^ b).toString(16).padStart(8,"0");
}

// generate coord wallet for an agent piece
function mkWallet(agentId, task, sessionId) {
  const seed = [...`${agentId}:${task}:${sessionId}`]
    .reduce((a,c,i) => a ^ (c.charCodeAt(0) * FIBO[i % 16]), 0);
  const fp = fiboAddr(seed);
  const geoKey = walletToGeoKey(fp);
  const shape  = AXIS_TO_SHAPE[AGENT_DEFS.find(a=>a.id===agentId).fold_axis];
  return { agent_id:agentId, task, topology_fp:fp, geo_key:geoKey, shape, session:sessionId };
}

// piece instance from wallet
function mkPiece(wallet, agentDef) {
  const addrL = fiboAddr(parseInt(wallet.geo_key, 16) ^ 0xaaaa);
  const addrR = fiboAddr(parseInt(wallet.geo_key, 16) ^ 0x5555);
  return {
    id:          `${agentDef.id}_${wallet.session}`,
    agent_id:    agentDef.id,
    shape:       wallet.shape,
    geo_key:     wallet.geo_key,
    fold_axis:   agentDef.fold_axis,
    token_cap:   agentDef.token_cap,
    bond_L:      addrL,
    bond_R:      addrR,
    bond_key:    intrinsicBond(addrL, addrR),
    plugs:       { N:null, S:null, E:null, W:null },
    task:        agentDef.task,
  };
}

// connect two pieces via extrinsic plug
function plugConnect(pieceA, faceA, pieceB, faceB) {
  const plugId = `plug_${pieceA.id}_${faceA}_${pieceB.id}_${faceB}`;
  return {
    ...pieceA,
    plugs: { ...pieceA.plugs, [faceA]: { target: pieceB.id, face: faceB, plug_id: plugId } }
  };
}

// reroute: fault → Ω shape substitution
function rerouteToOmega(piece, faultType) {
  const omega = OMEGA[faultType] || OMEGA["Ω_quarantine"];
  return {
    ...piece,
    shape:    omega.shape,
    rerouted: faultType,
    geo_key:  fiboAddr(parseInt(piece.geo_key,16) ^ 0xf0f0f0f0),
  };
}

// ═══════════════════════════════════════════════════════════════
// LAYER 2 — CLAUDE API (agent execution)
// ═══════════════════════════════════════════════════════════════

async function executePiece(piece, wallet, onToken) {
  const taskData = PROJECT_TASKS[piece.task];
  const sysprompt = `You are Agent-${piece.agent_id} [${SHAPES[piece.shape]?.role}] shape=${piece.shape} geo=${piece.geo_key} axis=${piece.fold_axis}.
Token budget: ${piece.token_cap}. You see ONLY your assigned task slice.
Respond ONLY in JSON (no markdown): {"agent":"${piece.agent_id}","shape":"${piece.shape}","geo_key":"${piece.geo_key}","output":"<result>","tokens_used":<n>,"status":"done"}`;

  const res = await fetch("https://api.anthropic.com/v1/messages", {
    method:"POST",
    headers:{"Content-Type":"application/json"},
    body: JSON.stringify({
      model:"claude-sonnet-4-20250514",
      max_tokens: piece.token_cap,
      system: sysprompt,
      messages:[{ role:"user", content:`TASK [wallet:${wallet.topology_fp}]: ${taskData}` }],
      stream: true,
    }),
  });

  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let full = "";
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    for (const line of dec.decode(value).split("\n").filter(l=>l.startsWith("data:"))) {
      try {
        const d = JSON.parse(line.slice(5));
        if (d.type === "content_block_delta") { full += d.delta.text||""; onToken(d.delta.text||""); }
      } catch {}
    }
  }
  try   { return JSON.parse(full.replace(/```json|```/g,"").trim()); }
  catch { return { agent:piece.agent_id, shape:piece.shape, geo_key:piece.geo_key, output:full, tokens_used:"?", status:"done" }; }
}

// ═══════════════════════════════════════════════════════════════
// UI COMPONENTS
// ═══════════════════════════════════════════════════════════════

// Tetromino SVG shapes
function TetrominoIcon({ shape, color, size=28, pulse=false }) {
  const s = size/4;
  const cells = {
    I: [[0,1],[1,1],[2,1],[3,1]],
    O: [[0,0],[1,0],[0,1],[1,1]],
    T: [[0,1],[1,1],[2,1],[1,0]],
    S: [[1,0],[2,0],[0,1],[1,1]],
    Z: [[0,0],[1,0],[1,1],[2,1]],
    L: [[0,0],[0,1],[0,2],[1,2]],
    J: [[1,0],[1,1],[1,2],[0,2]],
  }[shape] || [[0,0]];
  return (
    <svg width={size} height={size} style={{ flexShrink:0 }}>
      {cells.map(([cx,cy],i) => (
        <rect key={i} x={cx*s+1} y={cy*s+1} width={s-2} height={s-2}
          fill={color} opacity={pulse ? undefined : 0.85} rx={1}
          style={pulse ? { animation:`cellpulse 1.2s ease-in-out ${i*0.1}s infinite` } : {}}
        />
      ))}
    </svg>
  );
}

// Bond wire visual
function BondWire({ keyStr, color }) {
  return (
    <div style={{ display:"flex", alignItems:"center", gap:6, marginTop:4 }}>
      <div style={{ width:3, height:3, borderRadius:"50%", background:color, opacity:.5 }}/>
      <div style={{ flex:1, height:1, background:`${color}33` }}/>
      <span style={{ fontSize:8, fontFamily:"monospace", color:"#444", letterSpacing:1 }}>
        ⊕{keyStr.slice(0,8)}
      </span>
      <div style={{ flex:1, height:1, background:`${color}33` }}/>
      <div style={{ width:3, height:3, borderRadius:"50%", background:color, opacity:.5 }}/>
    </div>
  );
}

// Single piece card
function PieceCard({ agentDef, piece, wallet, state, stream, result }) {
  const shape = piece?.shape || AXIS_TO_SHAPE[agentDef.fold_axis];
  const shapeInfo = SHAPES[shape] || SHAPES["I"];
  const c = shapeInfo.color;
  const isActive = state === "working";
  const isDone   = state === "done";
  const isRerouted = piece?.rerouted;

  return (
    <div style={{
      background: "#07070e",
      border: `1px solid ${isActive||isDone ? c : "#111"}`,
      borderRadius:6, padding:"12px 14px",
      position:"relative", overflow:"hidden",
      boxShadow: isActive ? `0 0 24px ${c}22` : isDone ? `0 0 8px ${c}11` : "none",
      transition:"all 0.4s",
    }}>
      {isActive && <div style={{
        position:"absolute", inset:0,
        background:`radial-gradient(ellipse at 50% 0%, ${c}0d, transparent 60%)`,
        animation:"shimmer 2s ease-in-out infinite",
      }}/>}

      {/* header row */}
      <div style={{ display:"flex", justifyContent:"space-between", alignItems:"flex-start", marginBottom:8 }}>
        <div style={{ display:"flex", gap:8, alignItems:"center" }}>
          <TetrominoIcon shape={shape} color={c} size={24} pulse={isActive} />
          <div>
            <div style={{ fontSize:11, color:"#ddd", fontFamily:"monospace", fontWeight:700, letterSpacing:1 }}>
              AGENT-{agentDef.id}
              {isRerouted && <span style={{ color:"#fb923c", marginLeft:6, fontSize:9 }}>↩{piece.rerouted}</span>}
            </div>
            <div style={{ fontSize:9, color:c, letterSpacing:2 }}>
              {shape}-SHAPE · {shapeInfo.role.toUpperCase()}
            </div>
          </div>
        </div>
        <div style={{ textAlign:"right" }}>
          <div style={{ fontSize:8, color: state==="idle"?"#222": state==="done"?"#0f0":"#555", letterSpacing:2, fontFamily:"monospace" }}>
            {state === "idle" ? "STANDBY" : state === "claimed" ? "CLAIMED" : state === "working" ? "EXEC" : "SEALED"}
          </div>
          <div style={{ fontSize:8, color:"#333", fontFamily:"monospace" }}>axis={agentDef.fold_axis}</div>
        </div>
      </div>

      {/* geo_key + bond */}
      {piece && (
        <div style={{ marginBottom:8 }}>
          <div style={{ fontSize:8, fontFamily:"monospace", color:"#333", lineHeight:1.8 }}>
            <span style={{ color:"#444" }}>geo  </span>{piece.geo_key}
            <span style={{ color:"#222", margin:"0 6px" }}>·</span>
            <span style={{ color:"#444" }}>fp   </span>{wallet?.topology_fp?.slice(0,8)}...
          </div>
          <BondWire keyStr={piece.bond_key} color={c} />
        </div>
      )}

      {/* token bar */}
      <div style={{ display:"flex", gap:6, alignItems:"center", marginBottom:8 }}>
        <div style={{ flex:1, height:2, background:"#0d0d0d", borderRadius:1 }}>
          <div style={{
            height:"100%", borderRadius:1, transition:"width 0.6s",
            background: result && (result.tokens_used/agentDef.token_cap)>.8 ? "#f87171" : c,
            width: result ? `${Math.min(100,(result.tokens_used/agentDef.token_cap)*100)}%`
                         : isActive ? "45%" : "0%",
          }}/>
        </div>
        <span style={{ fontSize:8, color:"#444", fontFamily:"monospace", whiteSpace:"nowrap" }}>
          {result ? `${result.tokens_used}/${agentDef.token_cap}t` : `${agentDef.token_cap}t`}
        </span>
      </div>

      {/* output / stream */}
      <div style={{
        fontSize:9, fontFamily:"monospace", color:"#666",
        minHeight:32, maxHeight:64, overflow:"hidden", lineHeight:1.6,
      }}>
        {stream ? stream.slice(-180)
          : result ? <span style={{ color:"#4a8" }}>✓ {result.output?.slice(0,100)}{result.output?.length>100?"…":""}</span>
          : <span style={{ color:"#1a1a1a" }}>awaiting fold...</span>}
      </div>
    </div>
  );
}

// Ω node display
function OmegaNode({ type, triggered, piece }) {
  const om = OMEGA[type];
  const sh = SHAPES[om.shape];
  return (
    <div style={{
      display:"flex", alignItems:"center", gap:8,
      padding:"6px 10px",
      background: triggered ? "#0f0800" : "#08080f",
      border: `1px solid ${triggered ? om.color : "#111"}`,
      borderRadius:4, opacity: triggered ? 1 : 0.4,
      transition:"all 0.3s",
    }}>
      <TetrominoIcon shape={om.shape} color={om.color} size={18}/>
      <div>
        <div style={{ fontSize:9, color: triggered ? om.color : "#444", fontFamily:"monospace", letterSpacing:1 }}>
          {type}
        </div>
        <div style={{ fontSize:8, color:"#333" }}>{sh.role} · {om.desc}</div>
      </div>
      {triggered && piece && (
        <div style={{ marginLeft:"auto", fontSize:8, fontFamily:"monospace", color:om.color }}>
          ↩ {piece.agent_id}
        </div>
      )}
    </div>
  );
}

// Routing canvas — shows piece connections
function RoutingCanvas({ pieces, connections, activeId }) {
  if (!pieces.length) return (
    <div style={{
      height:80, display:"flex", alignItems:"center", justifyContent:"center",
      fontSize:9, color:"#1a1a1a", fontFamily:"monospace", letterSpacing:2,
    }}>NO PIECES LOADED</div>
  );
  return (
    <div style={{ display:"flex", alignItems:"center", gap:0, overflowX:"auto", padding:"8px 0" }}>
      {pieces.map((p,i) => {
        const sh = SHAPES[p.shape] || SHAPES.I;
        const isActive = p.agent_id === activeId;
        return (
          <div key={p.id} style={{ display:"flex", alignItems:"center" }}>
            <div style={{
              display:"flex", flexDirection:"column", alignItems:"center", gap:3,
              padding:"6px 8px",
              background: isActive ? `${sh.color}11` : "transparent",
              borderRadius:4, border:`1px solid ${isActive ? sh.color : "#111"}`,
              transition:"all 0.3s",
            }}>
              <TetrominoIcon shape={p.shape} color={sh.color} size={20} pulse={isActive}/>
              <span style={{ fontSize:7, fontFamily:"monospace", color: isActive ? sh.color : "#333" }}>
                {p.agent_id}
              </span>
              <span style={{ fontSize:6, fontFamily:"monospace", color:"#222" }}>
                {p.geo_key?.slice(0,4)}
              </span>
            </div>
            {i < pieces.length-1 && (
              <div style={{ display:"flex", alignItems:"center", width:24 }}>
                <div style={{ flex:1, height:1, background: connections[i] ? sh.color+"44" : "#111" }}/>
                {connections[i] && <div style={{ width:4, height:4, borderRadius:"50%", background:sh.color, opacity:.6 }}/>}
                <div style={{ flex:1, height:1, background: connections[i] ? SHAPES[pieces[i+1]?.shape]?.color+"44"||"#111" : "#111" }}/>
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}

// ═══════════════════════════════════════════════════════════════
// MAIN APP
// ═══════════════════════════════════════════════════════════════

export default function FoldGateTetrisRouter() {
  const [phase, setPhase]         = useState("idle");
  const [sessionId, setSessionId] = useState(null);
  const [wallets,  setWallets]    = useState({});
  const [pieces,   setPieces]     = useState({});
  const [states,   setStates]     = useState(Object.fromEntries(AGENT_DEFS.map(a=>[a.id,"idle"])));
  const [streams,  setStreams]     = useState({});
  const [results,  setResults]    = useState({});
  const [reroutes, setReroutes]   = useState({});
  const [log,      setLog]        = useState([]);
  const [activeId, setActiveId]   = useState(null);
  const logRef = useRef(null);

  useEffect(()=>{ if(logRef.current) logRef.current.scrollTop=logRef.current.scrollHeight; },[log]);

  const emit = (msg, color="#555") => setLog(l=>[...l,{msg,color,t:Date.now()}]);
  const setState = (id,s) => setStates(p=>({...p,[id]:s}));

  const summon = async () => {
    if (phase!=="idle" && phase!=="closed") return;

    // reset
    setWallets({}); setPieces({}); setStreams({}); setResults({}); setReroutes({}); setLog([]);
    setStates(Object.fromEntries(AGENT_DEFS.map(a=>[a.id,"idle"])));

    const sid = Math.random().toString(36).slice(2,10);
    setSessionId(sid);

    // ── OPEN GATE ──
    setPhase("open");
    emit(`▶ FoldGate opened — session ${sid}`, "#00ff9f");
    emit(`  fold_axis → shape mapping active`, "#444");

    // ── CLAIM: wallet + piece for each agent ──
    const newWallets = {}, newPieces = {};
    for (const a of AGENT_DEFS) {
      const w = mkWallet(a.id, a.task, sid);
      const p = mkPiece(w, a);
      newWallets[a.id] = w;
      newPieces[a.id]  = p;
      setState(a.id, "claimed");
      emit(`  ◈ Agent-${a.id} → ${p.shape}-shape [axis=${a.fold_axis}] geo=${p.geo_key}`, SHAPES[p.shape].color);
    }
    setWallets(newWallets); setPieces(newPieces);

    // ── WIRE extrinsic plugs: A→E B→W C→E D→W (chain) ──
    const wiredA = plugConnect(newPieces["A"],"E", newPieces["B"],"W");
    const wiredB = plugConnect(newPieces["B"],"E", newPieces["C"],"W");
    const wiredC = plugConnect(newPieces["C"],"E", newPieces["D"],"W");
    setPieces(p=>({...p, A:wiredA, B:wiredB, C:wiredC }));
    emit(`  ⟷ Extrinsic plugs wired: A─E/W─B─E/W─C─E/W─D`, "#333");
    await new Promise(r=>setTimeout(r,500));

    // ── EXECUTE: parallel piece execution ──
    setPhase("working");
    emit(`⟳ Routing ${AGENT_DEFS.length} pieces (parallel fold)...`, "#aaa");
    AGENT_DEFS.forEach(a=>{ setState(a.id,"working"); setActiveId(a.id); });

    const run = AGENT_DEFS.map(async agentDef => {
      const piece  = newPieces[agentDef.id];
      const wallet = newWallets[agentDef.id];
      setActiveId(agentDef.id);
      try {
        const result = await executePiece(
          piece, wallet,
          tok => setStreams(s=>({...s,[agentDef.id]:(s[agentDef.id]||"")+tok}))
        );
        // check overflow → reroute
        if (Number(result.tokens_used) > agentDef.token_cap * 0.95) {
          const rerouted = rerouteToOmega(piece, "Ω_compress");
          setPieces(p=>({...p,[agentDef.id]:rerouted}));
          setReroutes(r=>({...r,[agentDef.id]:"Ω_compress"}));
          emit(`  ↩ Agent-${agentDef.id} overflow → Ω_compress reroute`, "#fb923c");
        }
        setResults(r=>({...r,[agentDef.id]:result}));
        setStreams(s=>({...s,[agentDef.id]:null}));
        setState(agentDef.id,"done");
        emit(`  ✓ Agent-${agentDef.id} [${piece.shape}] sealed (${result.tokens_used}t)`, SHAPES[piece.shape].color);
      } catch(err) {
        const rerouted = rerouteToOmega(piece, "Ω_quarantine");
        setPieces(p=>({...p,[agentDef.id]:rerouted}));
        setReroutes(r=>({...r,[agentDef.id]:"Ω_quarantine"}));
        setState(agentDef.id,"error");
        emit(`  ✗ Agent-${agentDef.id} fault → Ω_quarantine`, "#f87171");
      }
    });

    await Promise.all(run);
    setActiveId(null);

    // ── CLOSE GATE ──
    setPhase("closing");
    emit(`▣ Gate closing — ghost delete session ${sid}...`, "#fbbf24");
    await new Promise(r=>setTimeout(r,700));
    setPhase("closed");
    emit(`✓ Gate sealed. All piece instances dissolved.`, "#00ff9f");
  };

  const reset = () => {
    setPhase("idle"); setSessionId(null);
    setWallets({}); setPieces({}); setStreams({}); setResults({}); setReroutes({}); setLog([]);
    setStates(Object.fromEntries(AGENT_DEFS.map(a=>[a.id,"idle"]))); setActiveId(null);
  };

  const pieceList   = AGENT_DEFS.map(a => pieces[a.id]).filter(Boolean);
  const connections = AGENT_DEFS.slice(0,-1).map((_,i) => !!pieces[AGENT_DEFS[i].id]?.plugs?.E);
  const totalUsed   = Object.values(results).reduce((s,r)=>s+(Number(r?.tokens_used)||0),0);
  const totalCap    = AGENT_DEFS.reduce((s,a)=>s+a.token_cap,0);
  const anyReroute  = Object.keys(reroutes).length > 0;

  return (
    <div style={{
      minHeight:"100vh", background:"#04040a", color:"#ccc",
      fontFamily:"'IBM Plex Mono', 'Courier New', monospace",
      padding:20,
    }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;700&display=swap');
        @keyframes shimmer{0%,100%{opacity:.4}50%{opacity:1}}
        @keyframes cellpulse{0%,100%{opacity:.5}50%{opacity:1}}
        @keyframes fadein{from{opacity:0;transform:translateY(3px)}to{opacity:1;transform:translateY(0)}}
        ::-webkit-scrollbar{width:3px;height:3px}
        ::-webkit-scrollbar-thumb{background:#181818;border-radius:2px}
      `}</style>

      {/* ── HEADER ── */}
      <div style={{ marginBottom:20 }}>
        <div style={{ fontSize:8, letterSpacing:5, color:"#222", marginBottom:4 }}>
          FOLDGATE · TETRIS ROUTER · UNIFIED
        </div>
        <div style={{ fontSize:18, fontWeight:700, letterSpacing:3, color:"#eee", lineHeight:1 }}>
          FOLD↔PIECE SYSTEM
        </div>
        <div style={{ fontSize:8, color:"#2a2a2a", marginTop:3, letterSpacing:2 }}>
          fold_axis=shape · wallet_fp=geo_key · Ω=fallback · agent=piece_instance
        </div>
      </div>

      {/* ── MAPPING LEGEND ── */}
      <div style={{
        display:"flex", gap:6, flexWrap:"wrap", marginBottom:16,
        padding:"8px 10px", background:"#06060e", border:"1px solid #0e0e1a", borderRadius:4,
      }}>
        {Object.entries(AXIS_TO_SHAPE).map(([axis,shape])=>(
          <div key={axis} style={{ display:"flex", alignItems:"center", gap:4 }}>
            <span style={{ fontSize:8, color:"#333", fontFamily:"monospace" }}>ax{axis}=</span>
            <TetrominoIcon shape={shape} color={SHAPES[shape].color} size={14}/>
            <span style={{ fontSize:7, color:SHAPES[shape].color }}>{shape}</span>
          </div>
        ))}
        <div style={{ flex:1 }}/>
        <span style={{ fontSize:8, color:"#222" }}>axis→shape mapping</span>
      </div>

      {/* ── ROUTING CANVAS ── */}
      <div style={{
        background:"#06060e", border:"1px solid #0e0e1a",
        borderRadius:6, padding:"10px 14px", marginBottom:16,
      }}>
        <div style={{ fontSize:8, letterSpacing:3, color:"#222", marginBottom:6 }}>ROUTING CANVAS</div>
        <RoutingCanvas pieces={pieceList} connections={connections} activeId={activeId}/>
      </div>

      {/* ── CONTROL ── */}
      <div style={{ display:"flex", gap:10, alignItems:"center", marginBottom:16 }}>
        <button onClick={summon}
          disabled={phase!=="idle"&&phase!=="closed"}
          style={{
            background:"transparent",
            border:`1px solid ${phase==="idle"||phase==="closed" ? "#00ff9f" : "#1a1a1a"}`,
            color: phase==="idle"||phase==="closed" ? "#00ff9f" : "#2a2a2a",
            padding:"7px 20px", fontSize:9, letterSpacing:3,
            cursor:phase==="idle"||phase==="closed"?"pointer":"not-allowed",
            borderRadius:3, fontFamily:"inherit",
          }}>
          SUMMON
        </button>
        {phase==="closed" && (
          <button onClick={reset} style={{
            background:"transparent", border:"1px solid #f8717144",
            color:"#f87171", padding:"7px 14px", fontSize:9,
            letterSpacing:2, cursor:"pointer", borderRadius:3, fontFamily:"inherit",
          }}>RESET</button>
        )}
        <div style={{ fontSize:8, color: phase==="idle"?"#1a1a1a":phase==="closed"?"#00ff9f44":"#fbbf2444", letterSpacing:3 }}>
          {phase.toUpperCase()} {sessionId ? `[${sessionId}]` : ""}
        </div>
        {totalUsed>0 && (
          <div style={{ display:"flex", alignItems:"center", gap:6, marginLeft:"auto" }}>
            <span style={{ fontSize:8, color:"#2a2a2a" }}>TOKEN</span>
            <div style={{ width:80, height:2, background:"#0d0d0d", borderRadius:1 }}>
              <div style={{
                height:"100%", borderRadius:1,
                width:`${Math.min(100,(totalUsed/totalCap)*100)}%`,
                background: totalUsed>totalCap*.8?"#f87171":"#00ff9f",
                transition:"width 0.5s",
              }}/>
            </div>
            <span style={{ fontSize:8, color:"#333" }}>{totalUsed}/{totalCap}</span>
          </div>
        )}
      </div>

      {/* ── PIECE GRID ── */}
      <div style={{ display:"grid", gridTemplateColumns:"1fr 1fr", gap:10, marginBottom:16 }}>
        {AGENT_DEFS.map(a=>(
          <PieceCard key={a.id}
            agentDef={a}
            piece={pieces[a.id]}
            wallet={wallets[a.id]}
            state={states[a.id]}
            stream={streams[a.id]}
            result={results[a.id]}
          />
        ))}
      </div>

      {/* ── OMEGA NODES ── */}
      <div style={{
        background:"#06060e", border:"1px solid #0e0e1a",
        borderRadius:6, padding:"10px 14px", marginBottom:16,
      }}>
        <div style={{ fontSize:8, letterSpacing:3, color:"#1a1a1a", marginBottom:8 }}>Ω FALLBACK NODES</div>
        <div style={{ display:"grid", gridTemplateColumns:"1fr 1fr", gap:6 }}>
          {Object.entries(OMEGA).map(([type])=>(
            <OmegaNode key={type} type={type}
              triggered={!!Object.values(reroutes).find(r=>r===type)}
              piece={AGENT_DEFS.find(a=>reroutes[a.id]===type)}
            />
          ))}
        </div>
      </div>

      {/* ── GATE LOG ── */}
      {log.length>0 && (
        <div style={{
          background:"#05050b", border:"1px solid #0a0a12",
          borderRadius:6, padding:"10px 14px",
        }}>
          <div style={{ fontSize:8, letterSpacing:3, color:"#1a1a2a", marginBottom:8 }}>GATE LOG</div>
          <div ref={logRef} style={{ maxHeight:140, overflowY:"auto" }}>
            {log.map((l,i)=>(
              <div key={i} style={{
                fontSize:9, color:l.color, lineHeight:1.9,
                animation:"fadein 0.2s ease",
              }}>{l.msg}</div>
            ))}
          </div>
        </div>
      )}

      {/* ── SEALED RESULTS ── */}
      {phase==="closed" && Object.keys(results).length>0 && (
        <div style={{
          marginTop:12, background:"#04080a",
          border:"1px solid #00ff9f11", borderRadius:6, padding:"10px 14px",
        }}>
          <div style={{ fontSize:8, letterSpacing:3, color:"#00ff9f44", marginBottom:10 }}>SEALED OUTPUTS</div>
          {AGENT_DEFS.map(a=>results[a.id]&&(
            <div key={a.id} style={{ marginBottom:8, paddingBottom:8, borderBottom:"1px solid #0a0a0a" }}>
              <div style={{ display:"flex", alignItems:"center", gap:6, marginBottom:3 }}>
                <TetrominoIcon shape={results[a.id].shape||AXIS_TO_SHAPE[a.fold_axis]} color={SHAPES[results[a.id].shape||"I"].color} size={14}/>
                <span style={{ fontSize:9, color:SHAPES[results[a.id].shape||"I"].color, fontFamily:"monospace" }}>
                  Agent-{a.id} · {results[a.id].shape}-shape · {results[a.id].geo_key?.slice(0,8)}
                </span>
              </div>
              <div style={{ fontSize:9, color:"#666", lineHeight:1.6, paddingLeft:20 }}>
                {results[a.id].output?.slice(0,140)}{results[a.id].output?.length>140?"…":""}
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
