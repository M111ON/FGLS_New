# Design Decisions

## 2026-07-02 Inbox Dashboard — Web dashboard for board visibility

**Context**: User wanted to view inbox/board state interactively. Three options were discussed: AI-formatted display on command, terminal UI, or web dashboard. The requirement was "สามารถแสดงผลได้จะเป็นเวปหรือเป็น interface อะไรก็ได้" (any interface is fine).

**Decision**: Built a web dashboard using Python's built-in `http.server` (zero dependencies) that serves an HTML page with auto-refresh and JSON API endpoints.

**Alternatives considered**:
- AI display via chat command (`/board`) — requires user to ask every time, no persistent view
- Terminal UI — less accessible, harder to style
- MCP resource approach — too coupled to MCP protocol

**Reasoning**: Web dashboard is zero-dependency (no Flask required), accessible from any browser, auto-refreshes every 10s, and provides the most visual information at a glance.

**AI contribution**:
  - Identified: Dashboard JS error (called `/api/tool` POST but server only had GET endpoints for `/api/state` and `/api/handoff`) — root cause diagnosed and fixed.
  - Suggested: Used `http.server` over Flask to avoid external dependencies failure mode.
  - Developer-driven: Chose web interface over terminal/MCP approaches.

**Intent class**: FEATURE_BUILDING
**Signal score**: HIGH
**Outcome**: implemented

## 2026-07-02 Cross-Session Board — Global skill + MCP integration

**Context**: Need persistent board across opencode sessions, per-workspace, with automatic context loading on session start.

**Decision**: Extended existing `inbox_mcp_server.py` with board + source tools, made workspace detection automatic via CWD, and created a global skill with New Session Protocol.

**Alternatives considered**:
- Separate board-only MCP server — rejected (simpler to extend existing)
- Git-based storage for board data — rejected (too heavy for status tracking)
- Symlinks for dashboard.py dedup — rejected (used hardlinks instead)

**Reasoning**: Single MCP server = simpler config. CWD-based workspace detection = zero config in skill. Per-project vault storage = no data mixing.

**AI contribution**:
  - Identified: Hardlink dedup opportunity for engine_dashboard.py (3 identical copies → 1 canonical + 2 hardlinks)
  - Suggested: All board + source tool implementations, global skill structure with New Session Protocol
  - Developer-driven: Workspace auto-detect approach (CWD vs --workspace arg)

**Intent class**: FEATURE_BUILDING
**Signal score**: HIGH
**Outcome**: implemented
