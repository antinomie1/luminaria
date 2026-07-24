# Luminaria

从零实现的极简 Wayland compositor **库**,现代 C++、内存安全,对标 wlroots。
架在 `libwayland-server` 上(不重造线协议),Vulkan 渲染,Meson 构建。

设计原则:**对外友好、对内高效、整体简单**。

> English docs: [README.md](README.md).

## 依赖(Fedora)

核心:

```sh
sudo dnf install -y \
  gcc-c++ meson ninja-build pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel \
  pixman-devel libxkbcommon-devel \
  vulkan-loader-devel vulkan-headers glslang mesa-vulkan-drivers \
  mesa-libgbm-devel libdrm-devel \
  libinput-devel libseat-devel systemd-devel
```

Xwayland(可选):

```sh
sudo dnf install -y \
  xorg-x11-server-Xwayland libxcb-devel xcb-util-wm-devel
```

## 构建 & 测试

```sh
meson setup build
meson test -C build
```

18 个测试:17 绿 + 1 skip(`drm` 需裸机 tty,桌面下自动 skip)。多数测试用
**进程内 libwayland-client**(socketpair)驱动真实协议,无需 GPU/父 compositor;
Vulkan 测试用真 GPU;`wayland-nested` 连真父 compositor(有 `WAYLAND_DISPLAY` 才跑)。

## 已实现并测试

| 模块 | 内容 | 测试 |
|---|---|---|
| core | `Result<T>`/`Error`、`CUnique` 句柄 RAII、`Signal<Event>`+RAII `Connection`、`Display`、`EventLoop`+`EventSource` | signal, core |
| util | `Box`、`Color`、`Pixel`、`RectFill`(constexpr) | box |
| render | Vulkan-Hpp RAII:清屏、矩形合成、客户端纹理合成(真 GPU 读回) | vulkan, composite, texture |
| present | `Output::commit_frame(pixels)` 呈现渲染帧(headless 存帧,DRM 写 dumb buffer) | render-output |
| 桥接 | wl_shm 客户端 buffer → RGBA → GPU 合成(真客户端端到端) | client-texture |
| 协议 | `wl_compositor`/`wl_surface`/`wl_subcompositor` | compositor |
| 协议 | xdg-shell:toplevel 全生命周期(配置握手 → map) | xdg |
| 协议 | `wl_seat` 键盘(xkb keymap)+ 指针,焦点 + 事件路由 | seat |
| scene | 场景树 + 定位 + 命中测试 + 扁平化到渲染器 | scene |
| backend | 抽象 `Backend` + `HeadlessBackend`(软件帧泵) | headless |
| backend | `WaylandBackend`(嵌套):连父 compositor 开窗、wl_shm 呈现帧 | wayland-nested |
| backend | `DrmBackend`(裸机 KMS):dumb buffer + 双缓冲 pageflip + vblank 帧泵 | drm(需 tty) |
| backend | `LibinputBackend`(裸机输入):键盘/指针事件信号 | libinput |
| xwayland | 启动 Xwayland + 最小 XWM(xcb 连接、重定向 root、map/configure) | xwayland |
| example | `tinyluminaria`(嵌套/headless)、`luminaria-drm-demo`、`luminaria-tty`(裸机 compositor) | tinyluminaria-smoke |

## 运行

**嵌套**(在现有桌面里开一个窗口,并暴露自己的 `WAYLAND_DISPLAY` 供客户端连):

```sh
./build/examples/tinyluminaria                 # 默认嵌套;LUMINARIA_BACKEND=headless 强制无头
```

**裸机**(需切到空闲 VT、停掉桌面,对该 VT 有 DRM master + 输入 ACL):

```sh
./build/examples/luminaria-tty                 # 自动探测 /dev/dri/card*,可传 card1 显式指定
```

`luminaria-tty` 是完整裸机 compositor:DRM 输出 + libinput 输入 + wl_compositor/xdg-shell/
seat,每帧把 mapped 客户端窗口用 Vulkan 合成后扫描出到显示器,键盘输入路由到聚焦
窗口,Esc 退出。它打印自己的 `WAYLAND_DISPLAY`,从另一个 VT/ssh 指个客户端过来:

```sh
WAYLAND_DISPLAY=wayland-1 weston-terminal
```

## 未实现(优化/扩展项,不阻塞核心)

- **窗口管理 UI**:移动/缩放/层叠、软件光标、指针命中焦点(现键盘焦点跟最新窗口)
- **合成质量**:`copyBufferToImage` 放纹理 —— 无缩放、无 alpha 混合、无剪裁(窗口须整个在屏内);需要时升级为纹理四边形管线
- **buffer 类型**:仅 wl_shm(ARGB/XRGB8888);dmabuf 零拷贝待接
- **会话/输出**:libseat 会话管理(VT 切换/resume)、`wl_output` global(部分客户端需要)
- **XWM 完整托管**:现 XWM 会 map/configure X 窗口,但 X↔`wl_surface` 关联(`WL_SURFACE_ID`)、ICCCM/EWMH、override-redirect 待补

抽象 `Backend`/`Output` 已就位,以上按同一模式扩展即可接入。

## 说明

用 `-std=c++23` 构建(依赖的最新特性是 `std::expected`,其余是 C++20/17)。内存安全
由构造保证:每个 C 句柄都 RAII 包装,每个信号监听析构自动摘链,无需手动 `wl_list_remove`。
