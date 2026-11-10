"""Loopback-only, bounded live diagnostics. No conversation or image persistence."""

import asyncio
import base64
import ipaddress
import json
import secrets
import time
from dataclasses import dataclass, field
from pathlib import Path

from websockets.datastructures import Headers
from websockets.http11 import Response
from websockets.exceptions import ConnectionClosed


CAMERA_MAGIC = b"IBJPEG1\x00"
MAX_CAMERA_BYTES = 128 * 1024
CAPTURE_TIMEOUT_SECONDS = 20
PAGE = Path(__file__).with_name("debug.html")
EVENTS = {
    "gateway.ready", "session.state", "activity.state", "reply.stage", "reply.metrics",
    "asr.final", "llm.delta", "hint.suggestion", "hint.error", "activity.error", "error",
    "ambient.final", "ambient.error", "ambient.gap", "ambient.speech",
}


def encode(event, **payload):
    return json.dumps({"event": event, **payload}, ensure_ascii=False)


def local_request(connection, request):
    remote = connection.remote_address
    if not remote or not ipaddress.ip_address(remote[0]).is_loopback:
        return False
    port = connection.local_address[1]
    hosts = {f"127.0.0.1:{port}", f"localhost:{port}", f"[::1]:{port}"}
    host_values = request.headers.get_all("Host")
    origin_values = request.headers.get_all("Origin")
    if len(host_values) != 1 or len(origin_values) > 1:
        return False
    host = host_values[0].lower()
    return host in hosts and (not origin_values or origin_values[0] == f"http://{host}")


def process_request(connection, request):
    path = request.path.split("?", 1)[0]
    if path not in {"/", "/debug", "/debug.html"}:
        return connection.respond(404, "Not found\n")
    # Browser pages cannot connect to the device endpoint. Devices have no Origin.
    if request.headers.get("Upgrade", "").lower() == "websocket" and path == "/":
        if request.headers.get_all("Origin"):
            return connection.respond(403, "Device endpoint does not accept browser clients\n")
        return None
    if not local_request(connection, request):
        return connection.respond(403, "Debug panel is available on loopback only\n")
    if request.headers.get("Upgrade", "").lower() == "websocket":
        if path == "/debug":
            return None
        return connection.respond(404, "Not found\n")
    body = PAGE.read_bytes()
    return Response(200, "OK", Headers([
        ("Content-Type", "text/html; charset=utf-8"),
        ("Content-Length", str(len(body))),
        ("Cache-Control", "no-store"),
        ("X-Content-Type-Options", "nosniff"),
        ("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; "
         "style-src 'unsafe-inline'; img-src blob:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'"),
    ]), body)


@dataclass(eq=False)
class Viewer:
    queue: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(maxsize=128))
    device_id: str | None = None


@dataclass
class Device:
    connection_id: str = field(default_factory=lambda: secrets.token_hex(8))
    camera: bool = False
    ambient: bool = False
    mock: bool = True
    state: str = "connected"
    capture_id: str | None = None
    capture_owner: Viewer | None = None
    capture_timer: asyncio.TimerHandle | None = None
    snapshot_size: int = 0


class DebugHub:
    def __init__(self):
        self.devices = {}
        self.viewers = set()

    def enqueue(self, viewer, event, **payload):
        if viewer.queue.full():
            while not viewer.queue.empty():
                viewer.queue.get_nowait()
            viewer.queue.put_nowait(encode("debug.gap"))
        viewer.queue.put_nowait(encode(event, **payload))

    def status(self, viewer=None):
        devices = [{"id": d.connection_id, "camera": d.camera, "ambient": d.ambient, "mock": d.mock,
                    "state": d.state} for d in self.devices.values()]
        for target in ([viewer] if viewer else self.viewers):
            self.enqueue(target, "debug.status", devices=devices)

    def register(self, ws, mock):
        self.devices[ws] = Device(mock=mock)
        self.status()

    def unregister(self, ws):
        self.cancel_capture(ws, "device_disconnected")
        device = self.devices.pop(ws, None)
        if device:
            self.publish_reset(device, "device_disconnected")
        self.status()

    def publish_reset(self, device, reason):
        for viewer in self.viewers:
            if viewer.device_id == device.connection_id:
                while not viewer.queue.empty():
                    viewer.queue.get_nowait()
                self.enqueue(viewer, "debug.reset", reason=reason)

    def publish(self, ws, payload):
        device = self.devices.get(ws)
        if device is None:
            return
        event = payload["event"]
        if event == "session.state":
            device.state = payload["state"]
        if event == "activity.state" and payload.get("state") in {"paused", "ended"}:
            self.cancel_capture(ws, "activity_stopped")
            if payload["state"] == "ended":
                self.publish_reset(device, "activity_ended")
        if event == "session.state" and payload.get("state") in {"ended", "interrupted"}:
            self.cancel_capture(ws, "session_stopped")
            if payload["state"] == "ended":
                self.publish_reset(device, "session_ended")
        if event not in EVENTS and not event.startswith("camera."):
            return
        # Audio frames and provider credentials never enter debug queues.
        for viewer in self.viewers:
            if viewer.device_id == device.connection_id:
                self.enqueue(viewer, "debug.event", connection_id=device.connection_id,
                             timestamp=time.time(), payload=payload)

    def publish_capture(self, device, viewer, payload):
        if viewer is not None and viewer in self.viewers:
            self.enqueue(viewer, "debug.event", connection_id=device.connection_id,
                         timestamp=time.time(), payload=payload)

    def cancel_capture(self, ws, code):
        device = self.devices.get(ws)
        if device is None or device.capture_id is None:
            return
        capture_id = device.capture_id
        owner = device.capture_owner
        if device.capture_timer:
            device.capture_timer.cancel()
        device.capture_id = None
        device.capture_owner = None
        device.snapshot_size = 0
        device.capture_timer = None
        self.publish_capture(device, owner, {"event": "camera.capture.error", "code": code,
                                             "capture_id": capture_id})

    def incoming(self, ws, payload):
        device = self.devices.get(ws)
        if device is None:
            return
        event = payload["event"]
        if event == "device.ready":
            device.camera = payload.get("camera_capture") is True
            device.ambient = payload.get("ambient_context") is True
            self.status()
        elif event in {"camera.capture.started", "camera.capture.error", "camera.snapshot"}:
            if device.capture_id is None or payload.get("capture_id") != device.capture_id:
                return
            if event == "camera.capture.error":
                code = payload.get("code")
                allowed = {"busy", "capture_failed", "upload_failed", "cancelled", "not_authorized"}
                self.cancel_capture(ws, code if isinstance(code, str) and code in allowed else "capture_failed")
            elif event == "camera.snapshot":
                size = payload.get("size")
                if type(size) is not int or not 4 <= size <= MAX_CAMERA_BYTES:
                    self.cancel_capture(ws, "invalid_size")
                else:
                    device.snapshot_size = size
            else:
                self.publish(ws, {"event": event, "capture_id": device.capture_id})

    def snapshot(self, ws, packet):
        device = self.devices.get(ws)
        if device is None or device.capture_id is None:
            return
        # Binary envelope: magic (8), ASCII capture_id (32), JPEG bytes.
        capture_id = packet[8:40].decode("ascii", errors="replace")
        if capture_id != device.capture_id:
            return
        jpeg = packet[40:]
        if (len(jpeg) != device.snapshot_size or len(jpeg) > MAX_CAMERA_BYTES or
                jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9"):
            self.cancel_capture(ws, "invalid_jpeg")
            return
        self.publish_capture(device, device.capture_owner, {
            "event": "camera.snapshot.data", "capture_id": capture_id, "size": len(jpeg),
            "jpeg_base64": base64.b64encode(jpeg).decode("ascii"),
        })
        device.capture_timer.cancel()
        device.capture_id = None
        device.capture_owner = None
        device.snapshot_size = 0
        device.capture_timer = None

    async def cancel_on_device(self, ws):
        try:
            await ws.send(encode("camera.cancel"))
        except ConnectionClosed:
            pass

    async def command(self, viewer, request):
        event = request.get("event")
        if event == "debug.select":
            for ws, device in list(self.devices.items()):
                if device.capture_owner is viewer:
                    self.cancel_capture(ws, "cancelled")
                    await self.cancel_on_device(ws)
            while not viewer.queue.empty():
                viewer.queue.get_nowait()
            device_id = request.get("device_id")
            viewer.device_id = device_id if isinstance(device_id, str) else None
            self.enqueue(viewer, "debug.reset", reason="device_selected")
            self.status(viewer)
            return
        ws = next((ws for ws, d in self.devices.items() if d.connection_id == viewer.device_id), None)
        if event not in {"camera.capture", "camera.cancel"}:
            self.enqueue(viewer, "debug.error", code="unknown_event")
        elif ws is None:
            self.enqueue(viewer, "debug.error", code="device_not_connected")
        elif event == "camera.cancel":
            if self.devices[ws].capture_owner is viewer:
                self.cancel_capture(ws, "cancelled")
                await self.cancel_on_device(ws)
        elif request.get("consent") is not True:
            self.enqueue(viewer, "debug.error", code="consent_required")
        elif not self.devices[ws].camera:
            self.enqueue(viewer, "debug.error", code="camera_firmware_update_required")
        elif self.devices[ws].capture_id:
            self.enqueue(viewer, "debug.error", code="camera_busy")
        else:
            device = self.devices[ws]
            device.capture_id = secrets.token_hex(16)
            device.capture_owner = viewer

            def expire():
                self.cancel_capture(ws, "capture_timeout")
                asyncio.create_task(self.cancel_on_device(ws))

            device.capture_timer = asyncio.get_running_loop().call_later(CAPTURE_TIMEOUT_SECONDS, expire)
            self.publish(ws, {"event": "camera.capture.requested", "capture_id": device.capture_id})
            try:
                await ws.send(encode("camera.capture", capture_id=device.capture_id, consent=True))
            except ConnectionClosed:
                self.cancel_capture(ws, "device_disconnected")

    async def handle_viewer(self, ws):
        viewer = Viewer()
        self.viewers.add(viewer)
        self.status(viewer)

        async def send_events():
            while True:
                await ws.send(await viewer.queue.get())

        sender = asyncio.create_task(send_events())
        try:
            async for message in ws:
                try:
                    if not isinstance(message, str) or len(message) > 1024:
                        raise ValueError
                    request = json.loads(message)
                    if not isinstance(request, dict) or not isinstance(request.get("event"), str):
                        raise ValueError
                except (ValueError, json.JSONDecodeError):
                    self.enqueue(viewer, "debug.error", code="invalid_message")
                    continue
                await self.command(viewer, request)
        finally:
            self.viewers.discard(viewer)
            sender.cancel()
            await asyncio.gather(sender, return_exceptions=True)
            for device_ws, device in list(self.devices.items()):
                if device.capture_owner is viewer:
                    self.cancel_capture(device_ws, "cancelled")
                    await self.cancel_on_device(device_ws)
