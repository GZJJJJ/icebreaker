import asyncio
import json

import pytest

import server
from activity import MAX_AMBIENT_SEGMENTS, Activity


class Socket:
    def __init__(self):
        self.events = []

    async def send(self, data):
        self.events.append(json.loads(data))


async def event(ws, session, name, **payload):
    await server.handle_message(ws, session, json.dumps({"event": name, **payload}))


def packet(session, pcm=None, activity_id=None):
    return (server.AMBIENT_MAGIC + (activity_id or session.activity.activity_id).encode("ascii") +
            (bytes(16000) if pcm is None else pcm))


@pytest.fixture(autouse=True)
def offline(monkeypatch):
    def blocked(*args, **kwargs):
        raise AssertionError("Unexpected provider HTTP")
    monkeypatch.setattr("providers.httpx.AsyncClient", blocked)
    monkeypatch.setattr(server, "load_api_key", lambda: "offline")


@pytest.mark.asyncio
async def test_join_enables_context_without_separate_flag_and_never_replies():
    ws, session = Socket(), server.Session()
    await event(ws, session, "activity.start", consent=True, hints_enabled=False)
    assert ws.events[-1]["listening_enabled"] is True
    await server.handle_message(ws, session, packet(session))
    await asyncio.wait_for(session.ambient_task, 1)
    assert session.activity.ambient
    assert session.activity.history == [] and session.audio == b""
    assert session.state == "idle"
    assert any(e["event"] == "ambient.final" for e in ws.events)
    assert not any(e["event"] in {"llm.delta", "tts.start", "tts.audio", "asr.final"} for e in ws.events)


@pytest.mark.asyncio
async def test_rejects_unjoined_paused_stale_and_malformed_audio():
    ws, session = Socket(), server.Session()
    await server.handle_message(ws, session, packet(session, activity_id="a" * 32))
    assert session.ambient_task is None
    await event(ws, session, "activity.start", consent=True)
    old_id = session.activity.activity_id
    for data in [b"x", b"", bytes(server.MAX_AMBIENT_BYTES + 2)]:
        await server.handle_message(ws, session, packet(session, data))
    assert session.ambient_task is None
    await event(ws, session, "activity.pause")
    await server.handle_message(ws, session, packet(session))
    assert session.ambient_task is None
    await event(ws, session, "activity.resume")
    assert old_id != session.activity.activity_id
    await server.handle_message(ws, session, packet(session, activity_id=old_id))
    assert session.ambient_task is None


@pytest.mark.parametrize("stop", ["activity.pause", "activity.end", "session.start", "session.interrupt"])
@pytest.mark.asyncio
async def test_stop_cancels_asr_and_discards_pending_and_late_text(monkeypatch, stop):
    entered = asyncio.Event()

    class Asr:
        def __init__(self, *args): pass
        async def transcribe(self, pcm):
            entered.set()
            await asyncio.Event().wait()

    monkeypatch.setattr(server, "SiliconFlowAsrProvider", Asr)
    ws, session = Socket(), server.Session(mock=False)
    await event(ws, session, "activity.start", consent=True)
    await server.handle_message(ws, session, packet(session))
    await asyncio.wait_for(entered.wait(), 1)
    await server.handle_message(ws, session, packet(session))
    await event(ws, session, stop)
    assert session.ambient_task is None and session.ambient_pending is None
    assert not session.activity.ambient
    assert not any(e["event"] == "ambient.final" for e in ws.events)


@pytest.mark.asyncio
async def test_slow_asr_has_one_active_and_latest_pending(monkeypatch):
    entered, release = asyncio.Event(), asyncio.Event()
    calls = []

    class Asr:
        def __init__(self, *args): pass
        async def transcribe(self, pcm):
            calls.append(pcm[0])
            entered.set()
            await release.wait()
            return str(pcm[0])

    monkeypatch.setattr(server, "SiliconFlowAsrProvider", Asr)
    ws, session = Socket(), server.Session(mock=False)
    await event(ws, session, "activity.start", consent=True)
    for value in range(1, 5):
        await server.handle_message(ws, session, packet(session, bytes([value]) * 16000))
        if value == 1: await asyncio.wait_for(entered.wait(), 1)
    release.set()
    await asyncio.wait_for(session.ambient_task, 1)
    assert calls == [1, 4]
    assert any(e["event"] == "ambient.gap" for e in ws.events)


@pytest.mark.asyncio
async def test_hints_wait_for_transcription_and_use_table_context(monkeypatch):
    entered, release = asyncio.Event(), asyncio.Event()
    contexts = []

    class Asr:
        def __init__(self, *args): pass
        async def transcribe(self, pcm):
            entered.set()
            await release.wait()
            return "大家刚刚聊了徒步和露营"

    class Llm:
        def __init__(self, *args): pass
        async def reply(self, text, **kwargs):
            contexts.append(kwargs["context"])
            yield "你最想去哪里露营？"

    monkeypatch.setattr(server, "SiliconFlowAsrProvider", Asr)
    monkeypatch.setattr(server, "SiliconFlowLlmProvider", Llm)
    ws, session = Socket(), server.Session(mock=False)
    await event(ws, session, "activity.start", consent=True, hints_enabled=True)
    await server.handle_message(ws, session, packet(session))
    await asyncio.wait_for(entered.wait(), 1)
    await event(ws, session, "hint.request", silence_ms=25000)
    await asyncio.sleep(0)
    assert not contexts
    release.set()
    await asyncio.wait_for(session.hint_task, 1)
    assert "徒步和露营" in contexts[0]
    await event(ws, session, "activity.end")
    assert not session.activity.ambient
    await event(ws, session, "activity.start", consent=True)
    assert "露营" not in session.activity.context()


def test_table_transcripts_are_bounded():
    activity = Activity.from_request({"consent": True})
    for _ in range(100):
        activity.remember_ambient("x" * 2000)
    assert len(activity.ambient) == MAX_AMBIENT_SEGMENTS
    assert all(len(text) <= 500 for text in activity.ambient)
    assert "ambient" not in activity.snapshot()


@pytest.mark.parametrize("failure", ["timeout", "provider"])
@pytest.mark.asyncio
async def test_asr_failure_does_not_stop_activity_or_emit_reply(monkeypatch, failure):
    class Asr:
        def __init__(self, *args): pass
        async def transcribe(self, pcm):
            if failure == "provider": raise ValueError("private provider detail")
            await asyncio.Event().wait()
    monkeypatch.setattr(server, "SiliconFlowAsrProvider", Asr)
    monkeypatch.setattr(server, "AMBIENT_ASR_TIMEOUT_SECONDS", 0.01)
    ws, session = Socket(), server.Session(mock=False)
    await event(ws, session, "activity.start", consent=True)
    await server.handle_message(ws, session, packet(session))
    await asyncio.wait_for(session.ambient_task, 1)
    assert session.activity.state == "active" and session.state == "idle"
    assert ws.events[-1] == {"event": "ambient.error",
                             "code": "asr_timeout" if failure == "timeout" else "asr_failed"}
    assert "private provider detail" not in str(ws.events)
    session.mock = True
    await server.handle_message(ws, session, packet(session))
    await asyncio.wait_for(session.ambient_task, 1)
    assert ws.events[-1]["event"] == "ambient.final"


@pytest.mark.asyncio
async def test_speech_cancels_pending_hint_but_not_ambient_asr(monkeypatch):
    entered, cancelled = asyncio.Event(), asyncio.Event()
    async def slow_hint(session):
        entered.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()
    monkeypatch.setattr(server, "generate_hint", slow_hint)
    ws, session = Socket(), server.Session()
    await event(ws, session, "activity.start", consent=True, hints_enabled=True)
    await event(ws, session, "hint.request", silence_ms=25000)
    await asyncio.wait_for(entered.wait(), 1)
    await event(ws, session, "ambient.speech", activity_id=session.activity.activity_id)
    assert cancelled.is_set() and session.hint_task is None
    assert not any(e["event"] == "hint.suggestion" for e in ws.events)
