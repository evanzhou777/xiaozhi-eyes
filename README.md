# xiaozhi-eyes

小智（xiaozhi-esp32）OLED 显示层「大眼睛」补丁 —— 适配 **0.91 寸 SSD1306 128×32** 面包板 WiFi 版。

把原来左侧的静态 emoji 表情换成自己画的两只大眼睛：待机随机眨眼，不同情绪下形状不同（开心变弯、生气变窄、困了半闭、惊讶变大、思考时一高一低、眨眼时闭一只眼）。

## 用法

1. Actions → **Build XiaoZhi Eyes Firmware** → Run workflow
2. 等 15~25 分钟，下载 artifact `firmware-eyes-128x32`
3. 烧录（ESP32-S3）：
   - `app-eyes-128x32.bin` → 地址 `0x20000`（保留 WiFi 配网与设备绑定）
   - `merged-eyes-128x32.bin` → 地址 `0x0`（整包，需重新配网）

## 原理

CI 拉取上游 `78/xiaozhi-esp32` 的 **v2.5.2** tag，用本仓库的 `oled_display.cc/.h` 覆盖 `main/display/`，再执行：

```
python scripts/build.py bread-compact-wifi --name bread-compact-wifi --language zh-CN
```

`bread-compact-wifi` 这个变体对应 `CONFIG_OLED_SSD1306_128X32=y`（0.96 寸屏用 `bread-compact-wifi-128x64`）。

## 注意

补丁基于 **v2.5.2** 源码。上游 `main` 分支的 `SetEmotion` 已改为返回 `bool` 并新增 `RemoveChatMessage`，直接套在 main 上会编译失败。要跟进新版本需重新适配。
