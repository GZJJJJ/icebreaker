# DNESP32S3B V1.1 摄像头选择与接口核对

## 结论

优先采用 **USB UVC、Full-Speed 下支持 MJPEG 的摄像头模组**，接板上 USB-A `HOST`。优先询问正点原子是否有明确配套当前 ESP32S3 BOX / `usb_stream` 示例的 USB 摄像头；尚未确认具体商品型号，不把通用 USB 免驱或某个传感器名称当成兼容保证。

不要为这块板直接购买裸 OV2640/OV5640 DVP 排线模块、树莓派 CSI 摄像头或仅有高速 YUY2/H.264 输出的摄像头。传感器是 OV2640 并不自动等于 USB 摄像头；需要相应的 USB 桥接与固件。

## 原理图核对

来源：`ATK_DNESP32S3B_V1.1.pdf` 第 1–3 页，PCB 丝印在第 4 页。

| 用户看到的接口 | 实际连接 | 对摄像头的意义 |
| --- | --- | --- |
| 两个 `5V/GND/TX/RX` 4pin PORT | J3 的 GPIO5/6，J4 的 GPIO18/8；供电为 VCC5 | 可作 UART 通信，但不是视频并口；只能接自带处理/协议的串口摄像头，吞吐与协议另验 |
| USB-A `HOST` | USB D−→GPIO19，D+→GPIO20，VCC→VCC5；图纸标为 USB_CAMERA | 摄像头的首选连接方式；要增加 UVC Host 驱动，不是插上就有识别 |
| Type-C `SLAVE` | 同一 GPIO19/20，连到 ESP32-S3 原生 USB | 与 HOST 共线，不是第二个独立 USB 控制器；使用摄像头时不要再接此口到电脑 |
| 目前用于烧录的 Type-C | CH343P USB-UART 桥接到 UART0 | 与 GPIO19/20 独立，可保留烧录/日志；本机 COM3 已枚举为 CH343 |
| 6pin `EX2~EX7` | XL9555 的 IO1_2~IO1_7，经 I²C 扩展 | **不是 GPIO2~7**，不是高速 DVP、SPI 或原生 I²C 摄像头排针；不适合像素时钟/图像数据采集 |

PORT 的 5V 是供电，不代表 TX/RX 可以承受 5V 电平；ESP32 信号按 3.3V 逻辑处理，未知外设不能直接接。

HOST 5V 与板上 VCC5 相连，图中没有独立的软件可控 USB 电源开关。需要核对电脑 USB 供电、摄像头功耗及板载音频负载，不能承诺该口能稳定给任意摄像头提供 500mA。需要额外供电时应重新核对电源设计，不能随意并联两个 5V 源或反灌电脑。

## 建议选购规格

- 明确支持 **UVC，USB 1.1 Full-Speed / 12Mbps 枚举**，并且在这个速度下提供 **MJPEG**，不是仅在 480Mbps High-Speed 时提供 MJPEG。
- 首次联调提供 **320×240 MJPEG，5/10/15fps 中至少一种稳定档位**；640×480 低帧率作为进一步评估项，不承诺高分辨率实时视频。
- 不要求自动对焦、摄像头麦克风、补光灯、4K；先选择简单、低功耗、能提供 USB 描述符和实测资料的模块。
- 桌面建议约 **80–100° 水平视角**作为初始选型方向，需结合安装高度/位置测试；普通镜头不能从桌子正中同时看见周围 360° 的所有参与者，不保证单镜头覆盖整桌。
- 可加物理遮挡、固定支架和明显的采集提示；先做主动快照，不默认持续上传。
- 原厂旧 `usb_stream` 的 isochronous 示例限制包括端点 MPS ≤512 bytes、视频流约 <4Mbps；这是旧驱动约束，**不是所有新 UVC 驱动或 ESP32-S3 硬件的通用上限**。选型保守起步，后续按选定驱动版本与 USB 描述符验证。

可直接问卖家：

> 我要接 ESP32-S3 的原生 USB Full-Speed Host（12Mbps），不是电脑 USB High-Speed。请确认摄像头在 Full-Speed 下能输出 UVC MJPEG 320×240，提供帧率、端点传输类型/MPS、5V 最大功耗，以及 ESP32-S3 实测或 USB 描述符。有无配套正点原子 DNESP32S3B BOX 例程验证？

若卖家只回复“USB2.0免驱/1080P”但不回答 Full-Speed 下的具体格式，不建议直接购买。商品链接或型号确认后再核对，不代用户下单。

## 驱动与资源

随板 ES8311 验收工程含 `main/APP/lv_camera_ui.c` 与 `managed_components/espressif__usb_stream`，证明原厂走 USB 摄像头路线，而不是 DVP。

截至本次查阅，Espressif 将旧 `usb_stream` 标为停止维护，新接入优先评估维护中的 `usb_host_uvc`（页面版本 2.6.0），再核对 ESP-IDF 5.5.6 兼容性；本轮未把任一 USB 驱动加入语音固件。

音频继续使用 ES8311 I²S，不启用摄像头自带 UAC 音频。先验收单张 JPEG、权限开关和云端建议，再做低频采样及与 Wi-Fi、LCD、PSRAM、播放/打断并行的压力测试。不可把板上有 HOST 接口当成已完成驱动和电源验证。

如果希望马上先验证视觉模型，也可以临时把 USB 摄像头接现有 Gateway 电脑，但这只能是实验接法，不能声称板载摄像头已接通。

## 参考

- 本地原理图：随板资料 `3，原理图/ATK_DNESP32S3B_V1.1.pdf`。
- 本地原厂示例：ES8311 屏验收代码中的 `main/APP/lv_camera_ui.c`。
- [Espressif 旧 USB Stream 指南](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_stream.html)
- [维护中的 usb_host_uvc](https://components.espressif.com/components/espressif/usb_host_uvc)
