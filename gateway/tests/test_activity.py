import asyncio
import json

import pytest

import server
from server import Session
from activity import Activity, MAX_HISTORY_TURNS, MAX_MESSAGE_CHARS


class Socket:
    def __init__(self):
        self.events = []

    async def send(self, data):
        self.events.append(json.loads(data))


async def event(ws, session, name, **payload):
    await server.handle_message(ws, session, json.dumps({"event": name, **payload}))


async def turn(ws, session):
    await event(ws, session, "session.start")
    await server.handle_message(ws, session, bytes(3200))
    await event(ws, session, "audio.end")
    await asyncio.wait_for(session.reply_task, 1)


@pytest.fixture(autouse=True)
def offline(monkeypatch):
    def blocked(*args, **kwargs):
        raise AssertionError("Unexpected external provider HTTP")

    monkeypatch.setattr("providers.httpx.AsyncClient", blocked)
    monkeypatch.setattr(server, "load_api_key", lambda: "offline")


@pytest.mark.parametrize("payload,code", [
    ({}, "consent_required"), ({"consent": "yes"}, "consent_required"),
    ({"consent": True, "scene": []}, "invalid_scene"),
    ({"consent": True, "scene": "guessed_romance"}, "invalid_scene"),
    ({"consent": True, "theme": {}}, "invalid_theme"),
    ({"consent": True, "camera_enabled": True}, "feature_not_ready"),
])
@pytest.mark.asyncio
async def test_invalid_start_keeps_existing_activity(payload, code):
    session, ws = server.Session(), Socket()
    await event(ws, session, "activity.start", consent=True, theme="film")
    session.activity.remember("old input", "old reply")
    await event(ws, session, "activity.start", **payload)
    assert ws.events[-1] == {"event": "activity.error", "code": code}
    assert session.activity.theme == "film" and session.activity.history


@pytest.mark.asyncio
async def test_hints_require_consent_and_return_suggestion(monkeypatch):
    ws, session = Socket(), Session(mock=True)
    await event(ws, session, "activity.start", consent=True, hints_enabled=False)
    await event(ws, session, "hint.request", silence_ms=25000)
    assert ws.events[-1] == {"event": "hint.error", "code": "hints_not_enabled"}
    await event(ws, session, "activity.start", consent=True, hints_enabled=True)
    await event(ws, session, "hint.request", silence_ms=25000)
    await asyncio.sleep(0)
    assert ws.events[-1]["event"] == "hint.suggestion"
    assert ws.events[-1]["spoken"] is False
    await event(ws, session, "activity.pause")
    assert session.hint_task is None


@pytest.mark.asyncio
async def test_hint_validation_and_busy(monkeypatch):
    ws, session = Socket(), Session(mock=True)
    await event(ws, session, "activity.start", consent=True, hints_enabled=True)
    await event(ws, session, "hint.request", silence_ms=-1)
    assert ws.events[-1]["code"] == "invalid_silence"
    await event(ws, session, "hint.request", silence_ms=25000)
    await event(ws, session, "hint.request", silence_ms=25000)
    assert ws.events[-1]["code"] == "busy"


@pytest.mark.asyncio
async def test_activity_spans_turns_interrupt_keeps_context_end_clears():
    session, ws = server.Session(), Socket()
    await event(ws, session, "activity.start", consent=True, scene="date", theme="reading")
    await turn(ws, session)
    await turn(ws, session)
    assert len(session.activity.history) == 4
    await event(ws, session, "session.interrupt")
    assert session.activity.state == "active" and len(session.activity.history) == 4
    await event(ws, session, "activity.pause")
    assert session.activity.state == "paused" and len(session.activity.history) == 4
    await event(ws, session, "session.start")
    assert ws.events[-1]["code"] == "activity_not_active"
    await event(ws, session, "activity.resume")
    await turn(ws, session)
    assert len(session.activity.history) == 6
    await event(ws, session, "activity.get")
    assert ws.events[-1]["history_turns"] == 3
    assert "history" not in ws.events[-1]  # Status contains counts, never conversation text.
    await event(ws, session, "activity.end")
    assert session.activity.state == "ended" and not session.activity.history
    assert not session.audio and session.reply_task is None
    await event(ws, session, "session.start")
    assert ws.events[-1]["code"] == "activity_not_active"
    await event(ws, session, "activity.start", consent=True, theme="travel")
    assert session.activity.state == "active" and not session.activity.history


@pytest.mark.asyncio
async def test_model_receives_activity_context_and_history(monkeypatch):
    calls = []

    class Asr:
        def __init__(self, *args):
            pass

        async def transcribe(self, pcm):
            return "context-canary"

    class Llm:
        def __init__(self, *args):
            pass

        async def reply(self, text, **kwargs):
            calls.append((text, kwargs))
            yield "reply-canary"

    async def tts(text):
        return bytes(3200)

    monkeypatch.setattr(server, "SiliconFlowAsrProvider", Asr)
    monkeypatch.setattr(server, "SiliconFlowLlmProvider", Llm)
    monkeypatch.setattr(server, "synthesize_with_fallback", tts)
    session, ws = server.Session(mock=False), Socket()
    await event(ws, session, "activity.start", consent=True, scene="meeting", theme="general")
    await turn(ws, session)
    await turn(ws, session)
    assert calls[0][1]["history"] == []
    assert calls[1][1]["history"] == [
        {"role": "user", "content": "context-canary"},
        {"role": "assistant", "content": "reply-canary"},
    ]
    assert "小会议" in calls[1][1]["context"]
    await event(ws, session, "activity.end")
    await event(ws, session, "activity.start", consent=True)
    await turn(ws, session)
    assert calls[-1][1]["history"] == []


@pytest.mark.asyncio
async def test_pause_cancels_pending_cloud_work_without_committing(monkeypatch):
    entered, cancelled = asyncio.Event(), asyncio.Event()

    async def tts(text):
        entered.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()

    monkeypatch.setattr(server, "synthesize_with_fallback", tts)
    # Use fake ASR and LLM; TTS is the cancellation boundary under test.
    class Asr:
        def __init__(self, *args): pass
        async def transcribe(self, pcm): return "input"

    class Llm:
        def __init__(self, *args): pass
        async def reply(self, text, **kwargs): yield "output"

    monkeypatch.setattr(server, "SiliconFlowAsrProvider", Asr)
    monkeypatch.setattr(server, "SiliconFlowLlmProvider", Llm)
    session, ws = server.Session(mock=False), Socket()
    await event(ws, session, "activity.start", consent=True)
    await event(ws, session, "session.start")
    await server.handle_message(ws, session, bytes(3200))
    await event(ws, session, "audio.end")
    await asyncio.wait_for(entered.wait(), 1)
    await event(ws, session, "activity.pause")
    assert cancelled.is_set() and not session.activity.history
    assert not any(e["event"] == "tts.audio" for e in ws.events)


def test_history_has_bounded_memory():
    activity = Activity(state="active")
    for i in range(20):
        activity.remember(str(i) * 3000, "x" * 3000)
    assert len(activity.history) == MAX_HISTORY_TURNS * 2
    assert all(len(message["content"]) <= MAX_MESSAGE_CHARS for message in activity.history)
