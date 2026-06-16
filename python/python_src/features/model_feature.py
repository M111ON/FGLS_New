from __future__ import annotations
import sys, os, io, logging, time, tempfile, struct
from pathlib import Path
from typing import Optional
from fastapi import APIRouter, HTTPException, UploadFile, File, Form
from fastapi.responses import Response
from pydantic import BaseModel
from .base import EngineFeature

logger = logging.getLogger("engine.features.model")

_HERE = Path(__file__).resolve().parent
_PYTHON = _HERE.parent.parent
_COLLECTION = _HERE.parent.parent.parent / "collection"
for p in [str(_HERE.parent), str(_COLLECTION)]:
    if p not in sys.path:
        sys.path.insert(0, p)

_MODEL_DIR = Path("I:/model")

# lazy-loaded model instances
_whisper_model = None
_whisper_info = None
_kokoro_pipeline = None
_vlm_model = None
_vlm_processor = None


def _load_whisper():
    global _whisper_model, _whisper_info
    if _whisper_model is not None:
        return
    try:
        from faster_whisper import WhisperModel
        logger.info("Loading faster-whisper (base)...")
        t0 = time.time()
        _whisper_model = WhisperModel("base", device="cpu", compute_type="int8")
        _whisper_info = {"model": "base", "device": "cpu", "load_ms": int((time.time()-t0)*1000)}
        logger.info(f"Whisper loaded in {_whisper_info['load_ms']}ms")
    except Exception as e:
        logger.warning(f"Whisper load failed: {e}")
        raise


def _load_kokoro():
    global _kokoro_pipeline
    if _kokoro_pipeline is not None:
        return
    try:
        from pykokoro import PipelineConfig, build_pipeline
        logger.info("Loading Kokoro TTS...")
        t0 = time.time()
        cfg = PipelineConfig(voice="af_heart")
        _kokoro_pipeline = build_pipeline(config=cfg)
        logger.info(f"Kokoro loaded in {int((time.time()-t0)*1000)}ms")
    except Exception as e:
        logger.warning(f"Kokoro load failed: {e}")
        raise


def _load_vlm():
    global _vlm_model, _vlm_processor
    if _vlm_model is not None:
        return
    try:
        from transformers import AutoProcessor, AutoModelForVision2Seq
        logger.info("Loading SmolVLM-256M...")
        t0 = time.time()
        model_id = "HuggingFaceTB/SmolVLM-256M-Instruct"
        _vlm_processor = AutoProcessor.from_pretrained(model_id)
        import torch
        _vlm_model = AutoModelForVision2Seq.from_pretrained(
            model_id, torch_dtype=torch.float32, device_map=None
        ).to("cpu")
        _vlm_model.eval()
        logger.info(f"SmolVLM loaded in {int((time.time()-t0)*1000)}ms")
    except Exception as e:
        logger.warning(f"SmolVLM load failed: {e}")
        raise


class TTSRequest(BaseModel):
    text: str
    voice: str = "af_heart"
    speed: float = 1.0


class ModelFeature(EngineFeature):
    name = "AI Models"
    description = "STT (Whisper) · TTS (Kokoro) · VLM (SmolVLM)"
    icon = "model"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/models", tags=["models"])

        @router.get("/status")
        def model_status():
            return {
                "whisper": "loaded" if _whisper_model is not None else "unloaded",
                "kokoro": "loaded" if _kokoro_pipeline is not None else "unloaded",
                "vlm": "loaded" if _vlm_model is not None else "unloaded",
                "whisper_info": _whisper_info,
            }

        @router.post("/stt")
        async def stt_transcribe(file: UploadFile = File(...)):
            try:
                _load_whisper()
            except Exception as e:
                raise HTTPException(503, f"Whisper unavailable: {e}")

            ext = Path(file.filename or "audio.wav").suffix.lower()
            if ext not in (".wav", ".mp3", ".m4a", ".ogg", ".flac"):
                raise HTTPException(400, f"Unsupported format: {ext}")

            data = await file.read()
            tmp = tempfile.NamedTemporaryFile(suffix=ext, delete=False)
            try:
                tmp.write(data)
                tmp.close()
                t0 = time.time()
                segments, info = _whisper_model.transcribe(tmp.name, beam_size=5)
                text = "".join(seg.text for seg in segments)
                elapsed = int((time.time()-t0)*1000)
            finally:
                os.unlink(tmp.name)

            return {
                "text": text.strip(),
                "duration_ms": elapsed,
                "language": info.language,
                "probability": round(info.language_probability, 3),
            }

        @router.post("/tts")
        async def tts_speak(req: TTSRequest):
            try:
                _load_kokoro()
            except Exception as e:
                raise HTTPException(503, f"Kokoro unavailable: {e}")

            if not req.text.strip():
                raise HTTPException(400, "text is required")

            from pykokoro import GenerationConfig
            t0 = time.time()
            audio = _kokoro_pipeline(req.text, voice=req.voice, generation=GenerationConfig(speed=req.speed))
            elapsed = int((time.time()-t0)*1000)

            buf = io.BytesIO()
            import soundfile as sf
            sf.write(buf, audio.audio, audio.sample_rate, format="WAV")
            wav_bytes = buf.getvalue()

            return Response(
                content=wav_bytes,
                media_type="audio/wav",
                headers={
                    "X-Duration-Ms": str(elapsed),
                    "X-Sample-Rate": str(audio.sample_rate),
                },
            )

        @router.post("/vlm")
        async def vlm_describe(
            file: UploadFile = File(...),
            prompt: str = Form("Describe this image in detail."),
        ):
            try:
                _load_vlm()
            except Exception as e:
                raise HTTPException(503, f"SmolVLM unavailable: {e}")

            ext = Path(file.filename or "image.jpg").suffix.lower()
            if ext not in (".jpg", ".jpeg", ".png", ".webp", ".bmp"):
                raise HTTPException(400, f"Unsupported format: {ext}")

            data = await file.read()
            from PIL import Image
            try:
                image = Image.open(io.BytesIO(data))
            except Exception:
                raise HTTPException(400, "Invalid image file")

            t0 = time.time()
            messages = [
                {
                    "role": "user",
                    "content": [
                        {"type": "image", "image": image},
                        {"type": "text", "text": prompt},
                    ],
                },
            ]
            prompt_text = _vlm_processor.apply_chat_template(messages, add_generation_prompt=True)
            inputs = _vlm_processor(images=image, text=prompt_text, return_tensors="pt")
            gen = _vlm_model.generate(**inputs, max_new_tokens=256)
            desc = _vlm_processor.decode(gen[0], skip_special_tokens=True)
            elapsed = int((time.time()-t0)*1000)

            # Strip the input prompt from output
            if prompt in desc:
                desc = desc.split(prompt)[-1].strip()
            desc = desc.strip()

            return {
                "description": desc,
                "duration_ms": elapsed,
                "prompt": prompt,
            }

        app.include_router(router)
