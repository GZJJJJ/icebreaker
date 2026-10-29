"""Opt-in board regression via the K0 queue and an offline Gateway on port 8765.

Stop the real Gateway first. The device must use this computer as its Gateway.
Microphone capture is exercised, but fake providers return silent TTS; no cloud calls.
"""

import argparse
import asyncio
import json
import os
from pathlib import Path
import sys
import time

import httpx
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gateway"))
import server as gateway


async def run(url):
    failure = False

    class FakeAsr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            assert pcm and len(pcm) % 2 == 0
            if failure:
                return ""
            return "offline regression"

    class FakeLlm:
        def __init__(self, *args):
            pass

        async def reply(self, text, **kwargs):
            yield "offline reply"

    async def silent_tts(text):
        return bytes(24000 * 2 * 3 // 2)  # 1.5 seconds, long enough to interrupt.

    def no_network(*args, **kwargs):
        raise AssertionError("Unexpected provider HTTP request")

    os.environ["ICEBREAKER_MOCK"] = "0"
    gateway.load_api_key = lambda: "offline-test"
    gateway.SiliconFlowAsrProvider = FakeAsr
    gateway.SiliconFlowLlmProvider = FakeLlm
    gateway.synthesize_with_fallback = silent_tts
    connections = set()
    accepted = 0
    last_button = 0.0

    async def handler(ws):
        nonlocal accepted
        connections.add(ws)
        accepted += 1
        try:
            await gateway.connection_handler(ws)
        except ConnectionClosed:
            pass
        finally:
            connections.discard(ws)

    async with httpx.AsyncClient(base_url=url, timeout=5, trust_env=False) as client:
        # Create the device HTTP client first; block only Provider HTTP afterwards.
        import providers
        providers.httpx.AsyncClient = no_network

        async def get(path):
            response = await client.get(path)
            response.raise_for_status()
            return response.json()

        async def wait_for(path, predicate, timeout=12):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                data = await get(path)
                if predicate(data):
                    return data
                await asyncio.sleep(0.04)
            raise AssertionError(f"Timed out waiting for {path}: {data}")

        async def state(name):
            return await wait_for("/health", lambda data: data["state"] == name)

        async def button(key=0):
            nonlocal last_button
            await asyncio.sleep(max(0, last_button + 0.6 - time.monotonic()))
            count = (await get("/health"))["button_presses"]
            response = await client.post(f"/api/button?key={key}")
            response.raise_for_status()
            if key == 0:
                await wait_for("/health", lambda data: data["button_presses"] == count + 1, 3)
            else:
                await asyncio.sleep(0.15)
            last_button = time.monotonic()

        async def turn(interrupt=False):
            before = await get("/api/stats")
            await button()
            await state("listening")
            assert (await get("/health"))["capture_requested"]
            await button()
            await state("speaking")
            playing = await get("/api/stats")
            assert playing["frames_rx"] > before["frames_rx"]
            assert playing["xio_output"] == 0xA4
            if interrupt:
                await button()
                await state("interrupted")
            else:
                await state("idle")
            await wait_for("/api/stats", lambda data: not data["player_busy"])
            done = await get("/api/stats")
            assert done["xio_output"] == 0x84
            assert done["decode_failures"] == done["play_failures"] == 0
            assert done["last_error"] == ""
            assert set(done["timings_ms"]) == {"asr", "llm_first", "llm", "tts", "send", "total"}
            assert all(value >= 0 for value in done["timings_ms"].values())
            assert done["cancel_ms"] >= 0
            if not interrupt:
                assert done["playback_wait_ms"] > 0
            assert not (await get("/health"))["capture_active"]

        async def reconnect(clean):
            before = accepted
            for ws in list(connections):
                if clean:
                    await ws.close(code=1012, reason="offline reconnect regression")
                else:
                    ws.transport.abort()
            await wait_for("/api/stats", lambda data: not data["connected"], 3)
            await wait_for("/api/stats", lambda data: data["connected"] and accepted > before)
            await state("idle")

        async with serve(handler, "0.0.0.0", 8765):
            await wait_for("/api/stats", lambda data: data["connected"] and accepted > 0, 20)
            await state("idle")
            await turn()
            await turn()  # Second normal turn, through the actual firmware path.
            await turn(interrupt=True)
            await turn()  # Restart after local interruption.
            await reconnect(clean=True)
            await turn()
            await reconnect(clean=False)
            await turn()
            failure = True
            await button()
            await state("listening")
            await button()
            await state("error")
            failed = await get("/api/stats")
            assert failed["last_error"] == "no_speech" and failed["reply_stage"] == "error"
            failure = False
            await turn()  # Diagnostic error is retained until the next turn starts.
            # New product activity path: K1 chooses a theme, K2 joins/ends, K0 still talks.
            await button(1)
            await button(2)
            activity = await wait_for("/api/activity", lambda data: data["state"] == "active")
            assert activity["theme"] == "film"
            assert not activity["camera_enabled"] and not activity["hints_enabled"]
            await turn()
            await wait_for("/api/activity", lambda data: data["history_turns"] == 1)
            await turn(interrupt=True)
            activity = await get("/api/activity")
            assert activity["state"] == "active" and activity["history_turns"] == 2
            response = await client.post("/api/activity", json={"action": "pause"})
            response.raise_for_status()
            await state("paused")
            await button()
            assert not (await get("/health"))["capture_requested"]
            response = await client.post("/api/activity", json={"action": "resume"})
            response.raise_for_status()
            await state("idle")
            await turn()
            await wait_for("/api/activity", lambda data: data["history_turns"] == 3)
            await button(2)
            ended = await wait_for("/api/activity", lambda data: data["state"] == "ended")
            assert ended["history_turns"] == 0
            await button()
            assert not (await get("/health"))["capture_requested"]
            assert (await client.post("/api/activity", json={"action": "start"})).status_code == 400
            await button(2)
            joined = await wait_for("/api/activity", lambda data: data["state"] == "active")
            assert joined["history_turns"] == 0
            await turn()
            await button(2)
            await wait_for("/api/activity", lambda data: data["state"] == "ended")
            stats = await get("/api/stats")
            print(json.dumps({"result": "PASS", "turns": 12, "completed": 9,
                              "interrupted": 2, "reconnects": accepted - 1,
                              "expected_errors": 1,
                              "activity_flow": "join/theme/history/interrupt/pause/resume/end/clear/rejoin",
                              "frames_rx": stats["frames_rx"],
                              "decode_failures": stats["decode_failures"],
                              "play_failures": stats["play_failures"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("url", help="Trusted LAN device URL")
    asyncio.run(run(parser.parse_args().url))
