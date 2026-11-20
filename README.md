<p align="right"><a href="README.en.md">English</a> | <b>中文</b></p>

# Celestia · hardware

Celestia 硬件抽象层（HAL），封装 Anyka SoC 平台的外设与多媒体能力，向上为 [`framework`](https://github.com/OpenCelestia/framework) 提供统一接口。

## 模块

| 目录 | 说明 |
|------|------|
| `camera/` | 摄像头采集 | 
| `display/` / `osd/` | 显示与 OSD 叠加 |
| `audio/` | 音频采集与播放 |
| `gpu/` | 图形加速 |
| `input/` | 输入设备 |
| `network/` | 网络 |
| `vision/` | 视觉/AI 推理接入 |
| `driver/` / `soc_platform/` | 驱动与 SoC 平台适配 |
| `include/` | 对外头文件 |

## 构建

由顶层 `repo` 工程统一构建，使用工程根目录的 `build.sh`。

## 许可

以 **Apache License 2.0** 授权，详见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。

> ⚠️ 本仓库为 HAL 抽象层；Anyka SoC 的 BSP/SDK 与厂商专有二进制**不包含**在本仓库内，也不受 Apache-2.0 覆盖。

## 贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)（采用 DCO，提交需 `-s` 签名）。
