"""
llm_memory_ingest.py

Ingest exported chat logs into a clean, canonical JSONL memory store.

Supported inputs:
  - JSON / JSONL transcript exports
  - Markdown / plain text chat transcripts with role prefixes

Output layout:
  <out_dir>/
    memory.jsonl          canonical message records
    conversations.jsonl    one compact record per conversation
    manifest.json         run summary + source inventory
    raw/                  optional copies of ingested exports

The design goal is intentionally narrow:
  export -> clean normalize -> store

No embeddings, no retrieval, no model calls here.
Those are separate layers.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import queue
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Iterator, Optional


ROLE_PATTERNS = (
    re.compile(r"^\s*(user|assistant|system|tool)\s*:\s*(.*)$", re.IGNORECASE),
    re.compile(r"^\s*#{1,6}\s*(user|assistant|system|tool)\s*$", re.IGNORECASE),
    re.compile(r"^\s*\*\*(user|assistant|system|tool)\s*:\s*\*\*\s*(.*)$", re.IGNORECASE),
)


@dataclass(frozen=True)
class ParserAdapter:
    name: str
    suffixes: tuple[str, ...]


@dataclass
class MessageRecord:
    conversation_id: str
    message_id: str
    turn_index: int
    role: str
    content: str
    source_path: str
    source_type: str
    source_name: str
    project: str
    title: str
    ts: str
    content_hash: str

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


@dataclass
class ConversationRecord:
    conversation_id: str
    title: str
    source_path: str
    source_type: str
    source_name: str
    project: str
    message_count: int
    roles: list[str]
    first_ts: str
    last_ts: str
    content_hash: str
    chunk_count: int
    summary: str
    keywords: list[str]
    chunk_ids: list[str]

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


@dataclass
class ChunkRecord:
    conversation_id: str
    chunk_id: str
    chunk_index: int
    start_turn: int
    end_turn: int
    message_count: int
    char_count: int
    role_counts: dict[str, int]
    source_path: str
    source_type: str
    source_name: str
    project: str
    title: str
    ts_first: str
    ts_last: str
    content_hash: str
    keywords: list[str]
    summary: str
    text: str

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def stable_hash(*parts: str) -> str:
    h = hashlib.sha256()
    for part in parts:
        h.update(part.encode("utf-8", errors="ignore"))
        h.update(b"\x1f")
    return h.hexdigest()


def file_fingerprint(path: Path) -> str:
    raw = path.read_bytes()
    return stable_hash("file", hashlib.sha256(raw).hexdigest())


def normalize_text(text: str) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\x00", "")
    lines = [line.rstrip() for line in text.split("\n")]
    text = "\n".join(lines)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def compact_spaces(text: str) -> str:
    return re.sub(r"[ \t]+", " ", text).strip()


def truncate_text(text: str, limit: int = 180) -> str:
    text = compact_spaces(normalize_text(text))
    if len(text) <= limit:
        return text
    return text[: max(0, limit - 1)].rstrip() + "…"


STOPWORDS = {
    "a", "an", "and", "are", "as", "at", "be", "but", "by", "can", "do",
    "for", "from", "has", "have", "he", "her", "his", "i", "if", "in", "is",
    "it", "its", "me", "my", "not", "of", "on", "or", "our", "she", "so",
    "that", "the", "their", "them", "there", "they", "this", "to", "up",
    "we", "what", "when", "where", "which", "who", "will", "with", "you",
    "your", "ได้", "จะ", "ที่", "และ", "ใน", "ของ", "ให้", "ว่า", "ไม่", "เป็น",
    "เรา", "ผม", "ฉัน", "มัน", "ครับ", "ค่ะ", "ครับผม",
}

DECISION_RE = re.compile(
    r"\b("
    r"need|should|must|can|fix|add|remove|change|choose|select|"
    r"decide|decision|implement|summary|chunk|memory|cache|store|"
    r"save|load|export|import|test|verify|run|use|replace|fallback|"
    r"preserve|support|limit|need to|would like|prefer"
    r")\b",
    re.IGNORECASE,
)


def detect_source_type(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix in {".json"}:
        return "json"
    if suffix in {".jsonl", ".ndjson"}:
        return "jsonl"
    if suffix in {".md", ".markdown", ".txt"}:
        return "markdown"
    return "text"


PARSER_REGISTRY = (
    ParserAdapter("chat_messages_json", (".json",)),
    ParserAdapter("canonical_memory_json", (".json",)),
    ParserAdapter("role_messages_json", (".json",)),
    ParserAdapter("jsonl_transcript", (".jsonl", ".ndjson")),
    ParserAdapter("markdown_transcript", (".md", ".markdown", ".txt")),
)


def iter_json_message_like(node: object) -> Iterator[dict]:
    if isinstance(node, dict):
        chat_messages = node.get("chat_messages")
        if isinstance(chat_messages, list):
            for item in chat_messages:
                if isinstance(item, dict):
                    role = str(item.get("sender") or item.get("role") or "").strip().lower()
                    if role == "human":
                        role = "user"
                    elif role == "assistant":
                        role = "assistant"
                    elif role == "system":
                        role = "system"
                    elif role == "tool":
                        role = "tool"
                    content = item.get("text")
                    if content is None:
                        content_obj = item.get("content")
                        if isinstance(content_obj, str):
                            content = content_obj
                        elif isinstance(content_obj, list):
                            content = "\n".join(
                                str(part.get("text", part)) if isinstance(part, dict) else str(part)
                                for part in content_obj
                                if part is not None
                            )
                        elif isinstance(content_obj, dict):
                            parts = content_obj.get("parts")
                            if isinstance(parts, list):
                                content = "\n".join(str(p) for p in parts if p is not None)
                    if isinstance(content, str) and role and content:
                        ts = item.get("created_at") or item.get("updated_at") or item.get("create_time")
                        yield {"role": role, "content": content, "ts": ts}

        role = node.get("role")
        content = node.get("content")
        if isinstance(role, str) and role and content is not None:
            yield {"role": role, "content": content, "ts": node.get("create_time") or node.get("created_at")}

        author = node.get("author")
        if isinstance(author, dict):
            author_role = author.get("role")
            if isinstance(author_role, str):
                content_obj = node.get("content")
                if isinstance(content_obj, dict):
                    parts = content_obj.get("parts")
                    if isinstance(parts, list):
                        yield {
                            "role": author_role,
                            "content": "\n".join(str(p) for p in parts if p is not None),
                            "ts": node.get("create_time") or node.get("created_at"),
                        }

        message = node.get("message")
        if isinstance(message, dict):
            yield from iter_json_message_like(message)

        for key in ("messages", "conversation", "conversations", "mapping", "children", "items"):
            value = node.get(key)
            if isinstance(value, dict):
                for item in value.values():
                    yield from iter_json_message_like(item)
            elif isinstance(value, list):
                for item in value:
                    yield from iter_json_message_like(item)

    elif isinstance(node, list):
        for item in node:
            yield from iter_json_message_like(item)


def extract_chat_messages_export(data: object) -> list[dict]:
    messages: list[dict] = []
    seen = set()

    if isinstance(data, list):
        conversations = data
    elif isinstance(data, dict):
        conversations = [data]
    else:
        return messages

    for conv in conversations:
        if not isinstance(conv, dict):
            continue
        chat_messages = conv.get("chat_messages")
        if not isinstance(chat_messages, list):
            continue

        for item in chat_messages:
            if not isinstance(item, dict):
                continue
            role = str(item.get("sender") or item.get("role") or "").strip().lower()
            if role == "human":
                role = "user"
            elif role == "assistant":
                role = "assistant"
            elif role == "system":
                role = "system"
            elif role == "tool":
                role = "tool"

            content = item.get("text")
            if content is None:
                content_obj = item.get("content")
                if isinstance(content_obj, str):
                    content = content_obj
                elif isinstance(content_obj, list):
                    content = "\n".join(
                        str(part.get("text", part)) if isinstance(part, dict) else str(part)
                        for part in content_obj
                        if part is not None
                    )
                elif isinstance(content_obj, dict):
                    parts = content_obj.get("parts")
                    if isinstance(parts, list):
                        content = "\n".join(str(p) for p in parts if p is not None)

            if not isinstance(content, str):
                continue
            content = normalize_text(content)
            if not role or not content:
                continue

            fingerprint = stable_hash("chat_messages_json", role, content, str(item.get("created_at") or item.get("updated_at") or ""))
            if fingerprint in seen:
                continue
            seen.add(fingerprint)
            ts = item.get("created_at") or item.get("updated_at") or item.get("create_time")
            messages.append({"role": role, "content": content, "ts": ts})

    return messages


def extract_canonical_memory_json(data: object) -> list[dict]:
    messages: list[dict] = []
    seen = set()

    if isinstance(data, list):
        items = data
    elif isinstance(data, dict):
        if isinstance(data.get("memory"), list):
            items = data["memory"]
        elif isinstance(data.get("messages"), list):
            items = data["messages"]
        elif isinstance(data.get("records"), list):
            items = data["records"]
        else:
            items = [data]
    else:
        return messages

    for item in items:
        if not isinstance(item, dict):
            continue

        role = str(item.get("role", "")).strip().lower()
        if role == "human":
            role = "user"
        elif role == "assistant":
            role = "assistant"
        elif role == "system":
            role = "system"
        elif role == "tool":
            role = "tool"

        content = item.get("content")
        if content is None:
            content = item.get("text")
        if isinstance(content, list):
            content = "\n".join(str(p) for p in content if p is not None)
        elif isinstance(content, dict):
            parts = content.get("parts")
            if isinstance(parts, list):
                content = "\n".join(str(p) for p in parts if p is not None)
        if not isinstance(content, str):
            continue

        content = normalize_text(content)
        if not role or not content:
            continue

        ts = item.get("ts") or item.get("created_at") or item.get("updated_at") or item.get("create_time")
        fingerprint = stable_hash(
            "canonical_memory_json",
            str(item.get("conversation_id") or ""),
            str(item.get("message_id") or ""),
            role,
            content,
            str(ts or ""),
        )
        if fingerprint in seen:
            continue
        seen.add(fingerprint)
        messages.append({"role": role, "content": content, "ts": ts})

    return messages


def extract_json_messages(data: object) -> list[dict]:
    messages = []
    seen = set()
    for item in iter_json_message_like(data):
        role = str(item.get("role", "")).strip().lower()
        content = item.get("content")
        if isinstance(content, list):
            content = "\n".join(str(p) for p in content if p is not None)
        elif content is None:
            content = ""
        else:
            content = str(content)

        content = normalize_text(content)
        if not role or not content:
            continue

        fingerprint = stable_hash(role, content, str(item.get("ts") or ""))
        if fingerprint in seen:
            continue
        seen.add(fingerprint)
        messages.append({
            "role": role,
            "content": content,
            "ts": item.get("ts"),
        })
    return messages


def extract_role_messages_json(data: object) -> list[dict]:
    return extract_json_messages(data)


def extract_jsonl_messages(lines: Iterable[str]) -> list[dict]:
    messages: list[dict] = []
    for raw in lines:
        raw = raw.strip()
        if not raw:
            continue
        try:
            item = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if isinstance(item, dict):
            messages.extend(extract_json_messages(item))
        elif isinstance(item, list):
            messages.extend(extract_json_messages(item))
    return messages


def parse_markdown_transcript(text: str) -> list[dict]:
    messages: list[dict] = []
    current_role: Optional[str] = None
    current_lines: list[str] = []

    def flush() -> None:
        nonlocal current_role, current_lines
        if current_role is None:
            current_lines = []
            return
        content = normalize_text("\n".join(current_lines))
        if content:
            messages.append({"role": current_role, "content": content, "ts": None})
        current_role = None
        current_lines = []

    for line in text.splitlines():
        matched = False
        for pat in ROLE_PATTERNS:
            m = pat.match(line)
            if not m:
                continue
            matched = True
            flush()
            current_role = m.group(1).lower()
            tail = m.group(2) if m.lastindex and m.lastindex >= 2 else ""
            if tail:
                current_lines.append(tail)
            break
        if matched:
            continue

        if current_role is None:
            continue
        current_lines.append(line)

    flush()
    return messages


def parse_export_payload(path: Path, raw: str) -> tuple[list[dict], str, str]:
    source_type = detect_source_type(path)
    for adapter in PARSER_REGISTRY:
        if source_type == "json" and adapter.name == "chat_messages_json":
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                return parse_markdown_transcript(raw), "markdown", "markdown_transcript"
            messages = extract_chat_messages_export(data)
            if messages:
                return messages, "json", adapter.name

        elif source_type == "json" and adapter.name == "canonical_memory_json":
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                return parse_markdown_transcript(raw), "markdown", "markdown_transcript"
            messages = extract_canonical_memory_json(data)
            if messages:
                return messages, "json", adapter.name

        elif source_type == "json" and adapter.name == "role_messages_json":
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                return parse_markdown_transcript(raw), "markdown", "markdown_transcript"
            messages = extract_role_messages_json(data)
            if messages:
                return messages, "json", adapter.name

        elif source_type == "jsonl" and adapter.name == "jsonl_transcript":
            messages = extract_jsonl_messages(raw.splitlines())
            return messages, "jsonl", adapter.name

        elif source_type == "markdown" and adapter.name == "markdown_transcript":
            messages = parse_markdown_transcript(raw)
            return messages, source_type, adapter.name

    if source_type == "json":
        return [], "json", "json_unparsed"

    messages = parse_markdown_transcript(raw)
    return messages, source_type, "markdown_transcript"


def split_messages_into_chunks(
    messages: list[dict],
    max_messages: int = 24,
    max_chars: int = 6000,
) -> list[list[dict]]:
    if not messages:
        return []

    chunks: list[list[dict]] = []
    current: list[dict] = []
    current_chars = 0

    for msg in messages:
        content = normalize_text(str(msg.get("content", "")))
        msg_chars = len(content)
        would_overflow = (
            current
            and (
                len(current) >= max_messages
                or current_chars + msg_chars > max_chars
            )
        )
        if would_overflow:
            chunks.append(current)
            current = []
            current_chars = 0

        current.append(msg)
        current_chars += msg_chars

    if current:
        chunks.append(current)

    return chunks


def extract_keywords(text: str, limit: int = 8) -> list[str]:
    tokens = re.findall(r"[A-Za-z0-9_./\\:-]+|[\u0E00-\u0E7F]+", text.lower())
    counts: dict[str, int] = {}
    for token in tokens:
        token = token.strip("._-:/\\")
        if not token or len(token) <= 1 or token in STOPWORDS:
            continue
        if token.isdigit():
            continue
        counts[token] = counts.get(token, 0) + 1
    ordered = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    return [word for word, _ in ordered[:limit]]


def role_counts_for_chunk(chunk: list[dict]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for msg in chunk:
        role = str(msg.get("role", "")).strip().lower() or "unknown"
        counts[role] = counts.get(role, 0) + 1
    return counts


def build_chunk_text(chunk: list[dict]) -> str:
    lines: list[str] = []
    for msg in chunk:
        role = str(msg.get("role", "")).strip().lower() or "unknown"
        content = normalize_text(str(msg.get("content", "")))
        if not content:
            continue
        lines.append(f"{role}: {content}")
    return "\n".join(lines)


def pick_significant_lines(chunk_text: str, limit: int = 3) -> list[str]:
    lines = [compact_spaces(line) for line in chunk_text.splitlines() if compact_spaces(line)]
    if not lines:
        return []

    picked: list[str] = []
    for line in lines:
        if DECISION_RE.search(line):
            picked.append(line)
        if len(picked) >= limit:
            break

    if not picked:
        picked.append(lines[0])
        if len(lines) > 1:
            picked.append(lines[-1])
    elif len(picked) < min(limit, len(lines)):
        tail = lines[-1]
        if tail not in picked:
            picked.append(tail)

    return picked[:limit]


def summarize_chunk_heuristic(
    chunk: list[dict],
    chunk_text: str,
    title: str,
) -> tuple[str, list[str]]:
    role_counts = role_counts_for_chunk(chunk)
    keywords = extract_keywords(chunk_text, limit=8)
    signals = pick_significant_lines(chunk_text, limit=3)

    summary_lines = [
        f"title: {title}",
        f"messages: {len(chunk)}",
    ]
    if role_counts:
        role_bits = ", ".join(f"{role}={count}" for role, count in sorted(role_counts.items()))
        summary_lines.append(f"roles: {role_bits}")
    if keywords:
        summary_lines.append(f"keywords: {', '.join(keywords)}")
    if signals:
        summary_lines.append("highlights:")
        summary_lines.extend(f"- {line}" for line in signals)

    return "\n".join(summary_lines).strip(), keywords


def build_llm_summary_prompt(
    chunk: list[dict],
    title: str,
    max_bullets: int = 4,
) -> str:
    lines = [
        "Summarize this chat chunk for long-term memory.",
        f"Title: {title}",
        f"Return at most {max_bullets} short bullets.",
        "Focus on decisions, tasks, important file names, paths, and unresolved questions.",
        "Chunk:",
    ]
    for msg in chunk:
        role = str(msg.get("role", "")).strip().lower() or "unknown"
        content = normalize_text(str(msg.get("content", "")))
        if not content:
            continue
        lines.append(f"{role}: {content}")
    return "\n".join(lines)


def summarize_chunk_with_command(
    chunk: list[dict],
    title: str,
    summary_cmd: str,
) -> tuple[str, list[str]]:
    prompt = build_llm_summary_prompt(chunk, title=title)
    completed = subprocess.run(
        summary_cmd,
        input=prompt,
        text=True,
        capture_output=True,
        shell=True,
    )
    stdout = normalize_text(completed.stdout or "")
    stderr = normalize_text(completed.stderr or "")
    if completed.returncode != 0 or not stdout:
        fallback, keywords = summarize_chunk_heuristic(chunk, build_chunk_text(chunk), title)
        if stderr:
            fallback = fallback + "\n\nllm_cmd_error: " + stderr
        return fallback, keywords
    return stdout, extract_keywords(build_chunk_text(chunk), limit=8)


def summarize_chunk(
    chunk: list[dict],
    title: str,
    summary_mode: str = "heuristic",
    summary_cmd: str = "",
) -> tuple[str, list[str]]:
    chunk_text = build_chunk_text(chunk)
    if summary_mode == "command" and summary_cmd.strip():
        return summarize_chunk_with_command(chunk, title=title, summary_cmd=summary_cmd.strip())
    return summarize_chunk_heuristic(chunk, chunk_text, title)


def extract_messages_from_path(path: Path) -> tuple[list[dict], str]:
    raw = path.read_text(encoding="utf-8", errors="ignore")
    messages, source_type, _parser_name = parse_export_payload(path, raw)
    return messages, source_type


def preview_export_path(path: Path, sample_limit: int = 3) -> dict:
    raw = path.read_text(encoding="utf-8", errors="ignore")
    messages, source_type, parser_name = parse_export_payload(path, raw)
    samples = [
        {
            "role": msg.get("role", ""),
            "content": truncate_text(str(msg.get("content", "")), 180),
            "ts": str(msg.get("ts") or ""),
        }
        for msg in messages[:sample_limit]
    ]
    return {
        "path": str(path),
        "source_type": source_type,
        "parser": parser_name,
        "message_count": len(messages),
        "sample": samples,
    }


def make_conversation_id(source_path: Path, title: str, messages: list[dict]) -> str:
    fingerprint = stable_hash(
        str(source_path.resolve()),
        title,
        str(len(messages)),
        *(f"{m.get('role','')}::{m.get('content','')[:128]}" for m in messages[:16]),
    )
    return fingerprint[:24]


def build_records(
    source_path: Path,
    project: str,
    source_name: Optional[str] = None,
) -> tuple[list[MessageRecord], Optional[ConversationRecord], str]:
    messages, inferred_type = extract_messages_from_path(source_path)
    source_name = source_name or source_path.stem
    title = source_path.stem.replace("_", " ").replace("-", " ").strip()

    if not messages:
        return [], None, inferred_type

    conversation_id = make_conversation_id(source_path, title, messages)
    records: list[MessageRecord] = []
    roles: list[str] = []
    first_ts = ""
    last_ts = ""

    for idx, msg in enumerate(messages):
        role = str(msg.get("role", "")).strip().lower()
        content = normalize_text(str(msg.get("content", "")))
        if not role or not content:
            continue
        if role not in roles:
            roles.append(role)
        ts = msg.get("ts")
        ts_str = str(ts) if ts else ""
        if not first_ts and ts_str:
            first_ts = ts_str
        if ts_str:
            last_ts = ts_str
        message_id = stable_hash(conversation_id, role, content, str(idx))[:24]
        records.append(
            MessageRecord(
                conversation_id=conversation_id,
                message_id=message_id,
                turn_index=idx,
                role=role,
                content=content,
                source_path=str(source_path),
                source_type=inferred_type,
                source_name=source_name,
                project=project,
                title=title,
                ts=ts_str,
                content_hash=stable_hash(role, content),
            )
        )

    if not records:
        return [], None, inferred_type

    conv = ConversationRecord(
        conversation_id=conversation_id,
        title=title,
        source_path=str(source_path),
        source_type=inferred_type,
        source_name=source_name,
        project=project,
        message_count=len(records),
        roles=roles,
        first_ts=first_ts,
        last_ts=last_ts,
        content_hash=stable_hash(*(r.content_hash for r in records)),
        chunk_count=0,
        summary="",
        keywords=[],
        chunk_ids=[],
    )
    return records, conv, inferred_type


def write_jsonl(path: Path, rows: Iterable[dict], append: bool = True) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = "a" if append else "w"
    with path.open(mode, encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            f.write("\n")


def copy_raw_source(source_path: Path, raw_dir: Path) -> Path:
    raw_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    safe_name = re.sub(r"[^A-Za-z0-9._-]+", "_", source_path.name)
    dst = raw_dir / f"{ts}_{safe_name}"
    shutil.copy2(source_path, dst)
    return dst


def ingest_path(
    source_path: Path,
    out_dir: Path,
    project: str,
    source_name: Optional[str] = None,
    keep_raw: bool = True,
    chunk_max_messages: int = 24,
    chunk_max_chars: int = 6000,
    summary_mode: str = "heuristic",
    summary_cmd: str = "",
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = out_dir / "raw"
    source_fingerprint = file_fingerprint(source_path)
    records, conv, source_type = build_records(
        source_path,
        project,
        source_name=source_name,
    )

    if keep_raw:
        copied = copy_raw_source(source_path, raw_dir)
    else:
        copied = None

    chunk_rows: list[ChunkRecord] = []
    if records and conv:
        messages_by_chunk = split_messages_into_chunks(
            [dataclasses.asdict(r) for r in records],
            max_messages=chunk_max_messages,
            max_chars=chunk_max_chars,
        )
        chunk_ids: list[str] = []
        chunk_summaries: list[str] = []
        chunk_keywords: list[str] = []

        for chunk_index, chunk in enumerate(messages_by_chunk):
            chunk_text = build_chunk_text(chunk)
            chunk_summary, keywords = summarize_chunk(
                chunk,
                title=f"{conv.title} / chunk {chunk_index + 1}",
                summary_mode=summary_mode,
                summary_cmd=summary_cmd,
            )
            start_turn = int(chunk[0].get("turn_index", chunk_index)) if chunk else chunk_index
            end_turn = int(chunk[-1].get("turn_index", chunk_index)) if chunk else chunk_index
            chunk_id = stable_hash(conv.conversation_id, str(chunk_index), str(start_turn), str(end_turn), chunk_text)[:24]
            role_counts = role_counts_for_chunk(chunk)
            ts_values = [str(m.get("ts") or "") for m in chunk if m.get("ts")]
            ts_first = ts_values[0] if ts_values else ""
            ts_last = ts_values[-1] if ts_values else ""
            chunk_rows.append(
                ChunkRecord(
                    conversation_id=conv.conversation_id,
                    chunk_id=chunk_id,
                    chunk_index=chunk_index,
                    start_turn=start_turn,
                    end_turn=end_turn,
                    message_count=len(chunk),
                    char_count=len(chunk_text),
                    role_counts=role_counts,
                    source_path=str(source_path),
                    source_type=source_type,
                    source_name=source_name or source_path.stem,
                    project=project,
                    title=conv.title,
                    ts_first=ts_first,
                    ts_last=ts_last,
                    content_hash=stable_hash(chunk_text),
                    keywords=keywords,
                    summary=chunk_summary,
                    text=chunk_text,
                )
            )
            chunk_ids.append(chunk_id)
            chunk_summaries.append(chunk_summary)
            chunk_keywords.extend(keywords)

        conv.chunk_count = len(chunk_rows)
        conv.chunk_ids = chunk_ids
        conv.keywords = sorted({kw for kw in chunk_keywords if kw})
        conv.summary = "\n\n".join(chunk_summaries).strip()

    if records:
        write_jsonl(out_dir / "memory.jsonl", (r.to_json() for r in records), append=True)
        write_jsonl(out_dir / "conversations.jsonl", [conv.to_json()] if conv else [], append=True)
        if chunk_rows:
            write_jsonl(out_dir / "chunks.jsonl", (r.to_json() for r in chunk_rows), append=True)

    summary = {
        "source_path": str(source_path),
        "source_name": source_name or source_path.stem,
        "source_type": source_type,
        "project": project,
        "message_count": len(records),
        "chunk_count": len(chunk_rows),
        "conversation_id": conv.conversation_id if conv else None,
        "title": conv.title if conv else source_path.stem,
        "raw_copy": str(copied) if copied else None,
        "source_fingerprint": source_fingerprint,
        "ingested_at": utc_now_iso(),
    }
    return summary


def load_manifest(path: Path) -> dict:
    if not path.exists():
        return {"runs": [], "sources": [], "seen_source_fingerprints": []}
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        manifest = {"runs": [], "sources": [], "seen_source_fingerprints": []}
    manifest.setdefault("runs", [])
    manifest.setdefault("sources", [])
    manifest.setdefault("seen_source_fingerprints", [])
    return manifest


def save_manifest(path: Path, manifest: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")


def ingest_many(
    inputs: list[Path],
    out_dir: Path,
    project: str,
    source_name: Optional[str] = None,
    keep_raw: bool = True,
    chunk_max_messages: int = 24,
    chunk_max_chars: int = 6000,
    summary_mode: str = "heuristic",
    summary_cmd: str = "",
) -> dict:
    manifest_path = out_dir / "manifest.json"
    manifest = load_manifest(manifest_path)
    seen_source_fingerprints = {str(item) for item in manifest.get("seen_source_fingerprints", [])}
    run = {
        "run_id": utc_now_iso(),
        "project": project,
        "input_count": len(inputs),
        "outputs": {
            "memory_jsonl": str(out_dir / "memory.jsonl"),
            "conversations_jsonl": str(out_dir / "conversations.jsonl"),
            "chunks_jsonl": str(out_dir / "chunks.jsonl"),
        },
        "sources": [],
    }

    total_messages = 0
    total_chunks = 0
    for input_path in inputs:
        source_fp = file_fingerprint(input_path)
        if source_fp in seen_source_fingerprints:
            run["sources"].append(
                {
                    "source_path": str(input_path),
                    "source_name": source_name or input_path.stem,
                    "source_type": detect_source_type(input_path),
                    "project": project,
                    "message_count": 0,
                    "chunk_count": 0,
                    "conversation_id": None,
                    "title": input_path.stem,
                    "raw_copy": None,
                    "source_fingerprint": source_fp,
                    "ingested_at": utc_now_iso(),
                    "skipped": True,
                    "skip_reason": "duplicate_source_fingerprint",
                }
            )
            continue
        summary = ingest_path(
            input_path,
            out_dir=out_dir,
            project=project,
            source_name=source_name,
            keep_raw=keep_raw,
            chunk_max_messages=chunk_max_messages,
            chunk_max_chars=chunk_max_chars,
            summary_mode=summary_mode,
            summary_cmd=summary_cmd,
        )
        total_messages += int(summary["message_count"])
        total_chunks += int(summary["chunk_count"])
        run["sources"].append(summary)
        manifest["sources"].append(summary)
        manifest["seen_source_fingerprints"].append(source_fp)
        seen_source_fingerprints.add(source_fp)

    run["total_messages"] = total_messages
    run["total_chunks"] = total_chunks
    run["skipped_sources"] = sum(1 for item in run["sources"] if item.get("skipped"))
    manifest["runs"].append(run)
    save_manifest(manifest_path, manifest)
    return run


def self_test() -> int:
    sample_md = """
User: First thing we did was wire the runner.
Assistant: Then we fixed 64-bit seek for large GGUF files.

User:
Now we need memory ingestion.
Assistant:
Create a canonical JSONL layer first.
""".strip()

    sample_json = {
        "chat_messages": [
            {"sender": "human", "text": "Start memory store.", "created_at": "2026-04-30T00:00:00Z"},
            {"sender": "assistant", "text": "Use a clean JSONL format.", "created_at": "2026-04-30T00:00:01Z"},
        ]
    }

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        md_path = td_path / "sample.md"
        json_path = td_path / "sample.json"
        md_path.write_text(sample_md, encoding="utf-8")
        json_path.write_text(json.dumps(sample_json), encoding="utf-8")

        out_dir = td_path / "store"
        run = ingest_many(
            [md_path, json_path],
            out_dir=out_dir,
            project="self-test",
            keep_raw=False,
            chunk_max_messages=2,
            chunk_max_chars=220,
            summary_mode="heuristic",
        )

        memory_lines = (out_dir / "memory.jsonl").read_text(encoding="utf-8").splitlines()
        conv_lines = (out_dir / "conversations.jsonl").read_text(encoding="utf-8").splitlines()
        chunk_lines = (out_dir / "chunks.jsonl").read_text(encoding="utf-8").splitlines()
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))

        assert run["total_messages"] == 6, run
        assert run["total_chunks"] >= 3, run
        assert len(memory_lines) == 6, memory_lines
        assert len(conv_lines) == 2, conv_lines
        assert len(chunk_lines) >= 3, chunk_lines
        assert manifest["runs"], "manifest missing runs"
        first = json.loads(memory_lines[0])
        assert first["role"] == "user"
        assert "runner" in first["content"] or "wire" in first["content"]
        first_chunk = json.loads(chunk_lines[0])
        assert first_chunk["summary"], first_chunk

        sample_conv = td_path / "sample_conversations.json"
        sample_conv.write_text(json.dumps([sample_json]), encoding="utf-8")
        out_dir2 = td_path / "store2"
        run2 = ingest_many([sample_conv], out_dir=out_dir2, project="self-test", keep_raw=False, chunk_max_messages=2, chunk_max_chars=220, summary_mode="heuristic")
        memory_lines2 = (out_dir2 / "memory.jsonl").read_text(encoding="utf-8").splitlines()
        assert run2["total_messages"] == 2, run2
        assert len(memory_lines2) == 2, memory_lines2

        run3 = ingest_many([sample_conv], out_dir=out_dir2, project="self-test", keep_raw=False, chunk_max_messages=2, chunk_max_chars=220, summary_mode="heuristic")
        memory_lines3 = (out_dir2 / "memory.jsonl").read_text(encoding="utf-8").splitlines()
        manifest2 = json.loads((out_dir2 / "manifest.json").read_text(encoding="utf-8"))
        assert run3["total_messages"] == 0, run3
        assert run3["skipped_sources"] == 1, run3
        assert len(memory_lines3) == 2, memory_lines3
        assert manifest2["seen_source_fingerprints"], manifest2

    print("[self-test] OK")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Ingest exported chat logs into a clean memory store.")
    p.add_argument("inputs", nargs="*", help="Input export files or directories")
    p.add_argument("--out-dir", default=str(Path(__file__).resolve().parent / "memory_store"), help="Output store directory")
    p.add_argument("--project", default="default", help="Project label to attach to imported records")
    p.add_argument("--source-name", default=None, help="Override source name in stored metadata")
    p.add_argument("--no-raw-copy", action="store_true", help="Do not copy raw exports into out_dir/raw")
    p.add_argument("--chunk-messages", type=int, default=24, help="Maximum messages per chunk")
    p.add_argument("--chunk-chars", type=int, default=6000, help="Maximum characters per chunk")
    p.add_argument(
        "--summary-mode",
        choices=("heuristic", "command"),
        default="heuristic",
        help="Summary generation mode",
    )
    p.add_argument(
        "--summary-cmd",
        default="",
        help="Command to generate a summary from stdin when --summary-mode=command",
    )
    p.add_argument("--gui", action="store_true", help="Open a small GUI for selecting input/output paths")
    p.add_argument("--self-test", action="store_true", help="Run a built-in self test and exit")
    return p.parse_args(argv)


def expand_inputs(items: list[str]) -> list[Path]:
    out: list[Path] = []
    seen: set[str] = set()
    for item in items:
        p = Path(item)
        if p.is_dir():
            for child in sorted(p.rglob("*")):
                if child.is_file() and child.suffix.lower() in {".json", ".jsonl", ".ndjson", ".md", ".markdown", ".txt"}:
                    key = str(child.resolve())
                    if key not in seen:
                        seen.add(key)
                        out.append(child)
        elif p.is_file():
            key = str(p.resolve())
            if key not in seen:
                seen.add(key)
                out.append(p)
    return out


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.self_test:
        return self_test()
    if args.gui:
        return launch_gui()

    inputs = expand_inputs(args.inputs)
    if not inputs:
        print("usage: llm_memory_ingest.py <export-file-or-dir> [...]", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    run = ingest_many(
        inputs=inputs,
        out_dir=out_dir,
        project=args.project,
        source_name=args.source_name,
        keep_raw=not args.no_raw_copy,
        chunk_max_messages=args.chunk_messages,
        chunk_max_chars=args.chunk_chars,
        summary_mode=args.summary_mode,
        summary_cmd=args.summary_cmd,
    )

    print(json.dumps(run, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


def launch_gui() -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except Exception as exc:  # pragma: no cover - GUI only
        print(f"tkinter is not available: {exc}", file=sys.stderr)
        return 1

    root = tk.Tk()
    root.title("LLM Memory Ingest")
    root.geometry("900x640")

    log_q: "queue.SimpleQueue[str]" = queue.SimpleQueue()
    inputs: list[Path] = []

    frame = ttk.Frame(root, padding=12)
    frame.pack(fill="both", expand=True)

    top = ttk.Frame(frame)
    top.pack(fill="x")

    path_box = tk.Listbox(top, height=8, selectmode=tk.EXTENDED)
    path_box.pack(side="left", fill="both", expand=True)

    btns = ttk.Frame(top)
    btns.pack(side="left", padx=(10, 0), fill="y")

    def refresh_paths() -> None:
        path_box.delete(0, tk.END)
        for p in inputs:
            path_box.insert(tk.END, str(p))

    def add_files() -> None:
        picked = filedialog.askopenfilenames(
            title="Select exported chat files",
            filetypes=[
                ("Chat exports", "*.json *.jsonl *.ndjson *.md *.markdown *.txt"),
                ("All files", "*.*"),
            ],
        )
        for item in picked:
            p = Path(item)
            if p not in inputs:
                inputs.append(p)
        refresh_paths()

    def add_folder() -> None:
        picked = filedialog.askdirectory(title="Select folder containing exports")
        if picked:
            p = Path(picked)
            if p not in inputs:
                inputs.append(p)
            refresh_paths()

    def remove_selected() -> None:
        selected = list(path_box.curselection())
        for idx in reversed(selected):
            if 0 <= idx < len(inputs):
                inputs.pop(idx)
        refresh_paths()

    def clear_paths() -> None:
        inputs.clear()
        refresh_paths()

    def clear_log() -> None:
        log.configure(state="normal")
        log.delete("1.0", tk.END)
        log.insert("end", "Ready.\n")
        log.configure(state="disabled")

    def write_log(message: str) -> None:
        log_q.put(message.rstrip() + "\n")

    def render_preview_report(items: list[Path]) -> str:
        expanded = expand_inputs([str(p) for p in items])
        if not expanded:
            return "No readable export files found.\n"

        lines: list[str] = []
        lines.append(f"Preview for {len(items)} selected path(s), {len(expanded)} file(s) after expand:\n")
        for idx, p in enumerate(expanded, start=1):
            try:
                info = preview_export_path(p)
                lines.append(f"[{idx}] {info['path']}")
                lines.append(f"    parser={info['parser']}  source_type={info['source_type']}  messages={info['message_count']}")
                if info["sample"]:
                    for sidx, sample in enumerate(info["sample"], start=1):
                        role = sample.get("role", "")
                        content = sample.get("content", "")
                        ts = sample.get("ts", "")
                        suffix = f"  ts={ts}" if ts else ""
                        lines.append(f"    sample[{sidx}] {role}: {content}{suffix}")
                else:
                    lines.append("    sample: <no messages extracted>")
            except Exception as exc:
                lines.append(f"[{idx}] {p}")
                lines.append(f"    ERROR: {exc}")
            lines.append("")
        return "\n".join(lines)

    ttk.Button(btns, text="Add File", command=add_files).pack(fill="x", pady=2)
    ttk.Button(btns, text="Add Folder", command=add_folder).pack(fill="x", pady=2)
    ttk.Button(btns, text="Remove Selected", command=remove_selected).pack(fill="x", pady=2)
    ttk.Button(btns, text="Clear", command=clear_paths).pack(fill="x", pady=2)

    form = ttk.Frame(frame)
    form.pack(fill="x", pady=(12, 8))

    out_dir_var = tk.StringVar(value=str(Path(__file__).resolve().parent / "memory_store"))
    project_var = tk.StringVar(value="default")
    source_name_var = tk.StringVar(value="")
    chunk_messages_var = tk.StringVar(value="24")
    chunk_chars_var = tk.StringVar(value="6000")
    summary_mode_var = tk.StringVar(value="heuristic")
    summary_cmd_var = tk.StringVar(value="")
    keep_raw_var = tk.BooleanVar(value=True)

    def add_labeled_row(parent, label, widget, row, button=None):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        widget.grid(row=row, column=1, sticky="ew", pady=4)
        if button is not None:
            button.grid(row=row, column=2, sticky="ew", padx=(8, 0), pady=4)

    form.columnconfigure(1, weight=1)

    out_entry = ttk.Entry(form, textvariable=out_dir_var)
    add_labeled_row(
        form,
        "Output dir",
        out_entry,
        0,
        ttk.Button(form, text="Browse", command=lambda: out_dir_var.set(filedialog.askdirectory(title="Select output folder") or out_dir_var.get())),
    )
    add_labeled_row(form, "Project", ttk.Entry(form, textvariable=project_var), 1)
    add_labeled_row(form, "Source name", ttk.Entry(form, textvariable=source_name_var), 2)
    add_labeled_row(form, "Chunk messages", ttk.Entry(form, textvariable=chunk_messages_var), 3)
    add_labeled_row(form, "Chunk chars", ttk.Entry(form, textvariable=chunk_chars_var), 4)
    mode_box = ttk.Combobox(form, textvariable=summary_mode_var, values=["heuristic", "command"], state="readonly")
    add_labeled_row(form, "Summary mode", mode_box, 5)
    add_labeled_row(form, "Summary cmd", ttk.Entry(form, textvariable=summary_cmd_var), 6)
    raw_check = ttk.Checkbutton(form, text="Keep raw copy", variable=keep_raw_var)
    raw_check.grid(row=7, column=1, sticky="w", pady=4)

    log = tk.Text(frame, height=12, wrap="word")
    log.pack(fill="both", expand=True, pady=(8, 0))
    log.insert("end", "Ready.\n")
    log.configure(state="disabled")

    def drain_log() -> None:
        try:
            while True:
                msg = log_q.get_nowait()
                log.configure(state="normal")
                log.insert("end", msg)
                log.see("end")
                log.configure(state="disabled")
        except queue.Empty:
            pass
        root.after(100, drain_log)

    def preview_job() -> None:
        if not inputs:
            messagebox.showerror("LLM Memory Ingest", "Choose at least one input file or folder.")
            return
        clear_log()
        try:
            report = render_preview_report(list(inputs))
            log.configure(state="normal")
            log.insert("end", report)
            log.see("end")
            log.configure(state="disabled")
        except Exception as exc:  # pragma: no cover - GUI only
            log.configure(state="normal")
            log.insert("end", f"ERROR: {exc}\n")
            log.configure(state="disabled")

    def run_job() -> None:
        if not inputs:
            messagebox.showerror("LLM Memory Ingest", "Choose at least one input file or folder.")
            return
        try:
            chunk_messages = int(chunk_messages_var.get().strip())
            chunk_chars = int(chunk_chars_var.get().strip())
        except ValueError:
            messagebox.showerror("LLM Memory Ingest", "Chunk values must be integers.")
            return

        out_dir = Path(out_dir_var.get().strip())
        project = project_var.get().strip() or "default"
        source_name = source_name_var.get().strip() or None
        summary_mode = summary_mode_var.get().strip() or "heuristic"
        summary_cmd = summary_cmd_var.get().strip()
        keep_raw = bool(keep_raw_var.get())
        selected_inputs = list(inputs)

        def worker() -> None:
            try:
                write_log(f"Ingesting {len(selected_inputs)} input(s)...")
                run = ingest_many(
                    inputs=expand_inputs([str(p) for p in selected_inputs]),
                    out_dir=out_dir,
                    project=project,
                    source_name=source_name,
                    keep_raw=keep_raw,
                    chunk_max_messages=chunk_messages,
                    chunk_max_chars=chunk_chars,
                    summary_mode=summary_mode,
                    summary_cmd=summary_cmd,
                )
                write_log(json.dumps(run, ensure_ascii=False, indent=2, sort_keys=True))
                write_log("Done.")
            except Exception as exc:  # pragma: no cover - GUI only
                write_log(f"ERROR: {exc}")

        threading.Thread(target=worker, daemon=True).start()

    buttons = ttk.Frame(frame)
    buttons.pack(anchor="e", pady=(10, 0), fill="x")
    ttk.Button(buttons, text="Preview", command=preview_job).pack(side="left")
    ttk.Button(buttons, text="Clear Log", command=clear_log).pack(side="left", padx=(8, 0))
    ttk.Button(buttons, text="Run Ingest", command=run_job).pack(side="right")
    root.after(100, drain_log)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
