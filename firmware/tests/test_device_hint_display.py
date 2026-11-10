"""Opt-in text-only board bench. Requires ICEBREAKER_TEXT_UI_TEST=ON firmware.

Run --serve in place of the real Gateway, then --check in another process.
No providers, PCM, TTS or camera commands. Activity IDs are deliberately empty.
The final synthetic suggestion remains visible until K1 dismiss / K2 end.
"""

import argparse
import asyncio
import json
import sys
from pathlib import Path

import httpx
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gateway"))
import server as gateway
from debug_panel import local_request, process_request


SAMPLE = "最近有没有一件让你觉得有趣的小事？"
LONG_SAMPLE = "旅行时你最喜欢怎样认识一座城市？可以聊聊路上的风景、偶遇的小店，或者一次临时改变计划的经历。不必有标准答案，想到什么就分享什么，也可以只是听听大家的故事。"


async def bench_server():
    device = None
    activity = {"state": "none", "theme": "travel", "scene": "social", "hints_enabled": False}
    report = {"ready": False, "binary_messages": 0, "hint_requests": 0, "last_id": 0}

    async def state():
        await gateway.send_json(device, "activity.state", **activity, activity_id="", history_turns=0)

    async def handler(ws):
        nonlocal device
        path = ws.request.path
        if path == "/debug":
            await gateway.DEBUG.handle_viewer(ws)
            return
        if path == "/control":
            async for message in ws:
                request = json.loads(message)
                if request["event"] == "bench.emit":
                    assert request["payload"]["event"] in {"hint.suggestion", "ambient.speech"}
                    await gateway.send_json(device, **request["payload"])
                await ws.send(json.dumps(report))
            return
        if device is not None:
            await ws.close(code=1013, reason="Single-device bench")
            return
        device = ws
        report["ip"] = ws.remote_address[0]
        gateway.DEBUG.register(ws, True)
        try:
            await gateway.send_json(ws, "gateway.ready", protocol=1, mock=True)
            async for message in ws:
                if isinstance(message, bytes):
                    report["binary_messages"] += 1
                    raise AssertionError("Text-only bench received binary audio/image")
                request = json.loads(message)
                event = request["event"]
                if event == "device.ready":
                    assert request.get("text_ui_test") is True, "Refusing to exercise non-bench firmware"
                    report["ready"] = True
                    gateway.DEBUG.incoming(ws, {"event": "device.ready"})
                elif event == "activity.start":
                    activity.update(state="active", theme=request["theme"],
                                    hints_enabled=request["hints_enabled"])
                    await state()
                elif event in {"activity.pause", "activity.resume", "activity.end"}:
                    activity["state"] = {"activity.pause": "paused", "activity.resume": "active",
                                         "activity.end": "ended"}[event]
                    await state()
                elif event == "activity.hints.set":
                    activity["hints_enabled"] = request["enabled"]
                    await state()
                elif event == "session.interrupt":
                    await gateway.send_json(ws, "session.state", state="interrupted")
                elif event == "hint.request":
                    report["last_id"] = request["request_id"]
                    report["hint_requests"] += 1
                    await gateway.send_json(ws, "hint.suggestion", request_id=request["request_id"],
                                            text=LONG_SAMPLE if report["hint_requests"] == 2 else SAMPLE,
                                            spoken=False)
                elif event != "hint.dismiss":
                    raise AssertionError(f"Unexpected command on text-only bench: {event}")
        except ConnectionClosed:
            pass
        finally:
            gateway.DEBUG.unregister(ws)
            device = None
            report["ready"] = False

    def request_gate(connection, request):
        if request.path == "/control":
            if local_request(connection, request) and not request.headers.get_all("Origin"):
                return None
            return connection.respond(403, "Local bench control only\n")
        return process_request(connection, request)

    print("TEXT-ONLY BENCH listening; cloud providers never called", flush=True)
    async with serve(handler, "0.0.0.0", 8765, process_request=request_gate):
        await asyncio.Future()


async def check():
    async with connect("ws://127.0.0.1:8765/control") as control:
        async def command(event="bench.status", **payload):
            await control.send(json.dumps({"event": event, **payload}))
            return json.loads(await asyncio.wait_for(control.recv(), 5))

        report = await command()
        assert report["ready"], "Device not connected yet"
        async with httpx.AsyncClient(base_url=f"http://{report['ip']}", trust_env=False, timeout=5) as client:
            async def get(path):
                response = await client.get(path)
                response.raise_for_status()
                return response.json()

            async def post(path, **payload):
                response = await client.post(path, json=payload)
                response.raise_for_status()

            async def wait(predicate):
                async with asyncio.timeout(6):
                    while True:
                        current = await get("/api/activity")
                        health = await get("/health")
                        stats = await get("/api/stats")
                        assert not health["capture_active"] and not health["ambient_listening"]
                        assert health["ambient_segments_sent"] == 0
                        assert not stats["player_busy"] and stats["frames_rx"] == stats["bytes_played"] == 0
                        if predicate(current):
                            return current
                        await asyncio.sleep(.1)

            async def show():
                await post("/api/button?key=1")
                await wait(lambda a: a["hint_ready"])
                return (await command())["last_id"]

            await post("/api/activity", action="start", consent=True, theme="travel", hints_enabled=True)
            await wait(lambda a: a["state"] == "active")
            first = await show()
            await asyncio.sleep(.65)
            await post("/api/button?key=1")
            await wait(lambda a: not a["hint_ready"])
            await command("bench.emit", payload={"event": "hint.suggestion", "request_id": first, "text": "迟到的旧提示"})
            await asyncio.sleep(.25)
            assert not (await get("/api/activity"))["hint_ready"]
            await asyncio.sleep(.65)
            second = await show()
            assert second != first
            await post("/api/activity", action="pause")
            await wait(lambda a: a["state"] == "paused" and not a["hint_ready"])
            await post("/api/activity", action="resume")
            await wait(lambda a: a["state"] == "active")
            await asyncio.sleep(.65)
            await show()
            await command("bench.emit", payload={"event": "ambient.speech"})
            await wait(lambda a: not a["hint_ready"])
            await post("/api/activity", action="end")
            await wait(lambda a: a["state"] == "ended" and not a["hint_ready"])
            await post("/api/activity", action="start", consent=True, theme="travel", hints_enabled=True)
            await wait(lambda a: a["state"] == "active")
            await asyncio.sleep(.65)
            await show()
            report = await command()
            assert report["binary_messages"] == 0
            print(json.dumps({"result": "PASS", "checks": ["receive", "K1 dismiss", "stale ID rejection",
                             "long text accepted", "pause clear", "speech event clear", "end clear", "rejoin"],
                             "binary_messages": report["binary_messages"], "demo": SAMPLE}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--serve", action="store_true")
    group.add_argument("--check", action="store_true")
    args = parser.parse_args()
    asyncio.run(bench_server() if args.serve else check())
