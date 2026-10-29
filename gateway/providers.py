"""External model adapters used by the Gateway."""

import asyncio
import io
import json
import logging
import os
import tempfile
import time
import wave
from collections.abc import AsyncIterator

import httpx

LOG = logging.getLogger("icebreaker.providers")
SYSTEM_PROMPT = (
    "你是破冰小子，面向主题咖啡馆多人桌的友善社交主持。不要强迫任何人发言。"
    "先准确回应用户当前问题，不要把每个问题都转成咖啡闲聊。"
    "回复用于扬声器直接朗读：通常只说一句，30字以内；不用表情或Markdown。"
    "如果用户要求只给答案，就只给答案，不延伸、不反问。"
)


def pcm_to_wav(pcm: bytes, sample_rate: int = 16000) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return output.getvalue()


class SiliconFlowAsrProvider:
    def __init__(self, api_key: str, model: str = "FunAudioLLM/SenseVoiceSmall"):
        self.api_key = api_key
        self.model = model

    async def transcribe(self, pcm: bytes) -> str:
        if not self.api_key:
            raise RuntimeError("SILICONFLOW_API_KEY is required")
        started = time.perf_counter()
        async with httpx.AsyncClient(timeout=90) as client:
            response = await client.post(
                "https://api.siliconflow.cn/v1/audio/transcriptions",
                headers={"Authorization": f"Bearer {self.api_key}"},
                data={"model": self.model},
                files={"file": ("audio.wav", pcm_to_wav(pcm), "audio/wav")},
            )
            response.raise_for_status()
            LOG.info("asr elapsed_ms=%d input_bytes=%d",
                     round((time.perf_counter() - started) * 1000), len(pcm))
            return response.json()["text"]


class SiliconFlowLlmProvider:
    def __init__(self, api_key: str, model: str):
        self.api_key = api_key
        self.model = model

    async def reply(self, text: str) -> AsyncIterator[str]:
        if not self.api_key or not self.model:
            raise RuntimeError("SILICONFLOW_API_KEY and SILICONFLOW_MODEL are required")
        started = time.perf_counter()
        first_content_ms = None
        first_reasoning_ms = None
        reasoning_chars = 0
        finish_reason = None
        async with httpx.AsyncClient(timeout=90) as client:
            async with client.stream(
                "POST",
                "https://api.siliconflow.cn/v1/chat/completions",
                headers={"Authorization": f"Bearer {self.api_key}"},
                json={
                    "model": self.model,
                    "stream": True,
                    "max_tokens": 160,
                    "messages": [
                        {"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": text},
                    ],
                },
            ) as response:
                response.raise_for_status()
                headers_ms = round((time.perf_counter() - started) * 1000)
                async for line in response.aiter_lines():
                    if not line.startswith("data: ") or line == "data: [DONE]":
                        continue
                    chunk = json.loads(line[6:])
                    choices = chunk.get("choices", [])
                    if not choices:
                        continue
                    choice = choices[0]
                    finish_reason = choice.get("finish_reason") or finish_reason
                    delta = choice.get("delta", {})
                    reasoning = delta.get("reasoning_content")
                    if reasoning:
                        reasoning_chars += len(reasoning)
                        if first_reasoning_ms is None:
                            first_reasoning_ms = round((time.perf_counter() - started) * 1000)
                    content = delta.get("content")
                    if content:
                        if content.strip() and first_content_ms is None:
                            first_content_ms = round((time.perf_counter() - started) * 1000)
                        yield content
        LOG.info("llm headers_ms=%d first_reasoning_ms=%s first_content_ms=%s "
                 "elapsed_ms=%d reasoning_chars=%d finish=%s", headers_ms,
                 first_reasoning_ms, first_content_ms,
                 round((time.perf_counter() - started) * 1000), reasoning_chars, finish_reason)


class EdgeTtsProvider:
    def __init__(self, voice: str = "zh-CN-XiaoxiaoNeural"):
        self.voice = voice

    async def synthesize(self, text: str) -> bytes:
        import edge_tts
        try:
            import imageio_ffmpeg
            ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
        except ImportError:
            ffmpeg = "ffmpeg"

        started = time.perf_counter()
        with tempfile.TemporaryDirectory() as temp_dir:
            mp3_path = os.path.join(temp_dir, "speech.mp3")
            pcm_path = os.path.join(temp_dir, "speech.wav")
            await edge_tts.Communicate(text, self.voice).save(mp3_path)
            downloaded_at = time.perf_counter()
            process = await asyncio.create_subprocess_exec(
                ffmpeg, "-v", "error", "-y", "-i", mp3_path,
                "-ac", "1", "-ar", "24000", "-f", "s16le", pcm_path,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                _, stderr = await process.communicate()
            except asyncio.CancelledError:
                if process.returncode is None:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                await process.communicate()
                raise
            if process.returncode:
                raise RuntimeError(stderr.decode(errors="replace"))
            LOG.info("tts download_ms=%d transcode_ms=%d text_chars=%d",
                     round((downloaded_at - started) * 1000),
                     round((time.perf_counter() - downloaded_at) * 1000), len(text))
            with open(pcm_path, "rb") as audio_file:
                return audio_file.read()


class VolcengineTtsProvider:
    """Placeholder: requires an account-specific Volcengine endpoint and credentials."""

    async def synthesize(self, text: str) -> bytes:
        raise RuntimeError("Volcengine TTS credentials and endpoint are not configured")


async def synthesize_with_fallback(text: str) -> bytes:
    return await EdgeTtsProvider(os.getenv("EDGE_TTS_VOICE", "zh-CN-XiaoxiaoNeural")).synthesize(text)
