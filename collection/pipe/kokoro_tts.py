#!/usr/bin/env python3
"""
kokoro_tts.py — Real Kokoro-82M TTS inference.

Bridges the FGLS pipe system to the Kokoro Python package.
Reads optional --voice / --text, generates 24kHz mono WAV.

Usage:
  python kokoro_tts.py --text "Hello world" --out hello.wav
  python kokoro_tts.py --text "..." --out out.wav --voice af_heart --speed 1.0
"""
import argparse
import sys
import numpy as np
import soundfile as sf
from kokoro import KPipeline


def main():
    ap = argparse.ArgumentParser(description="Kokoro TTS (FGLS bridge)")
    ap.add_argument("--text", required=True, help="Text to synthesize")
    ap.add_argument("--out", required=True, help="Output WAV path")
    ap.add_argument("--voice", default="af_heart", help="Voice id (af_heart, am_adam, bf_emma, ...)")
    ap.add_argument("--lang", default="a", help="Language code (a=en-us, b=en-br, ...)") 
    ap.add_argument("--speed", type=float, default=1.0, help="Speech speed")
    ap.add_argument("--repo", default="hexgrad/Kokoro-82M", help="Model repo id")
    args = ap.parse_args()

    try:
        pipeline = KPipeline(lang_code=args.lang, repo_id=args.repo)
    except Exception as e:
        # Fallback to default repo
        pipeline = KPipeline(lang_code=args.lang)

    all_audio = []
    for gs, ps, audio in pipeline(args.text, voice=args.voice, speed=args.speed):
        all_audio.append(audio)

    if not all_audio:
        print("ERROR: no audio generated", file=sys.stderr)
        return 1

    out = np.concatenate(all_audio, axis=0)
    sf.write(args.out, out, 24000)
    print(f"Wrote {args.out}: {len(out)} samples ({len(out)/24000:.2f}s) @ 24kHz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
