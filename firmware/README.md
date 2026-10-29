# Icebreaker standalone firmware

这是从零开始的 ESP-IDF 固件基线，不使用 OpenVela，也不依赖现有
`icebreaker`/`xiaozhi` 固件协议。

## 当前能力

- ESP32-S3 target
- NVS 初始化
- 可选 Wi-Fi station
- Wi-Fi 获取地址后启动 HTTP server
- `GET /health` 查看状态
- `POST /api/interrupt` 触发本地停播状态
- 可配置 GPIO 中断按键
- 已验证 DNESP32S3B V1.1 / ES8311，24 kHz mono PCM16，录音上传时转为 16 kHz
- K0 第一次按下录音，第二次结束；等待回复或播报时按下可打断，再按可开始下一轮
- 单一异步播放任务；`tts.end` 后播放完整 PCM，实际播完才回到 `idle`
- ST7789 屏幕显示设备标题、当前状态和 K0 操作提示；状态显示使用英文短标签，避免额外字体依赖
- K1 选择主题，K2 加入/结束活动；活动暂停/恢复/结束也可通过 `/api/activity` 控制。活动上下文由 Gateway 保留有限轮次，结束后清理。

## 构建

在 ESP-IDF PowerShell 中执行：

```powershell
cd firmware
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

也可以直接执行项目脚本：

注意：`build.ps1` 会重新设置 target、读取 Wi-Fi 文件；保留当前配置时请直接执行 `idf.py build`，不要重复 `set-target`。

```powershell
.\build.ps1
.\build.ps1 -Flash -Port COM4
```

在 `Icebreaker firmware` 菜单中填写 Wi-Fi；确认板子原理图后再填写
`Interrupt button GPIO`。

## 烧录与验证

```powershell
idf.py -p COM4 flash monitor
```

设备联网后：

```powershell
curl http://<device-ip>/health
curl -X POST http://<device-ip>/api/interrupt
```

## 状态与打断验收

- `GET /health`：实际播放时为 `speaking`，正常播完为 `idle`；本地打断为 `interrupted`。
- `POST /api/play`、`POST /api/tone` 仅排入同一个播放器，返回 `queued:true`，不等待播完。播放期间 HTTP 状态查询和打断仍能响应。
- 打断先关闭功放并丢弃待播 PCM，再通知 Gateway；迟到的旧 TTS 不应续播。
- `/api/stats` 包含 `connected`、`player_busy`、`interrupt_us`。`bytes_played` 是提交到 I2S 的字节数，不是声学测量；`interrupt_us` 仅是软件停播耗时，不能作为 K0 到静音的实测值。
- 录音最多 30 秒；最后一帧发送完成后才发 `audio.end`。
- 自动测试不能替代 K0 的声学延迟和语音尾字验收。
- `POST /api/button` 将一次按键事件放入 K0 共用队列，仅供可信内网联调；不是直接调用播放器。
- `/health` 包含录音活动标志、实际处理的按键数和 uptime，便于区分会话卡住与设备重启。
- `/api/stats` 的 `reply_stage` 区分 `asr/llm/tts/sending/receiving/playback/ready/error`；`ready` 只说明回复已交给播放器，实际状态仍看 `/health`。
- `last_error` 保留最近本轮的 Gateway 错误代码，下一轮开始才清除；`timings_ms` 保存云端各阶段耗时，`llm_first` 是 `llm` 的子集。打断的本轮可能没有完整耗时，不能把 0 当成云端瞬间完成。
- `playback_wait_ms` 是本地发送 `audio.end` 前到播放器启动的实际等待，包含网络和整段缓冲；它仍不是声学出声延迟。

## BOX 音频控制纠错（DNESP32S3B V1.1）

对应 `ATK_DNESP32S3B_V1.1.pdf` 第 2–3 页和 BOX 的 `09_iic_exio/components/BSP/XL9555/xl9555.h`：

- XL9555 IO0_2 (`0x0004`) 是 **BEEP，低电平响**，不是扬声器功放。
- IO0_5 (`0x0020`) 是 **SPK_EN，NS4150B CTRL，高电平开启**。
- IO0_7 (`0x0080`) 是屏幕背光。
- 启动、录音、播放、停播均保持 BEEP 高电平。输出寄存器预期：空闲 `0x0084`，播放 `0x00a4`。
- 不适用另一款 `ATK_DNESP32S3_V1.2.pdf` 的 ES8388/SPK_EN 引脚表。

`GET /api/stats` 的 `xio_output` 是实际读取的 XL9555 输出寄存器（十进制），空闲应为 `132`。
`POST /api/play` 接收最多 5 秒的裸 PCM16LE、24 kHz、mono，用来绕过 ASR/LLM 直接验证扬声器语音。
这些接口仅供可信内网调试，不能公开部署。

控制脚回归测试（电脑上的 C compiler，不依赖开发板）：

```powershell
gcc -std=c11 -Wall -Wextra -Werror -I main tests/test_board_audio_control.c -o "$env:TEMP/icebreaker-audio-control-test.exe"
& "$env:TEMP/icebreaker-audio-control-test.exe"
gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I tests/stubs -I main tests/test_audio_player.c -o "$env:TEMP/icebreaker-audio-player-test.exe"
& "$env:TEMP/icebreaker-audio-player-test.exe"
```

播放器测试使用确定性的 FreeRTOS stub，覆盖内存所有权、排队/写入时取消、静音排空 DMA、下一轮及失败恢复，不模拟真实硬件时序。

可选实机回归（从项目根目录运行；设备须空闲，会发送静音 PCM，不测试 K0 或声学延迟）：

```powershell
python firmware/tests/test_device_audio.py http://<device-ip>
```

完整设备会话回归：先停止真实 Gateway，再运行下面脚本。它在电脑 8765 端口启动离线 Gateway，驱动同一个 K0 队列，覆盖连续两轮、打断、下一轮、正常 CLOSE/网络异常重连后的下一轮；录音经过真实麦克风，ASR/LLM/TTS 使用替身且输出静音，不调用云端。结束后须重新启动真实 Gateway。

```powershell
python firmware/tests/test_device_sessions.py http://<device-ip>
.\gateway\start.ps1
```
