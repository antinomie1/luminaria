# Luminaria

从零实现的极简 Wayland compositor 库，现代 C++。
架在 `libwayland-server` 上（不重造线协议），Vulkan 渲染，Meson 构建。
设计原则：**对外友好、对内高效、整体简单**。

**当前状态：** 核心通路端到端可用。真实客户端（`weston-terminal`）可连接、映射窗口、
GPU 合成，嵌套运行时可见且可交互（指针、键盘、修饰键）。

---

## 依赖（Fedora）

核心：

```sh
sudo dnf install -y \
  gcc-c++ meson ninja-build pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel \
  pixman-devel libxkbcommon-devel \
  vulkan-loader-devel vulkan-headers glslang mesa-vulkan-drivers \
  mesa-libgbm-devel libdrm-devel \
  libinput-devel libseat-devel systemd-devel
```

Xwayland（可选）：

```sh
sudo dnf install -y \
  xorg-x11-server-Xwayland libxcb-devel xcb-util-wm-devel
```

编译需 C++23 stdlib（`<expected>`），gcc ≥ 14 / clang ≥ 18，以及 Vulkan、xkbcommon、
libdrm、libinput、libudev、wayland-protocols。

---

## 构建 & 测试

```sh
meson setup build
ninja -C build
meson test -C build
```

多数测试用**进程内 `libwayland-client`**（socketpair）驱动真实协议，无需 GPU /
父 compositor；Vulkan 测试用真 GPU；`wayland-nested` 连真父 compositor（有
`WAYLAND_DISPLAY` 才跑）。

---

## 已实现并测试

### 核心底层

| 模块 | 内容 | 测试 |
|---|---|---|
| core | `Result<T>` / `Error`、`CUnique` 句柄 RAII、`Signal<Event>` + RAII `Connection`（emit 期间 connect/disconnect 安全）、`Display`、`EventLoop` + `EventSource` | signal, core |
| util | `Box`、`Color`、`Pixel`、`Rect`（constexpr） | box |

### 协议对象（服务端）

| 模块 | 内容 | 测试 |
|---|---|---|
| 协议 | `wl_compositor` + `wl_surface` — attach / damage / commit / frame 回调 | compositor |
| 协议 | `wl_shm` buffer → RGBA 读回（ARGB8888 + XRGB8888） | client-texture |
| 协议 | `xdg_wm_base` / `xdg_surface` / `xdg_toplevel` 全生命周期（配置握手 → map） | xdg |
| 协议 | `wl_seat` v5 — 键盘（xkb keymap）+ 指针，焦点 enter/leave + 事件路由 | seat |
| 协议 | `wl_output` global — geometry / mode / scale / done（客户端 map 前需要） | — |
| 协议 | `wlr-screencopy-unstable-v1` (v3) + `ext-image-copy-capture-v1` + `ext-image-capture-source-v1` — 输出截图（shm 路径，dmabuf 已预留） | — |

### 渲染

| 模块 | 内容 | 测试 |
|---|---|---|
| render | Vulkan-Hpp RAII：纯色背景、矩形填充、客户端纹理合成（真 GPU 读回验证） | vulkan, composite, texture |
| render | 纹理裁剪 — 部分越界的 surface 只渲染可见部分（之前跨输出边缘直接丢弃） | texture |
| present | `Output::commit_frame(pixels)` 呈现渲染帧（headless 存帧，DRM 写 dumb buffer） | render-output |

### 后端

| 模块 | 内容 | 测试 |
|---|---|---|
| backend | 抽象 `Backend` + `HeadlessBackend`（软件帧泵，无 GPU/显示） | headless |
| backend | `WaylandBackend`（嵌套）：连父 compositor 开窗、wl_shm 呈现合成帧；**转发父 compositor 输入**（指针 enter/leave/motion/button、键盘按键 + 修饰键），经命中测试路由到 seat | wayland-nested |
| backend | `DrmBackend`（裸机 KMS）：dumb buffer + 双缓冲 pageflip + vblank 帧泵（真机未验证，测试 skip） | drm（需 tty） |
| backend | `LibinputBackend`（裸机输入）：发出 KeyEvent / PointerMotion / PointerButton 信号 | libinput |

### 场景 / Xwayland / 示例

| 模块 | 内容 | 测试 |
|---|---|---|
| scene | 场景树 + 定位 + 命中测试 + 扁平化到渲染器 | scene |
| xwayland | 启动 Xwayland + 最小 XWM（xcb 连接、重定向 root、map/configure） | xwayland |
| example | `tinyluminaria`（嵌套/headless 参考 compositor）、`luminaria-drm-demo`、`luminaria-tty`（裸机 compositor） | tinyluminaria-smoke |
| 生命周期 | 关闭的窗口在下帧回收，无残留条目 | — |

---

## 运行

**嵌套**（在现有桌面里开一个窗口，暴露自己的 `WAYLAND_DISPLAY` 供客户端连接）：

```sh
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria &
WAYLAND_DISPLAY=<打印的 socket 路径> weston-terminal
# 或 LUMINARIA_BACKEND=headless 强制无头模式
```

**裸机**（需切到空闲 VT、停掉桌面，对该 VT 有 DRM master + 输入 ACL）：

```sh
./build/examples/luminaria-tty                 # 自动探测 /dev/dri/card*，可传 card1 显式指定
```

`luminaria-tty` 是完整裸机 compositor：DRM 输出 + libinput 输入 + wl_compositor /
xdg-shell / seat，每帧用 Vulkan 合成 mapped 客户端窗口后扫描到显示器，键盘路由到
聚焦窗口，Esc 退出。它打印自己的 `WAYLAND_DISPLAY`，从另一 VT / ssh 指客户端过来：

```sh
WAYLAND_DISPLAY=wayland-1 weston-terminal
```

---

## 完整 Compositor 路线图（缺失协议 / 部分）

达到"能日常用的完整 Wayland compositor"还需实现以下内容。按**客户端是否可用**分层。
✓=已有，✗=缺，△=部分。参照：wlroots 有 73 个 type 实现，luminaria 现有 8 个。

### P0 — 不做真实程序直接跑不起来

- [ ] **`linux-dmabuf-v1` + GBM 分配器** — 所有 GPU 客户端（浏览器 / GL·Vulkan app /
      游戏 / 视频）走 dmabuf 而非 wl_shm。现在只有 shm CPU 拷贝 → 大多数真实程序不渲染。
      需要 dmabuf import + 零拷贝 scanout。**分水岭，第一优先。**
- [ ] **explicit sync**（`linux-drm-syncobj-v1`）— 现代 GPU 帧同步
- [ ] **`xdg_popup` + `xdg_positioner`** — 菜单 / tooltip / 下拉框 / 右键菜单
- [ ] **xdg-shell 窗口状态**：maximize / fullscreen / resize / configure bounds / min-max
- [ ] **`wl_subcompositor`**（subsurface）— 视频层、装饰、很多 toolkit 依赖
- [ ] **`wl_data_device`** — 复制粘贴 + 拖放（必备）
- [ ] **primary-selection**（中键粘贴）
- [ ] △ `wl_seat`：补全 touch、pointer axis/scroll（现 touch 仅 stub）
- [ ] **光标渲染** — 无可见光标精灵；实现 `wl_pointer.set_cursor`（目前为空操作）并合成光标 surface
- [ ] **seat 焦点安全** — seat 存原始 `Surface*` 焦点指针，无 destroy listener。当前调用顺序避免了悬空解引用，但聚焦的 `wl_surface` 销毁时应显式清除焦点。

### P1 — 真实桌面必须

- [ ] **多输出 + 热插拔** + output layout + 每输出 scale/transform（现单输出固定）
- [ ] **`xdg-decoration`** — 服务端/客户端装饰协商（否则无标题栏或双标题栏）
- [ ] **`presentation-time`**（正确 vsync 反馈）— `frame` 事件目前在 commit 时触发而非实际展示时，需对齐输出真实 vblank
- [ ] **damage tracking** — 现每帧全量合成整个输出；应追踪 surface + output damage，只重绘脏区域
- [ ] **HiDPI**：`wp-fractional-scale-v1` + `wp-viewporter`
- [ ] **IME**：`text-input-v3` + `input-method-v2` — 中文/日文输入法
- [ ] **libseat** 会话管理 + VT 切换（现靠 logind ACL，不能安全切 VT）
- [ ] **光标**：`cursor-shape-v1` + 实际绘制光标（`set_cursor` 现为空实现）
- [ ] **libinput 接入 `tinyluminaria`** — 裸机键盘/鼠标仅在嵌套后端可用；DRM/TTY 路径发出了输入事件但未路由到 seat。参照嵌套的命中测试→seat 接线。
- [ ] **键图一致性** — 嵌套后端转发父 compositor 的 xkb 修饰键掩码，对照我们自己的 keymap；标准布局没问题，非美式布局/group 需采用父 keymap 或对齐。

### P2 — 完整功能 / 桌面外壳

- [ ] **layer-shell**（`wlr-layer-shell` 或 `ext-*`）— 面板 / 状态栏 / 壁纸 / 锁屏层
- [ ] **`ext-session-lock-v1`** — 锁屏
- [x] **截图/录屏**：`wlr-screencopy-unstable-v1` + `ext-image-copy-capture-v1` + `ext-image-capture-source-v1`（shm 路径；dmabuf 预留）
- [ ] **`wlr-foreign-toplevel-management`** — 任务栏列窗口
- [ ] **`xdg-activation`** — 焦点转移 / 紧急提示
- [ ] **idle**：`ext-idle-notify` + idle-inhibit（视频防息屏）
- [ ] **output-management + gamma-control**（夜间模式）
- [ ] **data-control**（剪贴板管理器）
- [ ] **relative-pointer + pointer-constraints**（游戏/3D 锁定光标）、tablet-v2、pointer-gestures

### 非协议部分

- [ ] **真正的 XWM**（现 xwayland 极简）— 窗口堆叠、焦点、`_NET_*` hints、ICCCM/EWMH、override-redirect、X 剪贴板桥、`WL_SURFACE_ID` 关联
- [ ] **DRM atomic 模式设置 + GBM**（现 `drm.cpp` 279 行，legacy 级，真机未验证）
- [ ] **渲染管线**：damage、多层混合、透明/圆角、离屏缓冲

### 建议实现顺序（最短能用路径）

1. **dmabuf import + GBM 分配器** — 不做真实程序全渲染不了
2. **xdg_popup + 窗口状态** — 解锁绝大多数 GTK/Qt app
3. **data_device（剪贴板）+ xdg-decoration**
4. **多输出 + damage + presentation-time**
5. **layer-shell + session-lock** — 有这个才算"桌面"
6. **text-input / input-method** — 中文输入

---

## 已知限制

- 嵌套后端**故意以 v1 绑定 `wl_seat`**：更高版本会发 `wl_pointer.frame`/axis 事件，
  libwayland 遇到 null listener 槽会 abort，因此只请求我们处理的 v1 事件。
- **无独立于 Shift/Ctrl 的修饰键状态**（除父 compositor 报告的之外，嵌套模式）— 实践中够用。
- **DRM 路径在当前环境未在真机上验证**（无裸 VT）。
- 抽象 `Backend`/`Output` 已就位，以上待实现项按同一模式扩展即可接入。

---

## 说明

用 `-std=c++23` 构建（依赖的最新特性是 `std::expected`，其余是 C++20/17）。内存安全
由构造保证：每个 C 句柄都 RAII 包装，每个信号监听析构自动摘链，无需手动 `wl_list_remove`。
