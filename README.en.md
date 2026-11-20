<p align="right"><b>English</b> | <a href="README.md">中文</a></p>

# Celestia · hardware

Hardware abstraction layer for Anyka SoC peripherals and media. The public interface used by [`framework`](https://github.com/OpenCelestia/framework) is `include/`.

## Modules

| Directory | Role |
|-----------|------|
| `camera/` | Camera capture |
| `display/` / `osd/` | Display and OSD |
| `audio/` | Audio capture and playback |
| `gpu/` | Graphics acceleration |
| `input/` | Input devices |
| `network/` | Network |
| `vision/` | Vision / AI inference adapter |
| `driver/` / `soc_platform/` | Drivers and SoC platform glue |
| `include/` | Public headers |

## Build

Build from the repo workspace root with `build.sh`.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

This repository is the HAL. The Anyka BSP/SDK and vendor binaries are not in this repository and are not covered by Apache-2.0.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commits use the DCO (`git commit -s`).
