# 破冰小子（Icebreaker）

面向主题咖啡馆多人桌的 AI 社交主持设备原型。

## 项目目标

### 当前 MVP

- DNESP32S3B V1.1 设备联网
- K0 按键控制录音开始/结束
- Gateway 串联 SenseVoice ASR → DeepSeek LLM → Edge TTS
- ES8311 + 板载扬声器播放
- 播报中本地打断，旧音频清理，下一轮可恢复
- ST7789 状态屏显示联网、聆听、思考、播报、打断和错误状态

### 当前验收线

- 连续多轮会话
- 打断后重新录音和回复
- Gateway 断线重连
- 空识别、Provider 失败、超长音频和播放失败恢复
- 本地功放控制正确：蜂鸣器关闭，扬声器播放时开启

### 暂不纳入

- 摄像头和视觉理解
- OpenVela 迁移
- 商业后台、计费和门店设备管理
- 免费云服务上的固定低延迟 SLA

## 目录

- `firmware/`：ESP-IDF 固件
- `gateway/`：Python WebSocket Gateway 和 Provider 适配
- `tools/`：硬件联调工具
- `gateway/LATENCY.md`：语音链路延迟测量与限制
- `firmware/README.md`：板级构建、烧录和验收说明

## 安全边界

凭据、Wi-Fi 配置、flash dump、构建产物和本地会话文件不提交到仓库。Provider API Key 只从项目根目录本地配置或环境变量读取。

## 下一步

1. 主题选择和主持人会话状态
2. 屏幕交互布局和用户提示
3. 流式 ASR/TTS 评估，减少首音等待
4. 摄像头接入前先完成隐私开关和授权流程
