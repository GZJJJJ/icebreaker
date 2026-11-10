import asyncio
import base64
import json
from types import SimpleNamespace

import pytest
import pytest_asyncio
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.exceptions import InvalidStatus

import debug_panel
import server
from debug_panel import CAMERA_MAGIC, DebugHub, Viewer, process_request


async def receive(ws, event, payload_event=None):
    async with asyncio.timeout(2):
        while True:
            message = json.loads(await ws.recv())
            if message["event"] == event and (
                payload_event is None or message["payload"]["event"] == payload_event
            ):
                return message


@pytest_asyncio.fixture
async def gateway(monkeypatch):
    monkeypatch.setenv("ICEBREAKER_MOCK", "1")
    monkeypatch.setattr(server, "DEBUG", DebugHub())
    def blocked(*args, **kwargs):
        raise AssertionError("Provider calls forbidden")
    monkeypatch.setattr("providers.httpx.AsyncClient", blocked)
    async with serve(server.route_connection, "127.0.0.1", 0, process_request=process_request) as app:
        yield f"ws://127.0.0.1:{app.sockets[0].getsockname()[1]}"


async def select_device(debug, device):
    await receive(device, "gateway.ready")
    await device.send('{"event":"device.ready","camera_capture":true}')
    async with asyncio.timeout(2):
        while True:
            status = await receive(debug, "debug.status")
            if len(status["devices"]) == 1 and status["devices"][0]["camera"]:
                break
    await debug.send(json.dumps({"event": "debug.select", "device_id": status["devices"][0]["id"]}))
    await receive(debug, "debug.reset")
    await receive(debug, "debug.status")


@pytest.mark.asyncio
async def test_live_text_metrics_hint_and_no_audio_or_history(gateway, caplog):
    async with connect(gateway + "/debug") as debug, connect(gateway) as device:
        await select_device(debug, device)
        await device.send('{"event":"activity.start","consent":true,"hints_enabled":true}')
        await receive(device, "activity.state")
        await device.send('{"event":"session.start"}')
        await receive(device, "session.state")
        await device.send(bytes(3200))
        await device.send('{"event":"audio.end"}')
        events = []
        async with asyncio.timeout(2):
            while True:
                message = json.loads(await debug.recv())
                if message["event"] == "debug.event":
                    events.append(message["payload"])
                    if message["payload"].get("state") == "idle":
                        break
        assert any(p.get("text") == "这是一次 Mock 语音输入。" for p in events)
        assert any(p["event"] == "llm.delta" for p in events)
        assert any(p["event"] == "reply.metrics" for p in events)
        assert not any(p["event"].startswith("tts.") for p in events)
        assert "这是一次 Mock" not in caplog.text
        await receive(device, "reply.metrics")
        await device.send('{"event":"hint.request","silence_ms":25000}')
        assert (await receive(debug, "debug.event", "hint.suggestion"))["payload"]["text"]
        # A new viewer must select a device and receives no prior conversation.
        async with connect(gateway + "/debug") as late:
            await receive(late, "debug.status")
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(late.recv(), .05)
        await device.send('{"event":"activity.end"}')
        assert (await receive(debug, "debug.reset"))["reason"] == "activity_ended"


@pytest.mark.asyncio
async def test_capture_consent_isolation_envelope_and_repeat(gateway):
    async with connect(gateway + "/debug") as debug, connect(gateway) as device:
        await select_device(debug, device)
        await debug.send('{"event":"camera.capture"}')
        assert (await receive(debug, "debug.error"))["code"] == "consent_required"
        with pytest.raises(asyncio.TimeoutError):
            await asyncio.wait_for(device.recv(), .05)
        jpeg = b"\xff\xd8test\xff\xd9"
        for _ in range(2):
            await debug.send('{"event":"camera.capture","consent":true}')
            command = await receive(device, "camera.capture")
            capture_id = command["capture_id"]
            assert command["consent"] is True and len(capture_id) == 32
            await device.send(json.dumps({"event": "camera.snapshot", "capture_id": capture_id, "size": len(jpeg)}))
            # PCM remains PCM even while a snapshot is pending.
            await device.send('{"event":"session.start"}')
            await receive(device, "session.state")
            await device.send(bytes(3200))
            await device.send(CAMERA_MAGIC + capture_id.encode() + jpeg)
            payload = (await receive(debug, "debug.event", "camera.snapshot.data"))["payload"]
            assert base64.b64decode(payload["jpeg_base64"]) == jpeg
            await device.send('{"event":"audio.end"}')
            await receive(device, "reply.metrics")


@pytest.mark.asyncio
async def test_cancel_timeout_disconnect_and_old_firmware(gateway, monkeypatch):
    async with connect(gateway + "/debug") as debug, connect(gateway) as device:
        await select_device(debug, device)
        for mode in ("cancel", "timeout"):
            monkeypatch.setattr(debug_panel, "CAPTURE_TIMEOUT_SECONDS", .04 if mode == "timeout" else 20)
            await debug.send('{"event":"camera.capture","consent":true}')
            command = await receive(device, "camera.capture")
            if mode == "cancel":
                await debug.send('{"event":"camera.cancel"}')
            error = (await receive(debug, "debug.event", "camera.capture.error"))["payload"]
            assert error["code"] == ("cancelled" if mode == "cancel" else "capture_timeout")
            await receive(device, "camera.cancel")
            await device.send(CAMERA_MAGIC + command["capture_id"].encode() + b"\xff\xd8x\xff\xd9")
        monkeypatch.setattr(debug_panel, "CAPTURE_TIMEOUT_SECONDS", 20)
        await debug.send('{"event":"camera.capture","consent":true}')
        await receive(device, "camera.capture")
        await debug.close()
        await receive(device, "camera.cancel")
        assert next(iter(server.DEBUG.devices.values())).capture_id is None


@pytest.mark.asyncio
async def test_unknown_device_and_old_firmware(gateway):
    async with connect(gateway + "/debug") as debug, connect(gateway) as device:
        await receive(device, "gateway.ready")
        status = await receive(debug, "debug.status")
        while not status["devices"]:
            status = await receive(debug, "debug.status")
        await debug.send(json.dumps({"event":"debug.select", "device_id":status["devices"][0]["id"]}))
        await receive(debug, "debug.reset")
        await debug.send('{"event":"camera.capture","consent":true}')
        assert (await receive(debug, "debug.error"))["code"] == "camera_firmware_update_required"
        await debug.send('{"event":"debug.select","device_id":"missing"}')
        await receive(debug, "debug.reset")
        await debug.send('{"event":"camera.capture","consent":true}')
        assert (await receive(debug, "debug.error"))["code"] == "device_not_connected"


@pytest.mark.asyncio
async def test_browser_origin_protection(gateway):
    for path, origin in [("/debug", "https://evil.test"), ("", "http://127.0.0.1")]:
        with pytest.raises(InvalidStatus) as error:
            async with connect(gateway + path, origin=origin):
                pass
        assert error.value.response.status_code == 403


@pytest.mark.asyncio
async def test_http_page_served_by_same_port_and_normal_origin_debug_connects(gateway):
    port = int(gateway.rsplit(":", 1)[1])
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n".encode())
    await writer.drain()
    response = await asyncio.wait_for(reader.read(), 2)
    writer.close()
    await writer.wait_closed()
    assert response.startswith(b"HTTP/1.1 200 OK") and b"Cache-Control: no-store" in response
    assert b"Icebreaker" in response
    async with connect(gateway + "/debug", origin=f"http://127.0.0.1:{port}") as debug:
        assert (await receive(debug, "debug.status"))["devices"] == []


def test_http_page_loopback_host_origin_and_no_store():
    connection = SimpleNamespace(remote_address=("127.0.0.1", 12345), local_address=("127.0.0.1", 8765))
    connection.respond = lambda code, body: SimpleNamespace(status_code=code, body=body)
    request = SimpleNamespace(path="/", headers=Headers([("Host", "127.0.0.1:8765")]))
    response = process_request(connection, request)
    assert response.status_code == 200 and b"Icebreaker" in response.body
    assert response.headers["Cache-Control"] == "no-store"
    connection.remote_address = ("192.168.1.10", 12345)
    assert process_request(connection, request).status_code == 403
    connection.remote_address = ("127.0.0.1", 12345)
    request.headers = Headers([("Host", "evil.test:8765")])
    assert process_request(connection, request).status_code == 403


def test_slow_viewer_queue_is_bounded_and_reset_removes_content():
    hub = DebugHub()
    ws = object()
    hub.register(ws, True)
    viewer = Viewer(device_id=hub.devices[ws].connection_id)
    hub.viewers.add(viewer)
    for _ in range(500):
        hub.publish(ws, {"event": "llm.delta", "text": "canary"})
    assert viewer.queue.qsize() <= 128
    hub.publish(ws, {"event": "activity.state", "state": "ended"})
    queued = []
    while not viewer.queue.empty():
        queued.append(viewer.queue.get_nowait())
    assert "canary" not in "".join(queued)


@pytest.mark.asyncio
async def test_multiple_devices_selection_busy_invalid_and_unrequested_images():
    class Socket:
        def __init__(self):
            self.messages = []

        async def send(self, message):
            self.messages.append(json.loads(message))

    hub = DebugHub()
    one, two = Socket(), Socket()
    hub.register(one, True)
    hub.register(two, True)
    first, second = hub.devices[one], hub.devices[two]
    first.camera = second.camera = True
    viewer = Viewer(device_id=second.connection_id)
    observer = Viewer(device_id=second.connection_id)
    hub.viewers.add(viewer)
    hub.viewers.add(observer)
    hub.publish(one, {"event": "asr.final", "text": "wrong device"})
    assert viewer.queue.empty()
    hub.snapshot(two, CAMERA_MAGIC + b"0" * 32 + b"\xff\xd8x\xff\xd9")
    assert viewer.queue.empty()
    await hub.command(viewer, {"event": "camera.capture", "consent": True})
    assert not one.messages and two.messages[-1]["event"] == "camera.capture"
    capture_id = second.capture_id
    while not observer.queue.empty():
        observer.queue.get_nowait()
    await hub.command(viewer, {"event": "camera.capture", "consent": True})
    assert len(two.messages) == 1
    hub.incoming(two, {"event": "camera.snapshot", "capture_id": capture_id, "size": 5})
    hub.snapshot(two, CAMERA_MAGIC + b"f" * 32 + b"\xff\xd8x\xff\xd9")
    assert second.capture_id == capture_id
    hub.snapshot(two, CAMERA_MAGIC + capture_id.encode() + b"xxxxx")
    assert second.capture_id is None
    assert any('invalid_jpeg' in viewer.queue.get_nowait() for _ in range(viewer.queue.qsize()))
    assert observer.queue.empty()
    await hub.command(viewer, {"event": "camera.capture", "consent": True})
    capture_id = second.capture_id
    hub.incoming(two, {"event": "camera.snapshot", "capture_id": capture_id, "size": 5})
    while not observer.queue.empty():
        observer.queue.get_nowait()
    hub.snapshot(two, CAMERA_MAGIC + capture_id.encode() + b"\xff\xd8x\xff\xd9")
    assert json.loads(viewer.queue.get_nowait())["payload"]["event"] == "camera.capture.requested"
    data = [json.loads(viewer.queue.get_nowait()) for _ in range(viewer.queue.qsize())]
    assert any(m["payload"]["event"] == "camera.snapshot.data" for m in data)
    assert observer.queue.empty()
    await hub.command(viewer, {"event": "camera.capture", "consent": True})
    hub.incoming(two, {"event": "camera.snapshot", "capture_id": second.capture_id, "size": 999999})
    assert second.capture_id is None
