"""Opt-in real-provider latency probe; no microphone or device playback required."""

import argparse
import array
import asyncio
import base64
import json
from pathlib import Path
import time

from websockets.asyncio.client import connect

from providers import EdgeTtsProvider


async def benchmark(uri: str, runs: int) -> list[dict]:
    # Use the same deterministic input in every turn; input synthesis is not timed.
    source = await EdgeTtsProvider().synthesize("请只回答一加一等于几。")
    samples = array.array("h", source)
    pcm = array.array("h", (samples[i * 3 // 2] for i in range(len(samples) * 2 // 3))).tobytes()
    results = []
    async with connect(uri) as ws:
        ready = json.loads(await ws.recv())
        if ready.get("mock") is not False:
            raise RuntimeError("Latency benchmark requires real-provider mode")

        async def start(identifier):
            started = time.perf_counter()
            await ws.send(json.dumps({"event": "session.start", "session_id": identifier}))
            event = json.loads(await ws.recv())
            assert event.get("state") == "listening" and event.get("session_id") == identifier
            return round((time.perf_counter() - started) * 1000)

        async def upload():
            for offset in range(0, len(pcm), 3200):
                await ws.send(pcm[offset:offset + 3200])
            await ws.send('{"event":"audio.end"}')

        for index in range(runs):
            for interrupted in (False, True):
                cancel_ms = 0
                if interrupted:
                    await start(f"probe-cancel-{index}")
                    await upload()
                    while json.loads(await ws.recv()).get("state") != "thinking":
                        pass
                    cancelled_at = time.perf_counter()
                    await ws.send('{"event":"session.interrupt"}')
                    while json.loads(await ws.recv()).get("state") != "interrupted":
                        pass
                    cancel_ms = round((time.perf_counter() - cancelled_at) * 1000)
                start_ms = await start(f"probe-{index}-{int(interrupted)}")
                await upload()
                started = time.perf_counter()
                times = {}
                audio_bytes = 0
                chars = 0
                metrics = None
                async with asyncio.timeout(125):
                    while True:
                        event = json.loads(await ws.recv())
                        name = event["event"]
                        now_ms = round((time.perf_counter() - started) * 1000)
                        times.setdefault(name, now_ms)
                        if name == "error":
                            raise RuntimeError(f"Gateway error: {event.get('code')}")
                        if name == "llm.delta":
                            chars += len(event["text"])
                            if event["text"].strip():
                                times.setdefault("llm.content", now_ms)
                                times["llm.last"] = now_ms
                        elif name == "tts.audio":
                            audio_bytes += len(base64.b64decode(event["audio"], validate=True))
                        elif name == "reply.metrics":
                            metrics = event["timings_ms"]
                        if name == "session.state" and event.get("state") == "idle":
                            break
                result = {
                    "run": index + 1, "case": "after_interrupt" if interrupted else "normal",
                    "start_ack_ms": start_ms, "cancel_ack_ms": cancel_ms,
                    "asr_ms": times["asr.final"],
                    "llm_first_ms": times["llm.content"] - times["asr.final"],
                    "llm_ms": times["llm.last"] - times["asr.final"],
                    "tts_ms": times["tts.start"] - times["llm.last"],
                    "receive_ms": times["tts.end"] - times["tts.start"],
                    "total_ms": times["tts.end"], "reply_chars": chars,
                    "audio_seconds": round(audio_bytes / 48000, 2),
                }
                if metrics is not None:
                    result["server_timings_ms"] = metrics
                results.append(result)
                print(json.dumps(result), flush=True)
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--uri", default="ws://127.0.0.1:8765")
    parser.add_argument("--runs", type=int, default=1, help="Pairs of normal / interrupted turns")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = asyncio.run(benchmark(args.uri, args.runs))
    args.output.write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
