import { useState, useEffect, useRef } from "react";

// ── CONFIG ──────────────────────────────────────────────────────────────
const AGENT_ROLES = [
  { id: "A", role: "Reader",    color: "#00ff9f", cap: 300,  icon: "◈", task: "parse_data"      },
  { id: "B", role: "Analyst",   color: "#00cfff", cap: 400,  icon: "◆", task: "validate_schema" },
  { id: "C", role: "Writer",    color: "#ff6b6b", cap: 300,  icon: "◉", task: "write_output"    },
  { id: "D", role: "Auditor",   color: "#ffd93d", cap: 200,  icon: "◇", task: "audit_log"       },
];

const SYSTEM_PROMPT = (role, task, budget) =>
  `You are Agent-${role} (${task}). Token budget: ${budget}.
You can ONLY see and act on your assigned task. Respond in JSON:
{"agent":"${role}","task":"${task}","status":"done","output":"<your work>","tokens_used":<number>}
Be concise. Stay within budget. No markdown.`;

const PROJECT_MANIFEST = {
  project: "Geometric Obscurity Report Q1-2026",
  tasks: {
    parse_data:      "Extract key metrics from: Fibonacci closure: 144 cycles, Dodeca face: 3, spoke 2, slot 17",
    validate_schema: "Validate that all POGLS fields (face, spoke, slot, cycles) are non-zero integers",
    write_output:    "Write a 2-sentence executive summary of the geometric analysis findings",
    audit_log:       "Generate audit entry: timestamp, task count=4, integrity=pass",
  }
};

// ── COORD WALLET ─────────────────────────────────────────────────────────
function generateWallet(agentId, task) {
  const fp = btoa(`${agentId}:${task}:${Date.now()}`).slice(0, 16);
  return { agent_id: agentId, task, topology_fp: fp, session: crypto.randomUUID().slice(0,8) };
}

// ── API CALL ──────────────────────────────────────────────────────────────
async function summonAgent(agent, wallet, onToken) {
  const taskData = PROJECT_MANIFEST.tasks[agent.task];
  const res = await fetch("https://api.anthropic.com/v1/messages", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: "claude-sonnet-4-20250514",
      max_tokens: agent.cap,
      system: SYSTEM_PROMPT(agent.id, agent.role, agent.cap),
      messages: [{ role: "user", content: `TASK DATA (wallet:${wallet.topology_fp}): ${taskData}` }],
      stream: true,
    }),
  });

  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let full = "";
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    const lines = dec.decode(value).split("\n").filter(l => l.startsWith("data:"));
    for (const line of lines) {
      try {
        const d = JSON.parse(line.slice(5));
        if (d.type === "content_block_delta") {
          full += d.delta.text || "";
          onToken(d.delta.text || "");
        }
      } catch {}
    }
  }
  try {
    const clean = full.replace(/```json|```/g, "").trim();
    return JSON.parse(clean);
  } catch {
    return { agent: agent.id, task: agent.task, status: "done", output: full, tokens_used: "?" };
  }
}

// ── AGENT CARD ────────────────────────────────────────────────────────────
function AgentCard({ agent, state, stream, result, wallet }) {
  const statusMap = {
    idle:    { label: "STANDBY",   bg: "#0a0a0f", border: "#1a1a2e" },
    claimed: { label: "CLAIMED",   bg: "#0d1117", border: agent.color },
    working: { label: "WORKING",   bg: "#0d1117", border: agent.color },
    done:    { label: "SEALED",    bg: "#050a05", border: agent.color },
    error:   { label: "FAULT",     bg: "#1a0505", border: "#ff3333"  },
  };
  const s = statusMap[state] || statusMap.idle;
  const pulse = state === "working";

  return (
    <div style={{
      background: s.bg,
      border: `1px solid ${s.border}`,
      borderRadius: 8,
      padding: "14px 16px",
      position: "relative",
      overflow: "hidden",
      transition: "all 0.3s",
      boxShadow: state !== "idle" ? `0 0 20px ${agent.color}22` : "none",
    }}>
      {pulse && (
        <div style={{
          position: "absolute", inset: 0,
          background: `radial-gradient(ellipse at 50% 0%, ${agent.color}11, transparent 70%)`,
          animation: "pulse 2s ease-in-out infinite",
        }}/>
      )}
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 8 }}>
        <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
          <span style={{ color: agent.color, fontSize: 18 }}>{agent.icon}</span>
          <span style={{ color: "#eee", fontFamily: "monospace", fontSize: 13, fontWeight: 700 }}>
            AGENT-{agent.id}
          </span>
          <span style={{ color: "#666", fontSize: 11 }}>{agent.role}</span>
        </div>
        <span style={{
          fontSize: 9, letterSpacing: 2,
          color: state === "idle" ? "#333" : agent.color,
          fontFamily: "monospace",
        }}>{s.label}</span>
      </div>

      {/* wallet tag */}
      {wallet && (
        <div style={{ fontSize: 9, fontFamily: "monospace", color: "#444", marginBottom: 6 }}>
          ◈ {wallet.topology_fp} · {wallet.task}
        </div>
      )}

      {/* token cap bar */}
      <div style={{ display: "flex", gap: 4, alignItems: "center", marginBottom: 8 }}>
        <div style={{ flex: 1, height: 3, background: "#111", borderRadius: 2 }}>
          <div style={{
            height: "100%", borderRadius: 2,
            width: result ? `${Math.min(100, (result.tokens_used / agent.cap) * 100)}%` : state === "working" ? "60%" : "0%",
            background: agent.color,
            transition: "width 0.5s",
          }}/>
        </div>
        <span style={{ fontSize: 9, color: "#555", fontFamily: "monospace" }}>
          {result ? `${result.tokens_used}/${agent.cap}t` : `${agent.cap}t`}
        </span>
      </div>

      {/* stream output */}
      <div style={{
        fontFamily: "monospace", fontSize: 10, color: "#aaa",
        minHeight: 40, maxHeight: 80, overflow: "hidden",
        lineHeight: 1.5,
      }}>
        {stream
          ? stream.slice(-200)
          : result
          ? <span style={{ color: "#6a6" }}>✓ {result.output?.slice(0, 120)}</span>
          : <span style={{ color: "#333" }}>awaiting summon...</span>
        }
      </div>
    </div>
  );
}

// ── GATE VISUALIZER ───────────────────────────────────────────────────────
function GateRing({ open, phase }) {
  const colors = ["#00ff9f","#00cfff","#ff6b6b","#ffd93d"];
  return (
    <div style={{ position: "relative", width: 120, height: 120, margin: "0 auto" }}>
      {[0,1,2,3].map(i => (
        <div key={i} style={{
          position: "absolute",
          inset: i * 14,
          borderRadius: "50%",
          border: `1px solid ${open ? colors[i] : "#1a1a2e"}`,
          opacity: open ? 1 : 0.3,
          animation: open ? `spin${i % 2 === 0 ? "cw" : "ccw"} ${3 + i}s linear infinite` : "none",
          transition: "border-color 0.5s, opacity 0.5s",
        }}/>
      ))}
      <div style={{
        position: "absolute", inset: 0,
        display: "flex", alignItems: "center", justifyContent: "center",
        fontSize: 11, fontFamily: "monospace",
        color: open ? "#fff" : "#333",
        transition: "color 0.5s",
      }}>
        {phase === "idle" && "GATE"}
        {phase === "open" && "▶ OPEN"}
        {phase === "working" && "⟳ RUN"}
        {phase === "closing" && "▣ SEAL"}
        {phase === "closed" && "✓ DONE"}
      </div>
    </div>
  );
}

// ── MAIN ──────────────────────────────────────────────────────────────────
export default function SummoningGate() {
  const [phase, setPhase] = useState("idle"); // idle|open|working|closing|closed
  const [agentStates, setAgentStates] = useState(Object.fromEntries(AGENT_ROLES.map(a => [a.id, "idle"])));
  const [streams, setStreams] = useState({});
  const [results, setResults] = useState({});
  const [wallets, setWallets] = useState({});
  const [log, setLog] = useState([]);
  const [sessionId, setSessionId] = useState(null);
  const logRef = useRef(null);

  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [log]);

  const addLog = (msg, color = "#666") => setLog(l => [...l, { msg, color, t: Date.now() }]);

  const setAgentState = (id, state) => setAgentStates(s => ({ ...s, [id]: state }));

  const runSummon = async () => {
    if (phase !== "idle") return;

    // reset
    setResults({}); setStreams({}); setWallets({}); setLog([]);
    setAgentStates(Object.fromEntries(AGENT_ROLES.map(a => [a.id, "idle"])));

    const sid = crypto.randomUUID().slice(0, 8);
    setSessionId(sid);

    // OPEN GATE
    setPhase("open");
    addLog(`▶ Gate opened — session ${sid}`, "#00ff9f");

    // CLAIM — generate wallets
    const newWallets = {};
    for (const agent of AGENT_ROLES) {
      const w = generateWallet(agent.id, agent.task);
      newWallets[agent.id] = w;
      setAgentState(agent.id, "claimed");
      addLog(`  ◈ Agent-${agent.id} claimed ${agent.task} [${w.topology_fp}]`, agent.color);
    }
    setWallets(newWallets);
    await new Promise(r => setTimeout(r, 600));

    // WORK — parallel summon
    setPhase("working");
    addLog(`⟳ All agents working (parallel)...`, "#aaa");
    AGENT_ROLES.forEach(a => setAgentState(a.id, "working"));

    const promises = AGENT_ROLES.map(agent =>
      summonAgent(
        agent,
        newWallets[agent.id],
        (token) => setStreams(s => ({ ...s, [agent.id]: (s[agent.id] || "") + token }))
      ).then(result => {
        setResults(r => ({ ...r, [agent.id]: result }));
        setStreams(s => ({ ...s, [agent.id]: null }));
        setAgentState(agent.id, "done");
        addLog(`  ✓ Agent-${agent.id} sealed (${result.tokens_used}t used)`, agent.color);
        return result;
      }).catch(err => {
        setAgentState(agent.id, "error");
        addLog(`  ✗ Agent-${agent.id} fault: ${err.message}`, "#ff3333");
      })
    );

    await Promise.all(promises);

    // CLOSE GATE
    setPhase("closing");
    addLog(`▣ Closing gate — ghost deleting session...`, "#ffd93d");
    await new Promise(r => setTimeout(r, 800));

    setPhase("closed");
    addLog(`✓ Gate closed. Session ${sid} ghost deleted. Wallets sealed.`, "#00ff9f");
  };

  const reset = () => {
    setPhase("idle");
    setAgentStates(Object.fromEntries(AGENT_ROLES.map(a => [a.id, "idle"])));
    setStreams({}); setResults({}); setWallets({}); setLog([]); setSessionId(null);
  };

  const totalUsed = Object.values(results).reduce((s, r) => s + (Number(r?.tokens_used) || 0), 0);
  const totalCap  = AGENT_ROLES.reduce((s, a) => s + a.cap, 0);

  return (
    <div style={{
      minHeight: "100vh", background: "#05050a",
      fontFamily: "'Space Mono', monospace",
      padding: 24, color: "#eee",
    }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&display=swap');
        @keyframes pulse { 0%,100%{opacity:.3} 50%{opacity:1} }
        @keyframes spincw  { from{transform:rotate(0deg)} to{transform:rotate(360deg)} }
        @keyframes spinccw { from{transform:rotate(0deg)} to{transform:rotate(-360deg)} }
        @keyframes fadein { from{opacity:0;transform:translateY(4px)} to{opacity:1;transform:translateY(0)} }
        ::-webkit-scrollbar{width:4px} ::-webkit-scrollbar-track{background:#0a0a0f}
        ::-webkit-scrollbar-thumb{background:#222;border-radius:2px}
      `}</style>

      {/* Header */}
      <div style={{ textAlign: "center", marginBottom: 32 }}>
        <div style={{ fontSize: 10, letterSpacing: 6, color: "#444", marginBottom: 6 }}>
          GEOMETRIC OBSCURITY SYSTEM
        </div>
        <div style={{ fontSize: 22, fontWeight: 700, letterSpacing: 2, color: "#fff" }}>
          SUMMONING GATE
        </div>
        <div style={{ fontSize: 9, color: "#333", marginTop: 4 }}>
          {PROJECT_MANIFEST.project}
        </div>
      </div>

      <div style={{ maxWidth: 700, margin: "0 auto" }}>

        {/* Gate Ring + Controls */}
        <div style={{ textAlign: "center", marginBottom: 32 }}>
          <GateRing open={phase !== "idle" && phase !== "closed"} phase={phase} />
          <div style={{ marginTop: 20, display: "flex", gap: 12, justifyContent: "center" }}>
            <button onClick={runSummon} disabled={phase !== "idle" && phase !== "closed"}
              style={{
                background: phase === "idle" || phase === "closed" ? "#00ff9f22" : "#111",
                border: `1px solid ${phase === "idle" || phase === "closed" ? "#00ff9f" : "#222"}`,
                color: phase === "idle" || phase === "closed" ? "#00ff9f" : "#444",
                padding: "8px 24px", fontSize: 10, letterSpacing: 3,
                cursor: phase === "idle" || phase === "closed" ? "pointer" : "not-allowed",
                borderRadius: 4, fontFamily: "monospace",
              }}>
              SUMMON ALL
            </button>
            {phase === "closed" && (
              <button onClick={reset} style={{
                background: "#ff6b6b22", border: "1px solid #ff6b6b44",
                color: "#ff6b6b", padding: "8px 16px", fontSize: 10,
                letterSpacing: 2, cursor: "pointer", borderRadius: 4, fontFamily: "monospace",
              }}>RESET</button>
            )}
          </div>

          {/* Token budget bar */}
          {totalUsed > 0 && (
            <div style={{ marginTop: 16, display: "flex", alignItems: "center", gap: 8, justifyContent: "center" }}>
              <span style={{ fontSize: 9, color: "#555" }}>TOTAL TOKENS</span>
              <div style={{ width: 160, height: 3, background: "#111", borderRadius: 2 }}>
                <div style={{
                  height: "100%", borderRadius: 2,
                  width: `${Math.min(100, (totalUsed / totalCap) * 100)}%`,
                  background: totalUsed > totalCap * 0.8 ? "#ff6b6b" : "#00ff9f",
                  transition: "width 0.5s",
                }}/>
              </div>
              <span style={{ fontSize: 9, color: "#555" }}>{totalUsed}/{totalCap}</span>
            </div>
          )}
        </div>

        {/* Agent Grid */}
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 12, marginBottom: 20 }}>
          {AGENT_ROLES.map(agent => (
            <AgentCard
              key={agent.id}
              agent={agent}
              state={agentStates[agent.id]}
              stream={streams[agent.id]}
              result={results[agent.id]}
              wallet={wallets[agent.id]}
            />
          ))}
        </div>

        {/* Gate Log */}
        {log.length > 0 && (
          <div style={{
            background: "#08080f", border: "1px solid #111",
            borderRadius: 8, padding: 14,
          }}>
            <div style={{ fontSize: 9, letterSpacing: 3, color: "#333", marginBottom: 10 }}>
              GATE LOG — session {sessionId}
            </div>
            <div ref={logRef} style={{ maxHeight: 160, overflowY: "auto" }}>
              {log.map((l, i) => (
                <div key={i} style={{
                  fontSize: 10, color: l.color, lineHeight: 1.8,
                  animation: "fadein 0.2s ease",
                }}>
                  {l.msg}
                </div>
              ))}
            </div>
          </div>
        )}

        {/* Results summary */}
        {phase === "closed" && Object.keys(results).length > 0 && (
          <div style={{
            marginTop: 16, background: "#050f07",
            border: "1px solid #00ff9f22", borderRadius: 8, padding: 14,
          }}>
            <div style={{ fontSize: 9, letterSpacing: 3, color: "#00ff9f66", marginBottom: 10 }}>
              SEALED OUTPUTS
            </div>
            {AGENT_ROLES.map(agent => results[agent.id] && (
              <div key={agent.id} style={{ marginBottom: 10 }}>
                <span style={{ color: agent.color, fontSize: 10 }}>{agent.icon} Agent-{agent.id} ({agent.role})</span>
                <div style={{ fontSize: 10, color: "#888", marginTop: 4, lineHeight: 1.6 }}>
                  {results[agent.id].output}
                </div>
              </div>
            ))}
          </div>
        )}

      </div>
    </div>
  );
}
