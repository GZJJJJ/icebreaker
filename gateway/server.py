"""Icebreaker Gateway: device WebSocket protocol and mock providers."""

import argparse
import asyncio
import base64
import binascii
import json
import logging
import os
import time
from pathlib import Path
from dataclasses import dataclass, field
from typing import Any

from websockets.asyncio.server import ServerConnection, serve

from providers import SiliconFlowAsrProvider, SiliconFlowLlmProvider, synthesize_with_fallback

LOG = logging.getLogger("icebreaker.gateway")
REPLY_TIMEOUT_SECONDS = 120
MAX_CAPTURE_BYTES = 16000 * 2 * 30
MAX_TTS_BYTES = 900 * 1024


def load_api_key() -> str:
    key = os.getenv("SILICONFLOW_API_KEY")
    if key:
        return key
    api_file = Path(__file__).resolve().parent.parent / "API.txt"
    if not api_file.exists():
        return ""
    for line in api_file.read_text(encoding="utf-8").splitlines():
        if "siliconflow" in line.lower():
            for separator in ("：", ":"):
                if separator in line:
                    return line.split(separator, 1)[1].strip()
    return ""


def encode_event(event: str, **payload: Any) -> str:
    return json.dumps({"event": event, **payload}, ensure_ascii=False)


@dataclass
class Session:
    state: str = "connected"
    audio: bytearray = field(default_factory=bytearray)
    reply_task: asyncio.Task | None = None
    mock: bool = True
    turn: int = 0


async def send_json(ws: ServerConnection, event: str, **payload: Any) -> None:
    await ws.send(encode_event(event, **payload))


async def cancel_reply(session: Session) -> int:
    started = time.perf_counter()
    if session.reply_task is not None:
        session.reply_task.cancel()
        await asyncio.gather(session.reply_task, return_exceptions=True)
        session.reply_task = None
    session.audio.clear()
    return round((time.perf_counter() - started) * 1000)


async def run_reply(ws: ServerConnection, session: Session, pcm: bytes) -> None:
    started = time.perf_counter()
    stage_started = started
    timings = {}
    outcome = "ok"
    audio_bytes = 0
    stage = "asr"

    async def progress(name):
        nonlocal stage, stage_started
        stage = name
        stage_started = time.perf_counter()
        await send_json(ws, "reply.stage", stage=name,
                        elapsed_ms=round((stage_started - started) * 1000))

    try:
        async with asyncio.timeout(REPLY_TIMEOUT_SECONDS):
            await send_json(ws, "session.state", state="thinking")
            await progress("asr")
            if session.mock:
                recognized = "这是一次 Mock 语音输入。"
            else:
                recognized = await SiliconFlowAsrProvider(
                    load_api_key(),
                    os.getenv("SILICONFLOW_ASR_MODEL", "FunAudioLLM/SenseVoiceSmall"),
                ).transcribe(pcm)
            timings["asr"] = round((time.perf_counter() - stage_started) * 1000)
            await send_json(ws, "asr.final", text=recognized)
            if not recognized.strip():
                outcome = "no_speech"
                await send_json(ws, "error", code="no_speech")
                return

            await progress("llm")
            if session.mock:
                response = "欢迎来到破冰小子。今天想聊聊什么主题？"
                timings["llm_first"] = round((time.perf_counter() - stage_started) * 1000)
                await send_json(ws, "llm.delta", text=response)
            else:
                response = ""
                async for delta in SiliconFlowLlmProvider(
                    load_api_key(), os.getenv("SILICONFLOW_MODEL", "deepseek-ai/DeepSeek-R1-0528-Qwen3-8B")
                ).reply(recognized):
                    if delta.strip() and "llm_first" not in timings:
                        timings["llm_first"] = round((time.perf_counter() - stage_started) * 1000)
                    response += delta
                    await send_json(ws, "llm.delta", text=delta)
            timings["llm"] = round((time.perf_counter() - stage_started) * 1000)
            if not response.strip():
                outcome = "empty_reply"
                await send_json(ws, "error", code="empty_reply")
                return

            await progress("tts")
            if session.mock:
                await asyncio.sleep(0.05)
                audio = bytes(3200 * 3)
            else:
                audio = await synthesize_with_fallback(response)
            timings["tts"] = round((time.perf_counter() - stage_started) * 1000)
            if not audio or len(audio) % 2 or len(audio) > MAX_TTS_BYTES:
                raise ValueError("Invalid PCM16 audio")
            audio_bytes = len(audio)
            await progress("sending")
            await send_json(ws, "tts.start", sample_rate=24000, format="pcm_s16le")
            for index, offset in enumerate(range(0, len(audio), 3200)):
                await send_json(
                    ws, "tts.audio", index=index,
                    audio=base64.b64encode(audio[offset:offset + 3200]).decode("ascii"),
                    sample_rate=24000, format="pcm_s16le",
                )
                await asyncio.sleep(0)  # Allow control messages to cancel a long reply.
            await send_json(ws, "tts.end")
            timings["send"] = round((time.perf_counter() - stage_started) * 1000)
    except asyncio.CancelledError:
        outcome = "cancelled"
        raise
    except TimeoutError:
        outcome = "reply_timeout"
        await send_json(ws, "error", code="reply_timeout")
    except Exception as exc:
        # Provider exception strings can contain request details. Never log them.
        LOG.warning("%s failed (%s)", stage, type(exc).__name__)
        outcome = f"{stage}_failed"
        await send_json(ws, "error", code=f"{stage}_failed")
    finally:
        timings.setdefault(stage if stage != "sending" else "send",
                           round((time.perf_counter() - stage_started) * 1000))
        timings["total"] = round((time.perf_counter() - started) * 1000)
        LOG.info("reply connection=%x turn=%d outcome=%s timings_ms=%s audio_bytes=%d",
                 id(session), session.turn, outcome, json.dumps(timings), audio_bytes)
        if not asyncio.current_task().cancelling():
            await send_json(ws, "reply.metrics", outcome=outcome, timings_ms=timings,
                            audio_bytes=audio_bytes)
            session.state = "idle"
            await send_json(ws, "session.state", state="idle")


async def append_audio(ws: ServerConnection, session: Session, pcm: bytes) -> None:
    if session.state != "listening":
        await send_json(ws, "error", code="invalid_state", expected="listening")
    elif len(session.audio) + len(pcm) > MAX_CAPTURE_BYTES:
        session.audio.clear()
        session.state = "idle"
        await send_json(ws, "error", code="audio_too_long")
        await send_json(ws, "session.state", state="idle")
    else:
        session.audio.extend(pcm)


async def handle_message(ws: ServerConnection, session: Session, message: str | bytes) -> None:
    if isinstance(message, bytes):
        await append_audio(ws, session, message)
        return

    try:
        request = json.loads(message)
    except json.JSONDecodeError:
        await send_json(ws, "error", code="invalid_json")
        return

    if not isinstance(request, dict) or not isinstance(request.get("event"), str):
        await send_json(ws, "error", code="invalid_message")
        return

    event = request.get("event")
    if event == "session.start":
        cancel_ms = await cancel_reply(session)
        session.turn += 1
        session.state = "listening"
        LOG.info("turn.start connection=%x turn=%d cancel_ms=%d", id(session), session.turn, cancel_ms)
        await send_json(ws, "session.state", state=session.state,
                        session_id=request.get("session_id"), cancel_ms=cancel_ms)
    elif event == "audio.input":
        try:
            pcm = base64.b64decode(request.get("audio", ""), validate=True)
        except (binascii.Error, ValueError, TypeError):
            await send_json(ws, "error", code="invalid_audio")
        else:
            await append_audio(ws, session, pcm)
    elif event == "audio.end":
        if session.state != "listening":
            await send_json(ws, "error", code="invalid_state", expected="listening")
        elif not session.audio:
            await send_json(ws, "error", code="empty_audio")
            session.state = "idle"
            await send_json(ws, "session.state", state="idle")
        elif len(session.audio) % 2:
            session.audio.clear()
            await send_json(ws, "error", code="invalid_audio")
            session.state = "idle"
            await send_json(ws, "session.state", state="idle")
        else:
            pcm = bytes(session.audio)
            session.audio.clear()
            session.state = "thinking"
            session.reply_task = asyncio.create_task(run_reply(ws, session, pcm))
    elif event == "session.interrupt":
        cancel_ms = await cancel_reply(session)
        session.state = "interrupted"
        LOG.info("turn.interrupt connection=%x turn=%d cancel_ms=%d", id(session), session.turn, cancel_ms)
        await send_json(ws, "session.state", state=session.state, cancel_ms=cancel_ms)
    elif event == "session.end":
        await cancel_reply(session)
        session.state = "ended"
        await send_json(ws, "session.state", state=session.state)
    else:
        await send_json(ws, "error", code="unknown_event", received_event=event)


async def connection_handler(ws: ServerConnection) -> None:
    session = Session()
    session.mock = os.getenv("ICEBREAKER_MOCK", "1").lower() not in {"0", "false", "no"}
    await send_json(ws, "gateway.ready", protocol=1, mock=session.mock)
    try:
        async for message in ws:
            await handle_message(ws, session, message)
    finally:
        await cancel_reply(session)


async def run(host: str, port: int) -> None:
    async with serve(connection_handler, host, port):
        LOG.info("Gateway listening on ws://%s:%d", host, port)
        await asyncio.Future()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(run(args.host, args.port))


if __name__ == "__main__":
    main()
