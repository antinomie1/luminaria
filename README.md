# Luminaria

用现代 C++23 写 Wayland 混成器的库：架在 `libwayland-server` 上，用 Vulkan 渲染，通过命名模块按能力分层提供 API，没有公开头文件；并提供基于 `cxx` 的完整 Rust 绑定。

## 当前状态

真实客户端（Firefox、konsole、weston-terminal）可以连接并使用。完整功能见 **[docs/features.md](docs/features.md)**。

## 快速开始

### C++

```sh
xmake f -y --toolchain=clang && xmake -j4
xmake run minimal
```

依赖、测试、裸机运行方式见 **[docs/building.md](docs/building.md)**。

### Rust

```sh
cargo run --example minimal
# 裸机 DRM 运行：LUMINARIA_BACKEND=drm cargo run --example minimal
```

Rust 绑定使用说明见 **[bindings/rust/README.md](bindings/rust/README.md)**，API 详见 **[bindings/rust/docs/api.md](bindings/rust/docs/api.md)**。

## 文档

| 文件 | 内容 |
|---|---|
| [CONTEXT.md](CONTEXT.md) | 词汇表与术语定义 |
| [docs/architecture.md](docs/architecture.md) | 系统分层、模块结构、核心生命周期规矩 |
| [docs/api.md](docs/api.md) | C++ API 与协议接口完整参考 |
| [docs/features.md](docs/features.md) | 已实现并有测试覆盖的功能矩阵 |
| [bindings/rust/README.md](bindings/rust/README.md) | Rust 绑定概述、特性开关与快速上手 |
| [bindings/rust/docs/api.md](bindings/rust/docs/api.md) | Rust API 接口完整参考 |
| [docs/adr/](docs/adr/) | 核心架构设计决策记录（ADR） |
| [TODO.md](TODO.md) | 待落地任务与已知技术边界 |
| [AGENTS.md](AGENTS.md) | 面向 AI 代理的开发指南与开发规范 |

## 许可证

BSD 2-Clause，见 [LICENSE](LICENSE)。
