# 摄像头调研结论（2026-10-29）

## 当前推荐

先买/借 **UVC USB 摄像头**，接开发板 USB-A `HOST`，从 320×240 MJPEG、5–15 fps 主动快照开始。可优先询问正点原子配套型号；没有确认具体商品前，不把“USB 免驱、1080P”当成 ESP32-S3 兼容证明。

作为实验候选，社区有 ESP32-S3 + Logitech C270 的 UVC 测试记录；但 C270 的具体 Full-Speed 协商档位、当前板子供电和本项目驱动仍需实测，不能直接承诺买来即用。

## 原因

官方 USB UVC 资料要求：

- ESP32-S3 使用 USB Full-Speed，摄像头要能在 USB 1.1 Full-Speed 下工作；
- 摄像头应提供 MJPEG；
- 旧 `usb_stream` 文档对同步传输约束为 `<4 Mbps`、端点 MPS 不超过 512 bytes，批量模式约 `<8.8 Mbps`；
- 320×240 MJPEG 是比 640×480/1080P 更合理的第一档，视觉场景建议不需要高分辨率。

Espressif 已将旧 `usb_stream` 标为不再维护，新的方向是 `usb_host_uvc`（当前组件页显示 2.6.0，目标包含 ESP32-S3）。本项目当前仍未把 USB UVC 组件加入固件，先不与已稳定的音频链路混合改动。

## 板上接口结论

- USB-A `HOST`：连接到 ESP32-S3 原生 USB GPIO19/20，板上有 VCC5 供电；摄像头首选这里。
- Type-C `SLAVE`：也使用 GPIO19/20，与 HOST 是同一组原生 USB 线，不能当成第二个独立 USB 口；使用 HOST 摄像头时不要同时把 SLAVE 接电脑。
- 目前烧录/日志 Type-C：CH343 USB-UART，独立于 USB HOST/SLAVE，可继续使用 COM3。
- 两个 `5V/GND/TX/RX`：UART 扩展口，不是 UVC 视频接口；适合接自带图像处理和协议的串口摄像头，但不适合本项目第一版视觉方案。TX/RX 仍按 3.3V 逻辑处理，不能把 5V 直接施加到 ESP32 信号脚。
- `EX2~EX7`：XL9555 IO 扩展口，不是 ESP32 GPIO2–7，也不是高速 DVP 摄像头接口，不能用于直接接 OV2640 排线摄像头。

原理图还表明 USB HOST 的 5V 与板上 VCC5 相连；摄像头功耗要和 ES8311/功放同时工作一起测量。不要随意并联外部 5V 或反向给板子供电。

## 购买询问模板

> 我要接 ESP32-S3 原生 USB Full-Speed Host（12Mbps），不是电脑 USB High-Speed。请确认摄像头在 Full-Speed 下能输出 UVC MJPEG 320×240，并提供帧率、端点传输类型/MPS、5V 最大功耗，以及 ESP32-S3 或类似 MCU 的实测资料。

如果卖家不能回答 Full-Speed 格式和功耗，先不要买。

## 实现顺序

1. 先用一个已知兼容摄像头做 USB 枚举和 MJPEG 单帧接收。
2. 调到 320×240、低帧率，验证 PSRAM buffer 和 Wi-Fi/音频并行不丢播。
3. 增加物理遮挡或明确的摄像头开关状态。
4. 仅在用户主动触发时上传一张场景快照到 Gateway；先返回“建议场景”，不自动切换主题。
5. 视觉模型、费用、隐私告知和多人遮挡场景通过测试后，再评估低频辅助采样。
