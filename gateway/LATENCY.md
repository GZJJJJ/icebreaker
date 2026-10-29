# 2026-10-29 语音回复延迟排查

## 结论

- 28 秒并非已证实的“打断后固定卡住”。现场有一次在不修改、不重启的情况下恢复，当前未捕获此前无回复的失败现场。
- 真实测量显示取消确认约 14–23 ms、新会话确认约 0–1 ms，没有看到旧任务清理阻塞几十秒。这里只覆盖已测样本，不能排除其他取消路径的问题。
- 指定的 `deepseek-ai/DeepSeek-R1-0528-Qwen3-8B` 会先生成推理，再返回正文。日志中请求 headers 约 0.6–0.8 秒就返回，正文仍可能等约 7.5–8 秒；另一次孤立 LLM 探针甚至等到 20.6 秒。
- 对同一模型传 `enable_thinking:false` 仍收到了非空 `reasoning_content`，因此没有把该参数加入生产请求，也没有换模型。
- ASR 同一句输入有 0.6–12.1 秒波动，不能给免费云接口承诺固定延迟；测量未进一步拆出网络、服务排队与推理各自占比。
- TTS 下载约 1.6 秒，转码约 55–83 ms。FFmpeg 不是当前主要瓶颈。
- 现有架构按整轮串联 ASR → 完整 LLM 文字 → 整段 TTS → 设备接收完整 PCM → 播放，所有等待都会叠加。本轮没有冒险改回流式播放。

## 历史实机的 28 秒

来源：`C:\Users\Joe\AppData\Local\Temp\opencode\icebreaker-interrupt-live.log`，使用设备串口时间戳，不包含用户说话时长。

| 区间 | 约耗时 |
| --- | ---: |
| 第二轮结束录音 → ASR 结果到设备 | 11.33 s |
| ASR 结果 → LLM 最后文字到设备 | 8.59 s |
| 最后文字 → TTS 合成、下传、播放器启动 | 8.09 s |
| 合计：结束录音 → 播放器启动 | 28.01 s |

历史日志无法把最后一段进一步拆开，不能把 8.09 秒全部算给 TTS。

## 本轮改动

- 增加 `reply.stage` / `reply.metrics`；Gateway 日志记录各阶段和取消耗时。
- Provider 记录首个推理/正文时间及 TTS 下载/转码时间，只记录长度和耗时，不记录推理正文、密钥或 Provider 异常文本。
- `/api/stats` 保留 `reply_stage`、`last_error`、`cancel_ms`、`timings_ms`、本地 `playback_wait_ms`。
- 保留原模型和 `max_tokens:160`，只收紧口语提示词：通常一句、30 字以内；用户要求只给答案时不延伸、不反问，不强行带回咖啡话题。

## 同句真实 Provider 样本

使用 `benchmark_latency.py --runs 1`；每次一对普通会话和取消后立即新一轮。输入是固定合成语音“请只回答一加一等于几”，由电脑上传至本机 Gateway。下表阶段值取服务端 `reply.metrics`，总耗时有小量事件处理开销。

| 提示词 | 情况 | ASR | LLM（含推理等待） | TTS | 服务端发送 | 总耗时 | 回复字符 / 音频长度 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 原提示词 | 普通 | 0.860 s | 8.300 s | 1.892 s | 0.011 s | 11.064 s | 10 / 3.77 s |
| 原提示词 | 取消后新一轮 | 0.824 s | 10.283 s | 1.687 s | 0.041 s | 12.834 s | 72 / 13.37 s |
| 简短口语提示词 | 普通 | 0.892 s | 6.960 s | 1.793 s | 0.004 s | 9.651 s | 2 / 1.08 s |
| 简短口语提示词 | 取消后新一轮 | 0.622 s | 7.719 s | 1.339 s | 0.003 s | 9.683 s | 2 / 1.08 s |

这是少量非随机化样本。云端排队、生成随机性、网络均可能变化，不能把耗时差全部归因于提示词，也不能声称实机已经从 28 秒稳定降到 9.7 秒。
本探针不含 K0、真实麦克风、Wi-Fi 下传和声学出声时间；实际设备下一次对话可直接读 `playback_wait_ms`。

原始结果（不含录音、密钥和文字内容）：

- `C:\Users\Joe\AppData\Local\Temp\opencode\icebreaker-latency-before.json`
- `C:\Users\Joe\AppData\Local\Temp\opencode\icebreaker-latency-instrumented.json`
- `C:\Users\Joe\AppData\Local\Temp\opencode\icebreaker-latency-after.json`
- Provider 明细日志：`C:\Users\Joe\AppData\Local\Temp\opencode\icebreaker-gateway-latency.log`
- 当前真实 Gateway 日志：`C:\Users\Joe\AppData\Local\Temp\opencode\icebreaker-gateway-current.log`

## 验证与剩余工作

- Gateway 22 项离线测试通过；覆盖阶段计时、失败计时、慢取消清理、新一轮独立，以及只取正文、不泄露推理文本/密钥。
- 实机离线 8 轮通过：6 次完成、1 次打断、1 次预期空识别错误；覆盖两种重连、错误保留及下一轮清除，解码和播放失败均为 0。
- 实机 HTTP 静音 PCM 回归通过；本地播放器原生测试和 131072 种控制脚转换回归通过。
- 固件构建和 COM3 烧录成功，真实 Gateway 已恢复。
- 还需在新版真实 K0 会话中读取 `playback_wait_ms`，继续捕获偶发无回复的失败现场；未宣称该问题已经修复。
- 若要求稳定低延迟，下一步应由用户决定是否允许换非推理模型，或投入首句 TTS/播放器流式化；本轮未擅自实施这两项。
