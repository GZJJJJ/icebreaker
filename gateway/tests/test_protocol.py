import asyncio
import base64
import json

import pytest
import pytest_asyncio
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

import server as gateway_server
from server import Session, connection_handler, handle_message


@pytest.fixture(autouse=True)
def no_external_providers(monkeypatch):
    def blocked_client(*args, **kwargs):
        raise AssertionError("Unexpected provider network call in an offline test")

    monkeypatch.setattr(gateway_server, "load_api_key", lambda: "test-key")
    monkeypatch.setattr("providers.httpx.AsyncClient", blocked_client)


async def next_event(ws):
    return json.loads(await asyncio.wait_for(ws.recv(), 2))


async def until_state(ws, state):
    events = []
    async with asyncio.timeout(3):
        while True:
            event = await next_event(ws)
            events.append(event)
            if event.get("event") == "session.state" and event.get("state") == state:
                return events


async def start_turn(ws):
    await ws.send(json.dumps({"event": "session.start", "session_id": "test"}))
    assert (await next_event(ws))["state"] == "listening"
    await ws.send(bytes(3200))


@pytest_asyncio.fixture
async def gateway(monkeypatch):
    monkeypatch.setenv("ICEBREAKER_MOCK", "1")
    async with serve(connection_handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        yield f"ws://127.0.0.1:{port}"


@pytest.mark.asyncio
async def test_mock_session_streams_reply(gateway):
    async with connect(gateway) as ws:
        assert (await next_event(ws))["event"] == "gateway.ready"
        await ws.send(json.dumps({"event": "session.start", "session_id": "test"}))
        assert (await next_event(ws))["state"] == "listening"
        await ws.send(b"\x00" * 3200)
        await ws.send(json.dumps({"event": "audio.end"}))

        events = await until_state(ws, "idle")
        assert {"event": "asr.final", "text": "这是一次 Mock 语音输入。"} in events
        assert any(item["event"] == "llm.delta" for item in events)
        assert sum(item["event"] == "tts.audio" for item in events) == 3
        assert sum(item["event"] == "tts.end" for item in events) == 1
        for item in events:
            if item["event"] == "tts.audio":
                assert item["sample_rate"] == 24000
                assert len(base64.b64decode(item["audio"])) % 2 == 0


@pytest.mark.asyncio
async def test_interrupt_stops_reply(gateway):
    async with connect(gateway) as ws:
        await next_event(ws)
        await start_turn(ws)
        await ws.send(json.dumps({"event": "audio.end"}))
        while (await next_event(ws))["event"] != "asr.final":
            pass
        await ws.send(json.dumps({"event": "session.interrupt"}))
        await until_state(ws, "interrupted")
        with pytest.raises(asyncio.TimeoutError):
            await asyncio.wait_for(ws.recv(), 0.1)
        # Interrupt is idempotent, and the next turn works on the same socket.
        await ws.send(json.dumps({"event": "session.interrupt"}))
        await until_state(ws, "interrupted")
        await start_turn(ws)
        await ws.send(json.dumps({"event": "audio.end"}))
        assert any(e["event"] == "tts.audio" for e in await until_state(ws, "idle"))


@pytest.mark.asyncio
async def test_three_turns_do_not_accumulate_audio(gateway):
    async with connect(gateway) as ws:
        await next_event(ws)
        for _ in range(3):
            await start_turn(ws)
            await ws.send(json.dumps({"event": "audio.end"}))
            events = await until_state(ws, "idle")
            assert sum(e["event"] == "tts.end" for e in events) == 1


@pytest.mark.asyncio
async def test_reply_reports_stages_and_timings(gateway):
    async with connect(gateway) as ws:
        await next_event(ws)
        await start_turn(ws)
        await ws.send('{"event":"audio.end"}')
        events = await until_state(ws, "idle")
        stages = [e for e in events if e["event"] == "reply.stage"]
        assert [e["stage"] for e in stages] == ["asr", "llm", "tts", "sending"]
        assert all(e["elapsed_ms"] >= 0 for e in stages)
        metrics = next(e for e in events if e["event"] == "reply.metrics")
        assert metrics["outcome"] == "ok"
        assert set(metrics["timings_ms"]) == {
            "asr", "llm_first", "llm", "tts", "send", "total"
        }
        assert all(value >= 0 for value in metrics["timings_ms"].values())
        assert metrics["audio_bytes"] == 9600
        assert not any(key in metrics for key in ("text", "audio", "api_key"))


@pytest.mark.asyncio
async def test_cancellation_time_is_reported_and_next_reply_is_independent(monkeypatch):
    entered = asyncio.Event()

    class SlowCleanupAsr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            entered.set()
            try:
                await asyncio.Event().wait()
            finally:
                await asyncio.sleep(0.03)

    monkeypatch.setattr(gateway_server, "SiliconFlowAsrProvider", SlowCleanupAsr)
    ws, session = RecordingSocket(), Session(mock=False)
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, '{"event":"audio.end"}')
    await asyncio.wait_for(entered.wait(), 1)
    await handle_message(ws, session, '{"event":"session.interrupt"}')
    assert ws.events[-1]["cancel_ms"] >= 20
    assert not any(e["event"] == "tts.audio" for e in ws.events)
    session.mock = True
    await handle_message(ws, session, '{"event":"session.start"}')
    assert ws.events[-1]["cancel_ms"] < 20
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, '{"event":"audio.end"}')
    await session.reply_task
    assert next(e for e in reversed(ws.events) if e["event"] == "reply.metrics")["outcome"] == "ok"


class RecordingSocket:
    def __init__(self):
        self.events = []

    async def send(self, data):
        self.events.append(json.loads(data))


@pytest.mark.asyncio
async def test_empty_audio_and_invalid_messages_are_rejected():
    ws = RecordingSocket()
    session = Session()
    for message, code in [("[]", "invalid_message"), ("{", "invalid_json"),
                          ('{"event":"oops"}', "unknown_event")]:
        await handle_message(ws, session, message)
        assert ws.events[-1]["code"] == code
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, '{"event":"audio.end"}')
    assert ws.events[-2]["code"] == "empty_audio"
    assert ws.events[-1] == {"event": "session.state", "state": "idle"}
    assert session.reply_task is None
    await handle_message(ws, session, '{"event":"audio.input","audio":"!"}')
    assert ws.events[-1]["code"] == "invalid_audio"


@pytest.mark.asyncio
async def test_restart_cancels_old_asr_and_clears_audio(monkeypatch):
    entered, cancelled = asyncio.Event(), asyncio.Event()

    class BlockedAsr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            entered.set()
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.set()

    monkeypatch.setattr(gateway_server, "SiliconFlowAsrProvider", BlockedAsr)
    ws, session = RecordingSocket(), Session(mock=False)
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, '{"event":"audio.end"}')
    await asyncio.wait_for(entered.wait(), 1)
    old_task = session.reply_task
    await handle_message(ws, session, '{"event":"session.start"}')
    assert cancelled.is_set()
    assert old_task.done()
    assert session.reply_task is None
    assert not session.audio
    assert not any(e["event"] in {"asr.final", "tts.audio"} for e in ws.events)


@pytest.mark.asyncio
@pytest.mark.parametrize("action", ["session.end", "session.interrupt"])
async def test_stop_clears_capture_cache(action):
    ws, session = RecordingSocket(), Session()
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, json.dumps({"event": action}))
    assert not session.audio
    assert session.reply_task is None


@pytest.mark.asyncio
@pytest.mark.parametrize("outcome,code", [("", "no_speech"), ("failure", "asr_failed"),
                                         ("timeout", "reply_timeout")])
async def test_asr_failure_recovers_without_leaking_details(monkeypatch, caplog, outcome, code):
    class FakeAsr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            if outcome == "failure":
                raise RuntimeError("secret-canary-do-not-log")
            if outcome == "timeout":
                await asyncio.Event().wait()
            return outcome

    monkeypatch.setattr(gateway_server, "SiliconFlowAsrProvider", FakeAsr)
    monkeypatch.setattr(gateway_server, "REPLY_TIMEOUT_SECONDS", 0.02, raising=False)
    ws, session = RecordingSocket(), Session(mock=False)
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, '{"event":"audio.end"}')
    await asyncio.wait_for(session.reply_task, 1)
    assert any(e.get("code") == code for e in ws.events)
    assert ws.events[-1] == {"event": "session.state", "state": "idle"}
    assert not session.audio
    assert "secret-canary" not in caplog.text + json.dumps(ws.events)
    metrics = next(e for e in ws.events if e["event"] == "reply.metrics")
    assert metrics["outcome"] == code
    assert metrics["timings_ms"]["total"] >= 0


@pytest.mark.asyncio
@pytest.mark.parametrize("stage,outcome,code", [
    ("llm", "failure", "llm_failed"), ("llm", "empty", "empty_reply"),
    ("tts", "failure", "tts_failed"), ("tts", "empty", "tts_failed"),
    ("tts", "odd", "tts_failed"), ("tts", "oversized", "tts_failed"),
])
async def test_downstream_failures_and_next_turn(monkeypatch, caplog, stage, outcome, code):
    class FakeAsr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            return "你好"

    class FakeLlm:
        def __init__(self, *args):
            pass

        async def reply(self, text):
            if stage == "llm" and outcome == "failure":
                raise RuntimeError("secret-canary-do-not-log")
            if stage != "llm" or outcome != "empty":
                yield "你好"

    async def fake_tts(text):
        if outcome == "failure":
            raise RuntimeError("secret-canary-do-not-log")
        return {"empty": b"", "odd": b"x",
                "oversized": bytes(gateway_server.MAX_TTS_BYTES + 2)}[outcome]

    monkeypatch.setattr(gateway_server, "SiliconFlowAsrProvider", FakeAsr)
    monkeypatch.setattr(gateway_server, "SiliconFlowLlmProvider", FakeLlm)
    monkeypatch.setattr(gateway_server, "synthesize_with_fallback", fake_tts)
    ws, session = RecordingSocket(), Session(mock=False)
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, '{"event":"audio.end"}')
    await asyncio.wait_for(session.reply_task, 1)
    assert any(e.get("code") == code for e in ws.events)
    assert ws.events[-1] == {"event": "session.state", "state": "idle"}
    assert not any(e["event"] == "tts.audio" for e in ws.events)
    assert "secret-canary" not in caplog.text + json.dumps(ws.events)
    session.mock = True
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(3200))
    await handle_message(ws, session, '{"event":"audio.end"}')
    await asyncio.wait_for(session.reply_task, 1)
    assert any(e["event"] == "tts.end" for e in ws.events)


@pytest.mark.asyncio
async def test_capture_limit_and_odd_pcm_are_recoverable():
    ws, session = RecordingSocket(), Session()
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, bytes(gateway_server.MAX_CAPTURE_BYTES))
    assert len(session.audio) == gateway_server.MAX_CAPTURE_BYTES
    await handle_message(ws, session, b"xx")
    assert ws.events[-2]["code"] == "audio_too_long"
    assert session.state == "idle" and not session.audio
    await handle_message(ws, session, '{"event":"session.start"}')
    await handle_message(ws, session, b"x")
    await handle_message(ws, session, '{"event":"audio.end"}')
    assert ws.events[-2]["code"] == "invalid_audio"
    assert session.state == "idle" and not session.audio


@pytest.mark.asyncio
async def test_disconnect_cancels_pending_reply(monkeypatch):
    entered, cancelled = asyncio.Event(), asyncio.Event()

    class FakeAsr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            entered.set()
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.set()

    monkeypatch.setenv("ICEBREAKER_MOCK", "0")
    monkeypatch.setattr(gateway_server, "SiliconFlowAsrProvider", FakeAsr)
    async with serve(connection_handler, "127.0.0.1", 0) as server:
        async with connect(f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}") as ws:
            await next_event(ws)
            await start_turn(ws)
            await ws.send('{"event":"audio.end"}')
            await asyncio.wait_for(entered.wait(), 1)
        await asyncio.wait_for(cancelled.wait(), 1)


@pytest.mark.asyncio
async def test_edge_cancellation_kills_and_reaps_transcoder(monkeypatch):
    import edge_tts
    import providers

    entered = asyncio.Event()

    class FakeProcess:
        returncode = None
        killed = False
        reaped = False

        def kill(self):
            self.killed = True

        async def communicate(self):
            if not self.killed:
                entered.set()
                await asyncio.Event().wait()
            self.reaped = True
            self.returncode = -1
            return b"", b""

    process = FakeProcess()

    async def fake_save(*args):
        pass

    async def fake_spawn(*args, **kwargs):
        return process

    monkeypatch.setattr(edge_tts.Communicate, "save", fake_save)
    monkeypatch.setattr(providers.asyncio, "create_subprocess_exec", fake_spawn)
    task = asyncio.create_task(providers.EdgeTtsProvider().synthesize("test"))
    await asyncio.wait_for(entered.wait(), 1)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert process.killed and process.reaped
