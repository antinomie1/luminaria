# 构建、测试、运行

> 面向想把 Luminaria 编起来跑一遍的人。设计与结构见 [architecture.md](./architecture.md)。

## 依赖（Fedora）

核心：

```sh
sudo dnf install -y \
  gcc-c++ xmake pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel \
  libxkbcommon-devel \
  vulkan-loader-devel vulkan-headers glslang mesa-vulkan-drivers \
  mesa-libgbm-devel libdrm-devel \
  libinput-devel libseat-devel systemd-devel
```

编译需 **clang ≥ 22**（`xmake f --toolchain=clang`）或 **gcc ≥ 16** —— 更早的版本编不动
模块分区。目前实际验证过的是 clang 22：库、示例与全部测试构建通过，`xmake test` 全绿。
另需 Vulkan、xkbcommon、
libdrm、libinput、libudev、**libseat**、wayland-protocols，外加
**`glslangValidator`（glslang 包）**—— 渲染器的纹理四边形管线用它把 GLSL 编成 SPIR-V 并
直接嵌进二进制（运行时不读 shader 文件）。

运行 GPU 路径需要 **Vulkan 1.3** 驱动（loader 与设备都要 1.3，见
[ADR 0007](./adr/0007-vulkan-1.3-minimum.md)），并支持 `VK_KHR_external_memory_fd`、
`VK_EXT_external_memory_dma_buf`、`VK_EXT_image_drm_format_modifier`。Mesa 的 RADV / ANV /
NVK / Turnip 与 NVIDIA 专有驱动都满足；没有的机器上 `VulkanRenderer::create()` 报错说明原因，
`SceneRenderer` 退到 CPU 合成。

调试渲染器时设 `LUMINARIA_VK_VALIDATION=1` 打开 `VK_LAYER_KHRONOS_validation`（需另装
`vulkan-validation-layers`），告警打到 stderr；再加 `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`
连同步一起校验。测试套件在两者下都应零告警：

```sh
LUMINARIA_VK_VALIDATION=1 VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true xmake test
```

光标主题不需要 libXcursor：XCursor 文件格式的解析器就在
`cursor_theme.cppm` 里（含主题继承与动画帧），免去把 X11 拖进 Wayland
compositor。

---

## 构建 & 测试

```sh
xmake f -y      # 配置：顺带跑 wayland-scanner 和 glslangValidator
xmake           # 构建库与示例
xmake build -a  # 连测试二进制一起
xmake test      # 跑全部测试
xmake f -m release && xmake   # 优化构建（默认是 debug）
```

**内存要看着点。** clang 编一个模块单元要 1–2 GB，xmake 默认的并行度约等于线程数，
20 线程的机器上能吃满 32 GB 再压垮 swap，整机卡死。内存不宽裕就限并行度，或者干脆把
构建关进一个有上限的 cgroup，超了只杀构建：

```sh
xmake build -a -j4
systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0 xmake build -a -j4
```

Rust 绑定的 `build.rs` 把 cargo 的并行度原样交给 xmake，所以那边用 `cargo build -j4`。

多数测试用**进程内 `libwayland-client`**（socketpair）驱动真实协议，无需 GPU /
父 compositor；Vulkan 测试用真 GPU；`wayland-nested` 连真父 compositor（有
`WAYLAND_DISPLAY` 才跑）。跑不了的测试以 **exit 77** 自我跳过（无 GPU / 无空闲 VT /
无 seat），`xmake test` 会把原因打出来并记为通过。

## 运行

示例只有一个：`examples/minimal.cpp`，与 `bindings/rust/examples/minimal.rs` 是一对——
同样的窗口策略（每个窗口最大化到输出上、新的在最上面、点击置顶），同样的快捷键：
Logo+Return 开 `$TERMINAL`（默认 foot）、Logo+Tab 轮换、Logo+q 关窗、Logo+Escape 退出。
光标与帧回调全由库处理（`Cursor`、`SceneRenderer`），示例里只剩混成器自己的决定。
Rust 那份只有嵌套与 headless：绑定还不报告 `DrmBackend` 找到的输出。

**嵌套**（在现有桌面里开一个窗口，暴露自己的 `WAYLAND_DISPLAY` 供客户端连接）：

```sh
xmake run minimal &
WAYLAND_DISPLAY=<打印的 socket 名> weston-terminal
# 或 LUMINARIA_BACKEND=headless 强制无头模式；LUMINARIA_EXIT_MS=500 到点自动退出
```

**裸机**（需切到空闲 VT、停掉桌面，对该 VT 有 DRM master + 输入 ACL）：

```sh
LUMINARIA_BACKEND=drm xmake run minimal          # 自动探测 /dev/dri/card*，可传 card1 显式指定
```

裸机上它是完整的 compositor：DRM 输出 + libinput 输入 + libseat，GPU 合成或直出，硬件光标
平面，Ctrl+Alt+Fn 切 VT。它打印自己的 `WAYLAND_DISPLAY`，从另一 VT / ssh 指客户端过来：

```sh
WAYLAND_DISPLAY=wayland-1 weston-terminal
```

**真机验证并留下证据**（TODO 第 0 项）：`scripts/tty-check.sh` 把运行环境记下来、给
compositor 的输出打时间戳、按需拉起一个客户端，退出后在 `logs/tty-check-<时间>/` 生成
`report.md` —— 模式设置、硬件光标平面、GPU 直出、VT 切换、热插拔、空闲翻页数逐项勾出来，
连同原始日志。屏幕被接管之前它会先把要做的动作列出来。

```sh
./scripts/tty-check.sh                 # 自动挑客户端
./scripts/tty-check.sh --no-client     # 只跑 compositor
```

**截图 / 录屏**（compositor 跑起来后，任一后端，指向它的 `WAYLAND_DISPLAY`）：

```sh
sudo dnf install -y grim wf-recorder            # 依赖 screencopy 协议的现成客户端
WAYLAND_DISPLAY=<socket> grim shot.png          # 整屏截图 → PNG
WAYLAND_DISPLAY=<socket> wf-recorder -f out.mp4 # 录屏（Ctrl-C 停）
```

`grim` 走 `wlr-screencopy-unstable-v1`，把当前帧的 GPU 合成结果拷进它自己的 shm
buffer；`wf-recorder` 逐帧拉流。示例 compositor 已注册截图 manager 并挂好每个
输出的 capture 回调。

---
