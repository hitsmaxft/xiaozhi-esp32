# 产品链接

[微雪电子 ESP32-S3-RLCD-4.2](https://www.waveshare.net/shop/ESP32-S3-RLCD-4.2.htm)

## Agent 界面

这块 400 × 300 单色屏使用固定的三段布局：

| 区域 | 位置 | 内容 |
|---|---:|---|
| 顶部状态区 | y = 0–42 | Agent 标识、设备状态或时间、网络/静音/电池图标 |
| 中间显示区 | y = 50–215 | 大尺寸单色表情、远程图片预览或临时位图 |
| 底部对话区 | y = 224–299 | 说话者及最近一条对话；空闲时显示按键提示 |

`CustomLcdDisplay` 保留 XiaoZhi 的 `SetStatus`、`SetEmotion`、`SetChatMessage` 和
`SetPreviewImage` 接口。Device Use 的 `screen.render` 调整中间表情，
`screen.bitmap` 在中间区域显示指定时长；新表情会结束临时位图，图片预览会替换它。
对话和媒体链路不依赖屏幕刷新。状态与对话区域不会因为位图出现而被覆盖。

界面只用纯黑和纯白，以匹配 ST7305 的 1 bpp 输出；`IsMonochrome()` 也向
XiaoZhi 的设备信息报告这一能力。

# 编译配置命令

**克隆工程**

```bash
git clone https://github.com/78/xiaozhi-esp32.git
```

**进入工程**

```bash
cd xiaozhi-esp32
```

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig**

```bash
idf.py menuconfig
```

**选择板子**

```bash
Xiaozhi Assistant -> Board Type -> Waveshare ESP32-S3-RLCD-4.2
```

**编译**

```ba
idf.py build
```

**下载并打开串口终端**

```bash
idf.py build flash monitor
```
