# DS5Dongle Linux

[English README](README.md)

基于 Linux 小棒的 DualSense 蓝牙转 USB 桥接项目。

## 项目简介

这个项目把通过蓝牙连接到 Linux 小棒的 Sony DualSense 手柄，转换成主机侧可识别的
USB 复合设备。当前实现主要面向 macOS，已经处理手柄输入、输出反馈、扬声器/耳机播放、
麦克风采集和 mic 键静音。

## 当前状态

当前版本是已经可以实际运行的开发版本。

已测试环境：

- 主机：macOS 26.1, Apple Silicon Mac mini.
- 小棒系统：Debian GNU/Linux 11 bullseye, aarch64.
- 小棒内核：Linux 5.15.0-jsbsbxjxh66+ with USB gadget support.
- 手柄：Sony DualSense wireless controller over Bluetooth HID.
- USB 主机侧模式：composite USB gadget with HID + UAC2 audio.

## 支持功能

- DualSense USB HID 身份：`054c:0ce6`，产品名为 `DualSense Wireless Controller`。
- 手柄输入以约 250 Hz 上报到主机。
- 支持按钮、摇杆、扳机、触摸板和基础体感输入转发。
- 支持灯条、玩家灯、震动和自适应扳机相关 USB 输出报告。
- 支持把 USB 播放音频通过蓝牙 Opus 音频包发到手柄扬声器/耳机。
- 支持从 DualSense 蓝牙麦克风流采集音频，并作为 USB 麦克风输入提供给主机。
- 支持 mic 键切换静音，并同步静音状态和 mic 键灯。
- 支持 DualSense tester 的扬声器/耳机 1 kHz 正弦波测试。
- HID 手柄功能和 UAC2 音频功能会一起暴露给主机。

## 已知限制

- 当前主要在一台 OpenStick 风格的 aarch64 Linux 小棒上调试和测试。其他板子可能需要调整
  USB gadget、蓝牙和 ALSA 配置。
- 轮询率暂时还没有做成运行时选项。当前 USB 输入发送周期为 4 ms，约 250 Hz。
- Pico 固件支持可配置的轮询模式；Linux gadget 这边暂时没有把 HID endpoint 的
  `bInterval` 做成简单运行时配置。
- 蓝牙扬声器/耳机音频已经可用，但仍建议视为实验性功能。
- 浏览器版 DualSense tester 可能会把 1 kHz 测试作为普通输出报告发送，而不是原生
  feature report 命令；当前版本已经针对这种行为做了处理。

## 构建

项目使用 CMake 构建，依赖 ALSA、pthreads、WDL 和 Opus。

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j2
```

输出二进制文件为：

```sh
bin/ds5_dongle_linux
```

在 Debian 系 aarch64 系统上，可以先安装构建依赖：

```sh
apt update
apt install -y build-essential cmake pkg-config libasound2-dev
```

## 运行要求

小棒需要满足：

- root 权限。
- `configfs` 挂载到 `/sys/kernel/config`。
- 内核支持 USB gadget：`libcomposite`、`usb_f_hid`、`usb_f_uac2`。
- 通过 BlueZ 和 `/dev/hidraw*` 访问蓝牙 HID。
- UAC2 gadget 音频路径对应的 ALSA PCM 设备。

先通过 BlueZ 配对并连接 DualSense，然后以 root 运行桥接程序：

```sh
sudo ./bin/ds5_dongle_linux
```

程序会创建 USB 复合 gadget，并在蓝牙 DualSense 和 USB 主机之间转发数据。

## 常用环境变量

- `DS5_ENABLE_BT_MIC=1`：启用蓝牙麦克风处理。
- `DS5_BT_MIC_BUTTON_MUTE=1`：让 DualSense 的 mic 键用于切换麦克风静音。
- `DS5_AUDIO_PACKET_MODE=legacy`：使用旧版合并音频包格式。
- `DS5_AUDIO_STATE_MODE=pico-mic`：使用当前更接近 Pico 实现的音频状态布局。
- `DS5_HID_NO_OUT_ENDPOINT=1`：当主机通过 control transfer 发送 HID 输出报告时使用的兼容模式。

## 配对与连接

需要先通过 BlueZ 把手柄配对并连接到 Linux 小棒。蓝牙 HID 设备出现后，
再以 root 运行桥接程序。

## 诊断命令

小棒上常用的检查命令：

```sh
ls -l /dev/hidraw* /dev/hidg* /dev/snd/*
bluetoothctl info <controller-mac>
```

运行时预期现象：

- `/dev/hidraw*` 对应蓝牙 DualSense。
- `/dev/hidg0` 对应 USB HID gadget。
- `/dev/snd/*` 中会出现 UAC2 播放/采集设备。
- macOS 会把 `DualSense Wireless Controller` 识别为 HID 和音频设备。

## 致谢与参考

这个 Linux 版本是在对比 Pico 版本行为和公开 DualSense 协议资料的过程中开发的。

参考资料：

- rafaelvaloto/Pico_W-Dualsense
- egormanga/SAxense
- Paliverse/DualSenseX
- controllers.fandom.com DualSense packet documentation

原始项目来源：

- https://github.com/awalol/DS5Dongle_linux
- https://github.com/awalol/DS5Dongle
