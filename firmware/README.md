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
- 单一异步播放任务；`tts.end` 后播放完整 PCM，实际播完后活动中回到 `hosting`，未加入时回到 `idle`。
- ST7789 使用中文状态和操作提示；加入前提示语音上传云端，活动中显示“聆听中”。
- K1 选择主题；K2 加入已告知语音处理方式的活动，自动启用基础旁听，无额外旁听开关。活动中 K2 结束并停止采集；暂停/恢复也可通过 `/api/activity` 控制。加入前须向所有参与者说明语音分段发送至云端 ASR，不把一人的按键视为替未获告知者同意。
- 活动旁听按 100 ms 本地 VAD 分段：保留 300 ms 前导、约 800 ms 停顿结束、最长 8 秒，少于 400 ms 有声片段丢弃；纯静默不上传。K0 主动问答和扬声器播报期间暂不旁听，恢复时丢弃 300 ms RX 缓冲；尚无回声消除，不承诺全双工或无丢字。
- 旁听转写仅更新同桌聊天上下文，不逐句触发 LLM/TTS。冷场提示仍单独由 K1 长按切换，约 25 秒静默后请求建议，冷却约 60 秒，不自动播报。出现新语音会撤销旧建议并取消待生成提示。
- 冷场建议正文在 320×240 屏幕中央显示，最多 80 个 Unicode 字符、5 行；超长截断带省略号，未知字符显示方框。顶部保留主题，底部保留采集状态与操作说明，不自动播放建议。
- 有建议时 K1 短按收起；K1 长按仍切换提示权限。开始说话、主动问答、暂停、结束、关闭提示或断线会清除正文。异步显示任务不在 WebSocket/录音回调内等待 LCD DMA。
- 字体采用 Flash 内嵌的 Icebreaker Text 16（GNU Unifont 的 OFL-1.1 子集），约 672 KiB，不复制整套字库到 RAM；来源、许可证与重建方式见 `fonts/README.md`。

文字提示新版要求同步更新 Gateway：`hint.request` 的 `request_id` 会被原样回传，固件只接受当前待处理请求，防止收起后迟到的旧建议重新显示。正常运行版本尚未部署；工作区包含此前的活动旁听功能，烧录正常版本会一并部署，加入活动会启用分段语音云端转写。

2026-11-10：已烧录专用 **文字验收版**，实机协议检查通过：建议接收、K1 收起、迟到编号拒绝、长文本接收、暂停/结束清理及重新加入。验收未调用云 Provider、未上传音频或图片；用户已目视确认屏幕显示正常。此结论不代表正常模式语音或旁听实机验收通过，设备仍保留文字验收版。

文字验收使用 CMake 开关 `-D ICEBREAKER_TEXT_UI_TEST=ON`：不启动采音任务，不执行启动麦克风探测，K1 短按在没有建议时请求测试话题。屏幕底部明确标注“不采音、不播音”。配套 `tests/test_device_hint_display.py --serve` 替换 Gateway，拒绝非验收固件，活动 ID 保持空；另一个终端运行 `--check` 完成检查并保留最终合成话题。该服务只处理合成文字，不发送 TTS 或摄像头命令。

验收模式不是正常运行版本。恢复前停止文字验收服务，使用 `idf.py -D ICEBREAKER_TEXT_UI_TEST=OFF build` 构建并烧录正常固件，再启动已更新的 Gateway。正常模式加入活动会启用基础旁听，必须提前告知参与者。编译缓存会记住该开关，切换时须明确指定 ON/OFF。
- USB-A HOST 已接入官方 `usb_host_uvc`；启动时只初始化 USB Host，不自动开摄像头。收到 Gateway 调试面板的显式 `camera.capture` 后，才采集并上传一帧 `320x240 MJPEG @ 25 FPS` 快照，验证 JPEG 起止标记后停止采集。

手动快照使用一次性的 `capture_id` 与 `consent:true`，JPEG binary 消息带 `IBJPEG1\0` + 32 字节 ASCII `capture_id` 的头部，与麦克风 PCM 分开识别。
拍摄失败后不自动重试；K0 打断、活动暂停/结束、网络断开或面板取消均停止待拍/采集流程。图片缓冲在每次请求结束后释放，不保留在设备上。
JPEG 起止标记只作初步检查，浏览器预览失败会显示解码错误。新固件尚需实机验证重复拍摄、USB 热插拔及并行语音；编译通过不等于这些验收已通过。

旁听 binary 消息为 `IBAMBI1\0` + 32 字节 ASCII `activity_id` + PCM16LE/16 kHz mono。
活动暂停/恢复会更换 ID，旧音频不能混入新活动。需同步更新并重启 Gateway；老固件不支持旁听，面板会提示更新。
`GET /health` 的 `capture_active` 仍仅代表 K0 录音；旁听看 `ambient_listening`、`ambient_segments_sent`、`ambient_upload_failures`。这些计数是本次开机以来的累计值，不包含音频正文。

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
gcc -std=c11 -Wall -Wextra -Werror -I tests/stubs -I main tests/test_app_state.c -o "$env:TEMP/icebreaker-app-state-test.exe"
& "$env:TEMP/icebreaker-app-state-test.exe"
gcc -std=c11 -Wall -Wextra -Werror -I main tests/test_ambient_capture.c -o "$env:TEMP/icebreaker-ambient-test.exe"
& "$env:TEMP/icebreaker-ambient-test.exe"
gcc -std=c11 -Wall -Wextra -Werror -I main tests/test_hint_state.c -o "$env:TEMP/icebreaker-hint-state-test.exe"
& "$env:TEMP/icebreaker-hint-state-test.exe"
gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I tests/stubs -I main tests/test_audio_player.c -o "$env:TEMP/icebreaker-audio-player-test.exe"
& "$env:TEMP/icebreaker-audio-player-test.exe"
gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I tests/stubs -I main tests/test_usb_diag.c -o "$env:TEMP/icebreaker-camera-test.exe"
& "$env:TEMP/icebreaker-camera-test.exe"
```

播放器测试使用确定性的 FreeRTOS stub，覆盖内存所有权、排队/写入时取消、静音排空 DMA、下一轮及失败恢复，不模拟真实硬件时序。
状态单测覆盖活动进行中返回 `hosting`、暂停保持 `paused`、未加入或结束返回 `idle`；实机会话回归另检查播报结束和恢复活动后的状态，避免单轮完成后停掉本地冷场检测。
相机测试同样使用 stub，覆盖启动不采集、请求 ID、忙碌拒绝、排队/采集中取消、帧复制、超时、USB 错误和重复拍摄，不替代实机 USB 验收。

可选实机回归（从项目根目录运行；设备须空闲，会发送静音 PCM，不测试 K0 或声学延迟）：

```powershell
python firmware/tests/test_device_audio.py http://<device-ip>
```

完整设备会话回归：先停止真实 Gateway，再运行下面脚本。它在电脑 8765 端口启动离线 Gateway，驱动同一个 K0 队列，覆盖连续两轮、打断、下一轮、正常 CLOSE/网络异常重连后的下一轮；录音经过真实麦克风，ASR/LLM/TTS 使用替身且输出静音，不调用云端。结束后须重新启动真实 Gateway。

```powershell
python firmware/tests/test_device_sessions.py http://<device-ip>
.\gateway\start.ps1
```
