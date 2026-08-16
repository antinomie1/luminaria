# Luminaria

用现代 C++ 写 Wayland 混成器的库：架在 `libwayland-server` 上，用 Vulkan 渲染，通过 C++20 named module 按能力提供 API，没有公开头文件。

## 当前状态

真实客户端（Firefox、konsole、weston-terminal）可以连接并使用。完整功能见 **[docs/features.md](docs/features.md)**。

## 快速开始

```sh
xmake f -y --toolchain=clang && xmake
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria
```

依赖、测试、裸机运行方式见 **[docs/building.md](docs/building.md)**。

## 文档

| 文件 | 内容 |
|---|---|
| [CONTEXT.md](CONTEXT.md) | 词汇表 |
| [docs/architecture.md](docs/architecture.md) | 架构、模块结构、实现细节 |
| [docs/features.md](docs/features.md) | 已实现并有测试覆盖的功能 |
| [docs/adr/](docs/adr/) | 不可逆的设计决定及其理由 |
| [TODO.md](TODO.md) | 未完成的工作与执行顺序 |

## 许可证

BSD 2-Clause，见 [LICENSE](LICENSE)。
