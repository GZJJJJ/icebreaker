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

启动后打开 `http://127.0.0.1:8765/` 可进入本地调试面板。面板通过独立的 `/debug` WebSocket
旁听所选设备的新事件，内存显示 `asr.final`、`llm.delta`、`hint.suggestion`、处理耗时和错误；不回放历史，不转发音频，也不写入对话或图片文件。
先勾选已取得在场人员许可，再点击“拍摄一次快照”，才会向设备发出一次 `camera.capture`；每次拍摄都需重新勾选。
设备停止视频流后上传一帧 JPEG，图片只传至本机 Gateway，不送视觉模型或云端。支持取消，20 秒未完成自动超时。
结束活动、设备/调试通道断开、关闭页面、切换设备或手动清空时，清理页面文字和图片；关闭发起拍摄的页面也会取消该次拍摄。
面板的 HTTP 和 WebSocket 仅接受 loopback 连接并校验 Host/Origin；即使 Gateway 监听 `0.0.0.0`，局域网也不能访问面板。
设备 WebSocket 本身仍是可信内网开发接口，未添加设备身份认证，不能暴露到公网。

需要先重启 Gateway 才能加载面板代码。对话文字可配合旧固件查看；手动拍照必须构建并烧录新版固件，旧固件会显示“快照需更新固件”，不会发送拍摄指令。
调试订阅队列最多 128 条，页面仅保留最新一轮文字和最近 100 个事件；慢订阅发生丢帧时会清空不完整文字并提示。
调试面板不落盘不等于整条语音链路不落盘：现有 TTS 仍会创建并清理临时音频文件，ASR/LLM/TTS 仍使用各自云服务。

## 活动基础旁听

`activity.start` 的 `consent:true` 表示参与者已获告知并加入活动，也包括语音上下文采集，无额外旁听开关。设备不在上电、待机、暂停或结束时自动旁听；摄像头和冷场提示权限保持独立。

新固件自动上传 VAD 切出的语音片段，Gateway 只做 ASR 并更新最近 12 段、每段最多 500 字的内存上下文，不对每段调用 LLM/TTS。冷场建议与 K0 回复均可参考这些聊天内容；不识别说话人身份。暂停取消在途转写和排队片段，结束/断线清除活动内容，恢复后使用新 ID 拒绝迟到音频。

沿用 SenseVoice 云 ASR，每段最长等待 20 秒。队列仅容纳一个在途请求和一个最新待识别片段，过载替换旧片段并报告 `ambient.gap`，不承诺转写完整或延迟已改善。K0 问答优先，会取消在途旁听识别；设备处理主动问答和播报期间暂停旁听，尚不具备全双工回声消除。

面板“同桌聊天转写”显示最新一段和 `audio_ms/asr_ms`，与“语音识别原文 / AI 回复”分开；ASR 错误不会结束活动或自动重放旧语音。“清空面板”只清显示，不停止设备旁听；停止使用 K2 结束或 `/api/activity` 暂停。

协议：`ambient.speech` 撤销旧冷场建议；binary `IBAMBI1\0` + 32 字节 `activity_id` + PCM 是上下文片段，不得按 K0 问答 PCM 处理。`ambient.final/error/gap` 只供状态/调试，设备不会把它们当成主动回复。

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

新版设备显示建议正文，K1 短按收起、长按切换提示权限。`hint.request` 可带 1..4294967295 的整数 `request_id`，Gateway 在建议及对应错误中原样回传；不带编号的旧客户端保持原协议。新版固件只接受当前请求编号，部署时需同步更新并重启 Gateway，不能只烧录固件而保留旧 Gateway。

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

面板 DOM/WebSocket 逻辑测试（可选 Node.js，从项目根目录运行）：

```powershell
node --test gateway/tests/test_debug_ui.cjs
```

此测试使用 DOM stub，不等于真实浏览器目视验收；Gateway 测试另覆盖真实本地 HTTP/WebSocket 连接、权限、事件转发和快照封装。

ASR 使用 SiliconFlow 的 OpenAI-compatible `POST /v1/audio/transcriptions`，默认模型为
`FunAudioLLM/SenseVoiceSmall`。当前是整句上传，不是连续流式 ASR。TTS 默认使用免密的 Edge 在线语音，并通过 `imageio-ffmpeg` 将 MP3 转为 PCM16，不要求系统单独安装 `ffmpeg`。
