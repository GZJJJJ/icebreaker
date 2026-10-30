# Icebreaker Gateway

当前是软件联调阶段的 Gateway，默认使用 Mock provider；设备不直接接触云厂商 API 密钥。

## 启动

```powershell
cd gateway
.\bootstrap.ps1
.\start.ps1
```

Gateway 使用独立 `.venv` 和 Python 3.12.12；启动不依赖 PATH 中 `python` 的排序，也不改动 Anaconda。Anaconda 的 websockets 16 / pytest 9 不满足当前约束，缺少 `pytest-asyncio` 时测试也不会运行。

默认监听 `ws://127.0.0.1:8765`。

实机使用真实 Provider，可从项目根目录运行：

```powershell
.\gateway\start.ps1
```

此脚本固定使用 SenseVoice、指定 DeepSeek 和 Edge TTS，监听可信内网的 8765 端口。
Gateway 必须在电脑上持续运行；关闭运行它的终端后，设备无法对话。脚本不安装系统服务、不设置开机自启。
设备在服务恢复后自动重连；正常 CLOSE 和网络异常均会重连，收到 `gateway.ready` 后空闲状态恢复。

`API.txt` 可包含 `SiliconFlow：<API Key>`，Gateway 会读取此文件；环境变量 `SILICONFLOW_API_KEY` 优先级更高。选择当前账户有权限的免费 LLM 模型 ID：

```powershell
$env:ICEBREAKER_MOCK="0"
$env:SILICONFLOW_ASR_MODEL="FunAudioLLM/SenseVoiceSmall"
$env:SILICONFLOW_MODEL="deepseek-ai/DeepSeek-R1-0528-Qwen3-8B"
& .\.venv\Scripts\python.exe .\server.py
```

## 协议最小闭环

设备发送：

- `session.start`（设备每轮使用不同的 `session_id`；等待同 ID 的 `listening` 确认后才接受下行回复）
- binary PCM 音频帧，或 JSON `audio.input`
- `audio.end`
- `session.interrupt`
- `session.end`

Gateway 返回：

- `gateway.ready`
- `session.state`
- `asr.final`
- `llm.delta`
- `reply.stage`（`asr/llm/tts/sending`，附 `elapsed_ms`，不包含语音内容）
- `reply.metrics`（本轮各阶段毫秒数、结果代码、音频字节数；不包含密钥或识别文本）
- `tts.start`（`sample_rate:24000`，`format:pcm_s16le`）
- `tts.audio`（每帧最多 3200 bytes，base64 PCM16；Mock 为静音帧，真实模式来自 Edge TTS）
- `tts.end`（整段发送完毕，设备转交本地播放器）
- `error`

活动开启时可传 `hints_enabled:true`；设备在本地检测到约 25 秒静默后发送 `hint.request`，Gateway 返回 `hint.suggestion`。建议不会自动合成或播放，只作为屏幕提示；暂停、结束、打断和断线会取消提示任务。

Gateway 发送结束后返回 `session.state:idle`，只表示云端处理完成，不表示扬声器已播完。
设备必须以播放器完成回调更新实际状态。新会话、打断、结束、断线均取消旧任务并清空录音。
上行最多 30 秒（16 kHz mono PCM16）；下行最多 900 KiB；云端整轮超时为 120 秒。
异常只返回错误代码，不发送或记录 Provider 异常文本；Edge 转码取消时杀死并回收 FFmpeg 子进程。

## 回复延迟诊断

设备 `/api/stats` 保留 `reply_stage`、`last_error`、`cancel_ms` 和本轮 `timings_ms`。
`asr/llm/tts/send/total` 分别表示识别、完整文字生成、整段语音合成、服务端发送及总耗时；
`llm_first` 是 LLM 开始到首个非空白正文的等待，是 `llm` 的一部分，不能重复相加。
`cancel_ms` 是开始新轮时清理旧任务的耗时，不是 K0 的本地停播延迟。
服务端 `send` 只表示发送进入网络完成；设备仍要接收整段后才开始播放，不能当成端到端或声学延迟。
`0` 可能表示尚无测量、未执行、或小于 0.5 ms，结合阶段和错误代码判断。

日志分别记录 LLM headers/首正文/推理字符数及 TTS 下载/转码耗时，不记录推理正文。
收到 HTTP 200 不等于已经生成有效回复；思考型模型可能晚几秒才返回正文。

同一句合成语音，对比普通会话与“取消后立即开始下一轮”（真实 Provider，会发起云请求；不会从设备播放）：

```powershell
& .\.venv\Scripts\python.exe .\benchmark_latency.py --runs 1 --output "$env:TEMP/icebreaker-latency.json"
```

该探针测电脑到 Gateway 的协议耗时；不含 K0、麦克风、Wi-Fi 下传和实际出声。
免费服务有波动，少量样本不能证明固定 SLA，也不能把取消与慢请求简单当成因果。

## 验收

```powershell
.\test.ps1
```

离线测试覆盖正常会话、多轮、打断、重启/断线取消、空/超长/无效音频、ASR/LLM/TTS 失败与超时、FFmpeg 取消清理，禁止真实 Provider HTTP 请求。

ASR 使用 SiliconFlow 的 OpenAI-compatible `POST /v1/audio/transcriptions`，默认模型为
`FunAudioLLM/SenseVoiceSmall`。当前是整句上传，不是连续流式 ASR。TTS 默认使用免密的 Edge 在线语音，并通过 `imageio-ffmpeg` 将 MP3 转为 PCM16，不要求系统单独安装 `ffmpeg`。
