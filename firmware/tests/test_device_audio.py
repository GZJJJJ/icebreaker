"""Opt-in hardware regression. Sends silent PCM; does not exercise K0/acoustics."""

import argparse
import json
import time

import httpx


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("url", help="Trusted LAN device URL, e.g. http://192.168.137.201")
    args = parser.parse_args()
    with httpx.Client(base_url=args.url, timeout=5, trust_env=False) as client:
        def get(path):
            response = client.get(path)
            response.raise_for_status()
            return response.json()

        def post(path, pcm=b""):
            response = client.post(path, content=pcm)
            response.raise_for_status()
            return response.json()

        def wait_for(path, predicate, timeout=4):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                data = get(path)
                if predicate(data):
                    return data
                time.sleep(0.03)
            raise AssertionError(f"Timed out waiting for {path}: {data}")

        assert get("/api/stats")["connected"]
        initial = get("/api/stats")
        assert not initial["player_busy"], "Do not run while a conversation is active"
        assert get("/health")["state"] in {"idle", "interrupted", "error"}
        t0 = time.monotonic()
        assert post("/api/play", bytes(24000 * 2 * 5))["queued"]
        enqueue_ms = round((time.monotonic() - t0) * 1000, 1)
        wait_for("/health", lambda data: data["state"] == "speaking")
        playing = get("/api/stats")
        assert playing["player_busy"] and playing["xio_output"] == 0xA4
        assert client.post("/api/play", content=bytes(4800)).status_code == 400
        assert client.post("/api/tone").status_code == 400
        t0 = time.monotonic()
        assert post("/api/interrupt")["state"] == "interrupted"
        interrupt_http_ms = round((time.monotonic() - t0) * 1000, 1)
        muted = get("/api/stats")
        assert muted["xio_output"] == 0x84
        wait_for("/api/stats", lambda data: not data["player_busy"])
        stopped = get("/api/stats")["bytes_played"]
        time.sleep(0.5)
        assert get("/api/stats")["bytes_played"] == stopped, "Old PCM resumed after stop"
        assert get("/health")["state"] == "interrupted"
        post("/api/interrupt")  # Idempotent.
        for _ in range(3):
            assert post("/api/play", bytes(24000))["queued"]  # 0.5 seconds.
            wait_for("/health", lambda data: data["state"] == "speaking")
            wait_for("/api/stats", lambda data: not data["player_busy"])
            assert get("/health")["state"] == "idle"
            assert get("/api/stats")["xio_output"] == 0x84
        for invalid in (b"", b"x", bytes(240002)):
            assert client.post("/api/play", content=invalid).status_code == 400
        final = get("/api/stats")
        assert final["play_failures"] == initial["play_failures"]
        print(json.dumps({
            "result": "PASS", "enqueue_http_ms": enqueue_ms,
            "interrupt_http_ms": interrupt_http_ms,
            "local_stop_call_us": muted["interrupt_us"],
            "output_playing": hex(playing["xio_output"]),
            "output_muted": hex(muted["xio_output"]),
            "next_playbacks_completed": 3,
            "play_failures": final["play_failures"],
            "note": "Software/register test only; K0/acoustic latency not measured",
        }, ensure_ascii=False))


if __name__ == "__main__":
    main()
