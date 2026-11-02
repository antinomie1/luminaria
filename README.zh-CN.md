# Luminaria

[English](README.md) | [简体中文](README.zh-CN.md)

用现代 C++23 写 Wayland 混成器的库：架在 `libwayland-server` 上，用 Vulkan 渲染，通过命名模块按能力分层提供 API，没有公开头文件；并提供基于 `cxx` 的完整 Rust 绑定。

## 项目定位与设计哲学

Luminaria 是一个**纯机制库**（Mechanism, not policy）。凡是每个混成器都得写一遍、写错就是 bug 的公共机制（帧调度、damage 记账、遮挡剔除、fence 编排、硬件光标合成、设备热插拔），库替你写好；凡是构成混成器个性与策略的决定（平铺算法、窗口摆位、按键绑定、外观样式），全部留给混成器开发者。

- **极低开销与按需帧调度（Demand-Driven）**：空闲桌面 **0 次 GPU 提交、0 次页面翻页、0 次 CPU 唤醒**。屏幕静止时不订阅 vblank、不空转轮询，每次帧事件严格按需索取。
- **内存安全靠构造（Memory Safety by Construction）**：全链路 RAII 包装 C 句柄，信号监听析构自动摘链，无手写 `wl_list_remove`；采用代际句柄 `SurfaceId{index, generation}` 消除悬空指针，槽复用绝不误指新客户端。
- **纯即时模式，无保留场景树（Immediate-Mode Scene）**：摒弃沉重的保留模式场景树（ADR 0001），每帧由混成器提交摆位列表；窗口位移、缩放与开闭的损坏区（damage）由前后帧摆位串 diff 自动推导，移动窗口仅重绘覆盖的矩形而非整屏。
- **纯 C++23 模块与一等公民 Rust 绑定**：无宏污染与公开头文件，严格 DAG 模块分层；Rust 侧基于 `cxx` 封装，提供按模块切分的 cargo 特性开关，零样板开箱即用。

## 核心特性

### 🚀 现代化 Vulkan 1.3 渲染流水线
- **纯 GPU 零拷贝通路**：客户端 dmabuf 经 `VK_EXT_image_drm_format_modifier` 零拷贝导入为 Vulkan 纹理，合成直接写入扫描目标（Scanout Target），KMS Atomic 翻页直出，全程无 CPU 停等与读回。
- **现代特性起步**：强制 Vulkan 1.3，全面采用动态渲染（Dynamic Rendering）、`synchronization2` 与 `timelineSemaphore`，无 Framebuffer 对象分配开销。
- **前后遮挡剔除（Occlusion Culling）**：从前往后累积不透明区域，全屏窗口下的壁纸完全不进入 GPU 采样。
- **精确 Damage 渲染**：脏区折叠为不相交矩形集，逐矩形配置 scissor 进行局部重绘；稳态下每帧零堆内存分配。
- **直出与高级效果**：全屏单窗口直出（Direct Scanout）；支持离屏合成（`OffscreenTarget`）、自定义片元着色器（SPIR-V fragment shader）及 x-ray 背景模糊。
- **CPU 降级合成器**：内置 `CpuCompositor`，支持无 GPU 环境、CI 测试及软件帧泵。

### 🖥️ 完备的后端与硬件交互
- **裸机 DRM/KMS**：原子模式设置（Atomic modeset）、多输出与热插拔（udev 监听）、硬件光标平面（光标移动零重绘）、VRR 自适应刷新率（FreeSync / G-Sync）、显式同步（`linux-drm-syncobj-v1`）。
- **Wayland 嵌套后端**：在已有 Wayland 宿主中开窗运行，支持服务端/客户端装饰协商（CSD/SSD）、父级 dmabuf 零拷贝直接呈现、宿主输入事件无缝转发。
- **Libinput 裸机输入**：集成 xkb 状态机、多指触控、触摸板手势（swipe/pinch）、数位板/笔具支持（`zwp_tablet_manager_v2`）、虚拟键盘/鼠标注入。
- **Session 会话管理**：基于 libseat 实现无特权 VT 切换，切出时自动释放 GPU 与输入控制权。

### 📦 完备的 Wayland 协议生态
- **核心与桌面外壳**：`wl_compositor` v6、`xdg_wm_base` v5、`zwlr_layer_shell_v1`（面板、壁纸、通知、锁屏）、`ext_workspace_v1`、`zwlr_foreign_toplevel_management_v1`（任务栏与窗口控制）。
- **帧与时序控制**：`wp_presentation`（真实 KMS 硬件时间戳）、`wp_tearing_control_v1`（异步撕裂上屏）、`wp_fifo_v1`、`wp_commit_timing_v1`。
- **HiDPI 与色彩**：`wp_fractional_scale_v1`（120 分之一高精度小数缩放）、`wp_viewporter`、参数化色彩管理协议（`wp_color_manager_v1`）。
- **捕获与录屏**：屏幕录制（`wlr-screencopy` / `ext-image-copy-capture`）、独立光标流捕获，以及**单窗口捕获**（`ext-foreign-toplevel-image-capture-source-v1`，独立于遮挡与最小化状态，完整支持 GPU tiled dmabuf）。
- **输入法与安全**：`zwp_text_input_v3` 与 `zwp_input_method_v2`（输入法与候选框全套）、指针约束与相对位移、失败向关（fail-closed）的绝对安全锁屏（`ext_session_lock_v1`）。

### 🦀 头等公民的 Rust 绑定
- **按需模块化编译**：与 C++ 模块一一对应的 Cargo 特性（`backend`、`render`、`shell`、`desktop`、`text`），关闭的模块不产生任何系统库链接。
- **纯粹 Rust 体验**：全链路统一的 `Event` 事件流、强类型状态与枚举、无缝支持 `mint` / `glam` 几何类型转换。

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
