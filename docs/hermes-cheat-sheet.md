# Hermes Cheat Sheet — เครื่องมือทั้งหมดที่มี

> Updated: July 18, 2026 | Hermes v0.18.2 | 68 skills + 17 plugins enabled

---

## 🔧 Core Tools (built-in)

| Tool | ทำอะไร | ใช้ตอนไหน |
|------|--------|-----------|
| `read_file` | อ่านไฟล์ + line numbers | ดูโค้ด/ข้อความ |
| `write_file` | เขียนไฟล์ทั้งหมด | สร้าง/overwrite ไฟล์ |
| `patch` | แก้ไขแบบ find-replace | แก้บางบรรทัด |
| `search_files` | ค้นหาเนื้อหา/ไฟล์ (ripgrep) | grep/find |
| `terminal` | รัน shell command | build, git, run |
| `browser_*` | เปิด/คลิก/พิมพ์ในเว็บ | web interaction |
| `vision_analyze` | วิเคราะห์รูปภาพ | ดูรูป/图表 |
| `text_to_speech` | แปลงข้อความเป็นเสียง | voice output |
| `memory` | บันทึก/จัดการ persistent memory | สิ่งที่ต้องจำข้าม session |
| `session_search` | ค้นหาบทสนทนาย้อนหลัง | หาว่าเคยคุยอะไร |
| `todo` | จัดการ task list | วางแผนงาน |
| `clarify` | ถาม user เพื่อตัดสินใจ | เลือก approach |
| `delegate_task` | ส่งงานให้ subagent | parallel work |
| `cronjob` | ตั้งเวลาทำงานอัตโนมัติ | scheduled tasks |
| `skill_view` | โหลด skill content | ใช้ skill |
| `skill_manage` | สร้าง/แก้ไข/ลบ skill | จัดการ skills |
| `project_create/switch/list` | จัดการ workspace | สลับโปรเจกต์ |

---

## 🔌 Plugins (Enabled)

### 🛡️ Quality

| Plugin | ทำอะไร | ใช้ตอนไหน |
|--------|--------|-----------|
| `evey-reflect` | Critique output ก่อนส่ง — จับ error/miss | ทุกครั้งที่ส่งคำตอบสำคัญ |
| `evey-validate` | ตรวจ hallucination (regex + LLM scoring 0-10) | ตอนอ้างอิงข้อมูล |
| `evey-email-guard` | Screen prompt injection (20+ regex + AI) | ตอนรับ email/external text |
| `evey-sandbox` | Run code ใน sandbox + resource limits | execute code ปลอดภัย |
| `evey-session-guard` | จัดการ session lifecycle + cleanup | อัตโนมัติ |
| `security-guidance** | Warn ตอนเขียน code อันตราย (eval, pickle, yaml.load) | ทุกครั้งที่เขียน file |

### 📚 Learning

| Plugin | ทำอะไร | ใช้ตอนไหน |
|--------|--------|-----------|
| `evey-learner` | เรียนรู้จาก interactions → ใช้ครั้งถัดไป | อัตโนมัติ |
| `evey-memory-adaptive` | Importance scoring + decay — memory สำคัญอยู่นาน | อัตโนมัติ |
| `evey-memory-consolidate` | Nightly: extract facts → MEMORY.md + vectors | cron job |
| `evey-cache` | ไม่ถามคำถามเดิมซ้ำ — ประหยัด tokens | อัตโนมัติ |
| `evey-identity` | SOUL.md evolves based on interactions | อัตโนมัติ |

### 🔗 Communication

| Plugin | ทำอะไร | ใช้ตอนไหน |
|--------|--------|-----------|
| `evey-bridge` | สื่อสารกับ Claude Code ได้ | ถ้าใช้ Claude Code ด้วย |
| `evey-goals` | Autonomous goal management | ตั้ง goal แล้ว agent ทำเอง |
| `evey-telegram-ux` | Rich Telegram formatting | ถ้าใช้ Telegram |
| `evey-research` | Automated research pipeline | ค้นคว้าข้อมูล |
| `evey-digest` | Daily digest จากหลาย source | cron job |
| `evey-delegation-score` | Score + rank delegation quality | ติดตาม subagent quality |

---

## 📚 Skills (68 installed)

### 🤖 Autonomous AI Agents

| Skill | ทำอะไร |
|-------|--------|
| `hermes-agent` | Configure/extend Hermes Agent |
| `claude-code` | Delegate coding to Claude Code CLI |
| `codex` | Delegate coding to OpenAI Codex CLI |
| `opencode` | Delegate coding to OpenCode CLI |

### 💻 Software Development

| Skill | ทำอะไร |
|-------|--------|
| `plan` | เขียน markdown plan → .hermes/plans/ |
| `spike` | Throwaway experiment ก่อน build |
| `systematic-debugging` | 4-phase root cause debugging |
| `test-driven-development` | RED-GREEN-REFACTOR cycle |
| `requesting-code-review` | Pre-commit review + security scan |
| `simplify-code` | Parallel 3-agent code cleanup |
| `node-inspect-debugger` | Debug Node.js via Chrome DevTools |
| `geometric-compression` | 🏗️ FGLS geometric compression system |
| `geometric-pipeline-qa` | 🏗️ QA for geometric pipeline |
| `computation-over-storage` | 🏗️ Architecture pattern |

### 🎨 Creative

| Skill | ทำอะไร |
|-------|--------|
| `claude-design` | ออกแบบ HTML (landing, deck, prototype) |
| `sketch` | ปาด HTML mockup 2-3 แบบ |
| `excalidraw` | Hand-drawn diagrams (arch, flow, seq) |
| `architecture-diagram` | Dark-themed SVG architecture diagrams |
| `ascii-art` | ASCII art (pyfiglet, cowsay) |
| `ascii-video` | Convert video → colored ASCII |
| `p5js` | p5.js sketches (gen art, shaders, 3D) |
| `manim-video` | 3Blue1Brown-style math animations |
| `comfyui` | Generate images/video/audio |
| `humanizer` | ลบ AI-isms จากข้อความ |
| `songwriting-and-ai-music` | Songwriting + Suno AI prompts |

### 📊 Data Science & MLOps

| Skill | ทำอะไร |
|-------|--------|
| `jupyter-live-kernel` | Iterative Python via live Jupyter |
| `huggingface-hub` | Search/download/upload models |
| `llama-cpp` | Local GGUF inference |
| `weights-and-biases` | Log ML experiments |
| `segment-anything-model` | SAM: zero-shot image segmentation |

### 🐙 GitHub

| Skill | ทำอะไร |
|-------|--------|
| `github-auth` | GitHub auth setup |
| `github-pr-workflow` | PR lifecycle: branch→commit→open→merge |
| `github-code-review` | Review PRs + inline comments |
| `github-issues` | Create/triage/assign issues |
| `github-repo-management` | Clone/create/fork repos |
| `codebase-inspection` | LOC, languages, ratios |

### 📝 Productivity

| Skill | ทำอะไร |
|-------|--------|
| `google-workspace` | Gmail, Calendar, Drive, Docs, Sheets |
| `notion` | Notion API + ntn CLI |
| `obsidian` | Read/search/edit Obsidian vault |
| `powerpoint` | Create/read/edit .pptx decks |
| `nano-pdf` | Edit PDF text/typos |
| `ocr-and-documents` | Extract text from PDFs/scans |
| `airtable` | Airtable REST API via curl |
| `maps` | Geocode, POIs, routes |

### 🔬 Research

| Skill | ทำอะไร |
|-------|--------|
| `arxiv` | Search arXiv papers |
| `blogwatcher` | Monitor blogs/RSS feeds |
| `polymarket` | Query Polymarket markets |
| `llm-wiki` | Karpathy's LLM Wiki |

### 🎵 Media

| Skill | ทำอะไร |
|-------|--------|
| `youtube-content` | YouTube transcripts → summaries |
| `gif-search` | Search/download GIFs from Tenor |
| `heartmula` | Suno-like song generation |
| `songsee` | Audio spectrograms/features |

### 📧 Email

| Skill | ทำอะไร |
|-------|--------|
| `himalaya` | IMAP/SMTP email from terminal |

---

## 🎯 Quick Reference — ทำ X ยังไง?

| ต้องการ | ใช้ |
|---------|-----|
| อ่านไฟล์ | `read_file` |
| แก้โค้ดบางจุด | `patch` |
| รัน build/test | `terminal` |
| ค้นหาในโปรเจกต์ | `search_files` |
| ดูเว็บ/กรอกฟอร์ม | `browser_*` |
| ดูรูป | `vision_analyze` |
| ค้นข้อมูล | `web_search` / `evey-research` |
| สร้าง diagram | `skill_view("excalidraw")` |
| สร้าง UI mockup | `skill_view("sketch")` |
| Debug crash | `skill_view("systematic-debugging")` |
| TDD workflow | `skill_view("test-driven-development")` |
| GitHub PR | `skill_view("github-pr-workflow")` |
| จำสิ่งสำคัญ | `memory` tool |
| หาว่าเคยคุยอะไร | `session_search` |
| ตั้ง cron job | `cronjob` tool |
| ส่งงานให้คนอื่น | `delegate_task` |

---

## ⌨️ Slash Commands (ใน chat)

| Command | ทำอะไร |
|---------|--------|
| `/reset` | เริ่ม session ใหม่ |
| `/model` | เปลี่ยน model |
| `/skills` | Search/install skills |
| `/cron` | จัดการ cron jobs |
| `/plugins` | ดู plugins |
| `/config` | ดู config |
| `/debug` | Upload debug report |
| `/goal` | ตั้ง standing goal |
| `/yolo` | Skip approval prompts |
| `/help` | ดูคำสั่งทั้งหมด |

---

*Generated from actual system state — July 18, 2026*
