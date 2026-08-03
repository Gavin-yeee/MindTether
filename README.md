# MindTether

> A Windows desktop app that detects Edge browser audio playback and reminds you to stay focused during study breaks.

MindTether 是一款 Windows 学习监督工具。通过实时监听 Edge 浏览器的音频会话，自动判断你是否在观看网课；当你暂停视频开始休息时，程序进行倒计时，超时后通过蜂鸣和交互按钮提醒你回归学习。

## ✨ 功能特性

- ✅ 实时音频状态检测（毫秒级响应，避免虚假延迟）
- ✅ 图形界面，直观显示当前状态和倒计时
- ✅ 自定义休息时长（支持小数分钟）与蜂鸣次数
- ✅ 设置自动保存（MindTether_settings.txt）
- ✅ 休息超时时提供「继续学习」「再休息 5 分钟」快捷操作
- ✅ 绿色免安装，单可执行文件，低系统资源占用

## 🖥️ 技术栈

- C++17 / Win32 API
- Windows Core Audio API（IAudioSessionManager2 / IAudioMeterInformation）
- Visual Studio 2022 / MSVC
- 静态链接（/MT），无外部运行时依赖

## 🚀 开始使用

1. 从 [Releases](../../releases) 下载最新版 `MindTether.exe`。
2. 双击运行，打开 Edge 播放网课视频。
3. 暂停视频，倒计时自动启动；超时后按提示操作即可。

详细使用说明见 `readme.txt`。

## 🛠️ 编译方式

```bash
# 环境：Visual Studio 2022 (Windows)
# 打开 MindTether.sln，选择 Release x64，生成即可
