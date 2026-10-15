# BetterFace for OBS

[English](README.md)

适用于 Apple 芯片 Mac 的原生 OBS 美颜滤镜：多人磨皮、瘦脸、美白、红润，以及淡化黑眼圈和法令纹。
在 OBS 中直接添加滤镜，使用 macOS 自带的 Vision 处理人脸。

## 下载与安装

前往 [v2.0.5 发布页](https://github.com/vanawaker/BetterFaceOBS/releases/tag/v2.0.5)，下载
`BetterFace-for-OBS-2.0.5.pkg`。同页提供源码、安装说明、许可证和 SHA-256 校验文件。

1. 完全退出 OBS，再运行安装包。
2. 安装包没有经过 Apple 公证；如被系统拦截，可在“系统设置 → 隐私与安全性”中允许打开。
3. 重启 OBS，在摄像头源的“滤镜”中添加 **BetterFace for OBS**。
4. 从较低强度开始调整，并使用实际摄像头、灯光及推流负载试播。

完整步骤和卸载方法见[安装说明](dist_template/安装说明.txt)。

## 环境要求

- Apple 芯片 Mac，macOS 13 或更高版本。
- Apple 芯片版 OBS Studio 31 或更高版本；开发验证使用 OBS 32.2.2。
- 当前不支持 Windows、Linux 和 Intel Mac，未逐一验证上述范围内的所有系统与 OBS 组合。

## 功能与限制

| 功能 | 当前状态 |
| --- | --- |
| 磨皮、瘦脸、美白、红润、淡化黑眼圈和法令纹 | 可用 |
| 多人同框 | 最多处理 6 张脸，所有人共用参数 |
| 单独调整某个人 | 尚未实现 |
| 脖子、耳朵磨皮 | 可选 |
| 背景保护 | 禁用，不能开启 |
| 故障时输出原画 | 可检测的转换、资源和渲染故障会停用该滤镜实例的处理 |

瘦脸可能带动紧贴脸部的背景线条。遮挡、侧脸、小脸及复杂光线都会影响识别和效果。
触发故障保护后，修改参数或开关滤镜不会恢复处理；请在直播结束后重建滤镜或重启 OBS。
保护机制不提供进程隔离，不能保证拦住所有系统框架卡死、驱动故障或原生崩溃。

## 构建

需要 Xcode Command Line Tools、Git、Python 3 和已安装的 OBS。

```sh
git clone https://github.com/vanawaker/BetterFaceOBS.git
cd BetterFaceOBS
./tools/fetch_obs_headers.sh
./tools/build_plugin.sh 2.0.5
./tools/package.sh 2.0.5 build/BetterFace.plugin
```

构建和打包不会自动安装插件或重启 OBS。输出在 `dist/`，已有同名安装包不会被覆盖。
开发和测试方法见 [DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 隐私与贡献

人脸检测和美颜在本机 OBS 进程内完成，插件没有网络客户端或统计上报。
开发用诊断数据可能含有人脸关键点，分享前需要自行检查。
公开文件不包含私人素材、凭据或本机用户路径；检查范围见[发布隐私检查](docs/PRIVACY_REVIEW.md)。

欢迎提交问题和改进，请先阅读[贡献说明](CONTRIBUTING.md)。
后续计划包括逐人参数调整和背景补偿，当前版本不包含这些功能，也不承诺上线日期。
本项目与 Facebetter 和 OBS Studio 没有隶属关系。

## 许可证

Copyright (C) 2026 vanawaker and BetterFace contributors.

采用 **GPL-2.0-or-later**，即 GNU GPL 第 2 版或任何后续版本，按许可证条款允许使用、修改和再分发，
不提供担保。参见 [LICENSE](LICENSE) 和[第三方说明](THIRD_PARTY_NOTICES.md)。
