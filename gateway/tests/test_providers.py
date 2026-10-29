import json
import logging

import httpx
import pytest

import providers


@pytest.mark.asyncio
async def test_llm_keeps_selected_model_and_reads_only_spoken_content(monkeypatch, caplog):
    original_client = httpx.AsyncClient

    def respond(request):
        body = json.loads(request.content)
        assert body["model"] == "deepseek-ai/DeepSeek-R1-0528-Qwen3-8B"
        assert body["max_tokens"] == 160
        assert "enable_thinking" not in body  # Not honored by this model in the real probe.
        assert body["messages"][0]["content"] == providers.SYSTEM_PROMPT
        assert "30字以内" in providers.SYSTEM_PROMPT and "只给答案" in providers.SYSTEM_PROMPT
        chunks = [
            {"choices": [{"delta": {"reasoning_content": "private-reasoning-canary"}}]},
            {"choices": []},
            {"choices": [{"delta": {"content": "2"}}]},
            {"choices": [{"delta": {}, "finish_reason": "stop"}]},
        ]
        sse = "".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
        return httpx.Response(200, text=sse + "data: [DONE]\n\n")

    def offline_client(*args, **kwargs):
        return original_client(transport=httpx.MockTransport(respond), **kwargs)

    monkeypatch.setattr(providers.httpx, "AsyncClient", offline_client)
    caplog.set_level(logging.INFO, logger="icebreaker.providers")
    model = providers.SiliconFlowLlmProvider("secret-key-canary", "deepseek-ai/DeepSeek-R1-0528-Qwen3-8B")
    assert [delta async for delta in model.reply("请只回答一加一等于几")] == ["2"]
    assert "first_content_ms=" in caplog.text and "reasoning_chars=" in caplog.text
    assert "private-reasoning-canary" not in caplog.text and "secret-key-canary" not in caplog.text
