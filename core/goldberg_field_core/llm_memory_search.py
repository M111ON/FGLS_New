#!/usr/bin/env python3
"""
llm_memory_search.py

Lightweight retrieval for the local memory store.

This script ranks memory chunks from `memory_store/chunks.jsonl` against a
free-text query and prints a compact prompt block that can be injected into a
local LLM prompt.

The scorer is intentionally simple:
  - token overlap against title / keywords / summary / text
  - small bonuses for exact phrase hits
  - recency tie-breaker when available

No embeddings, no model calls, no external dependencies.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


TOKEN_RE = re.compile(r"[A-Za-z0-9_]+|[\u0E00-\u0E7F]+")


@dataclass
class MemoryChunk:
    chunk_id: str
    conversation_id: str
    title: str
    summary: str
    keywords: list[str]
    text: str
    ts_first: str
    ts_last: str
    score: float = 0.0


def normalize(text: str) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\x00", "")
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def tokenize(text: str) -> list[str]:
    return [tok.lower() for tok in TOKEN_RE.findall(text.lower()) if tok.strip()]


def load_query() -> tuple[str, Path, int, int, str]:
    parser = argparse.ArgumentParser()
    parser.add_argument("--query", default="", help="Free-text query to search memory with.")
    parser.add_argument("--query-file", default="", help="Read the query text from a UTF-8 file.")
    parser.add_argument("--store", default="", help="Path to memory_store directory.")
    parser.add_argument("--top", type=int, default=4, help="Maximum number of chunks to return.")
    parser.add_argument("--max-preview", type=int, default=360, help="Preview length per result.")
    parser.add_argument("--format", choices=("text", "json"), default="text", help="Output format.")
    args = parser.parse_args()

    query = args.query.strip()
    if not query and args.query_file.strip():
        query = Path(args.query_file).expanduser().read_text(encoding="utf-8").strip()
    if not query:
        query = os.environ.get("POGLS_MEMORY_QUERY", "").strip()
    if not query and not sys.stdin.isatty():
        query = sys.stdin.read().strip()

    store_raw = (args.store or os.environ.get("POGLS_MEMORY_STORE", "")).strip()
    if store_raw:
        store_dir = Path(store_raw).expanduser()
    else:
        store_dir = Path(__file__).resolve().parent / "memory_store"

    return query, store_dir, args.top, args.max_preview, args.format


def iter_chunks(path: Path) -> Iterable[MemoryChunk]:
    if not path.exists():
        return

    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue

            keywords = row.get("keywords") or []
            if not isinstance(keywords, list):
                keywords = []

            yield MemoryChunk(
                chunk_id=str(row.get("chunk_id") or ""),
                conversation_id=str(row.get("conversation_id") or ""),
                title=str(row.get("title") or ""),
                summary=str(row.get("summary") or ""),
                keywords=[str(k) for k in keywords if k is not None],
                text=str(row.get("text") or ""),
                ts_first=str(row.get("ts_first") or ""),
                ts_last=str(row.get("ts_last") or ""),
            )


def score_chunk(chunk: MemoryChunk, query: str, query_tokens: list[str]) -> float:
    title = chunk.title.lower()
    summary = chunk.summary.lower()
    text = chunk.text.lower()
    kw_text = " ".join(chunk.keywords).lower()
    title_tokens = set(tokenize(chunk.title))
    summary_tokens = set(tokenize(chunk.summary))
    text_tokens = set(tokenize(chunk.text))
    keyword_tokens = set(tokenize(kw_text))

    score = 0.0

    for tok in query_tokens:
        if len(tok) < 2:
            continue
        if tok in title_tokens:
            score += 6.0
        if tok in keyword_tokens:
            score += 4.5
        if tok in summary_tokens:
            score += 3.0
        if tok in text_tokens:
            score += 1.5
        if tok in title:
            score += 1.0
        if tok in summary:
            score += 0.8
        if tok in text:
            score += 0.2

    query_norm = normalize(query).lower()
    if query_norm and query_norm in title:
        score += 8.0
    if query_norm and query_norm in summary:
        score += 4.0
    if query_norm and query_norm in text:
        score += 1.0

    # Small bias toward denser, more recent chunks.
    score += min(len(chunk.summary) / 500.0, 1.0) * 0.5
    score += min(len(chunk.text) / 4000.0, 1.0) * 0.25
    if chunk.ts_last:
        score += 0.1

    return score


def pick_preview(text: str, limit: int) -> str:
    text = normalize(text)
    if len(text) <= limit:
        return text
    return text[: max(0, limit - 1)].rstrip() + "…"


def format_text(query: str, chunks: list[MemoryChunk], top: int, max_preview: int) -> str:
    lines = []
    lines.append("MEMORY_CONTEXT")
    lines.append(f"query: {normalize(query)}")
    lines.append(f"hits: {len(chunks)} of {top}")
    if not chunks:
        lines.append("no matching memory chunks found.")
        return "\n".join(lines)

    for i, chunk in enumerate(chunks, 1):
        lines.append("")
        lines.append(f"{i}. score={chunk.score:.2f} | title={chunk.title or '(untitled)'}")
        if chunk.conversation_id:
            lines.append(f"   conversation_id={chunk.conversation_id}")
        if chunk.chunk_id:
            lines.append(f"   chunk_id={chunk.chunk_id}")
        if chunk.keywords:
            lines.append(f"   keywords={', '.join(chunk.keywords[:12])}")
        if chunk.ts_first or chunk.ts_last:
            lines.append(f"   ts={chunk.ts_first or '?'} .. {chunk.ts_last or '?'}")
        if chunk.summary:
            lines.append("   summary=" + pick_preview(chunk.summary, max_preview))
        if chunk.text:
            lines.append("   preview=" + pick_preview(chunk.text, max_preview))
    return "\n".join(lines)


def main() -> int:
    query, store_dir, top, max_preview, out_format = load_query()
    if top <= 0:
        top = 4
    if max_preview <= 0:
        max_preview = 360

    chunks_path = store_dir / "chunks.jsonl"
    query_tokens = tokenize(query)

    scored: list[MemoryChunk] = []
    for chunk in iter_chunks(chunks_path) or []:
        chunk.score = score_chunk(chunk, query, query_tokens)
        if chunk.score > 0:
            scored.append(chunk)

    scored.sort(
        key=lambda c: (
            c.score,
            c.ts_last or c.ts_first or "",
            len(c.summary) + len(c.text),
        ),
        reverse=True,
    )
    scored = scored[:top]

    if out_format == "json":
        print(json.dumps(
            {
                "query": query,
                "store": str(store_dir),
                "hits": [
                    {
                        "chunk_id": c.chunk_id,
                        "conversation_id": c.conversation_id,
                        "title": c.title,
                        "summary": c.summary,
                        "keywords": c.keywords,
                        "text_preview": pick_preview(c.text, max_preview),
                        "score": c.score,
                        "ts_first": c.ts_first,
                        "ts_last": c.ts_last,
                    }
                    for c in scored
                ],
            },
            ensure_ascii=False,
            indent=2,
        ))
        return 0

    print(format_text(query, scored, top, max_preview))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
