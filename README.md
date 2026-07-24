# Luminaria

从零实现的极简 Wayland compositor 库，现代 C++。
架在 `libwayland-server` 上（不重造线协议），Vulkan 渲染，Meson 构建。
设计原则：**对外友好、对内高效、整体简单**。

**当前状态：** P0 全部完成 —— 真实程序跑得起来所需的协议已齐。真实客户端
（`weston-terminal`）可连接、映射窗口、GPU 合成，嵌套运行时可见且可交互（指针、键盘、
修饰键、滚轮）。菜单/下拉框（`xdg_popup`）、subsurface、窗口状态（最大化/全屏/交互式
移动缩放）、复制粘贴与拖放（`wl_data_device` + primary-selection）、光标渲染、触摸、
显式同步（`linux-drm-syncobj-v1`）均已实现并有测试。GPU 客户端经 `linux-dmabuf-v1`
交 dmabuf buffer；截图/录屏（`grim` / `wf-recorder`）可用。

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
| 协议 | `linux-dmabuf-unstable-v1` (v3) + GBM 分配器 — GPU 客户端 dmabuf buffer 导入（ARGB8888 / XRGB8888，**任意 GPU modifier**：LINEAR 走 mmap，tiled/vendor modifier 走 Vulkan external-memory 导入）；广告 GPU 实际支持的 modifier 列表 | dmabuf |
| 协议 | `xdg_wm_base` v5 / `xdg_surface` / `xdg_toplevel` 全生命周期（配置握手 → map/unmap）；窗口状态机：maximize / fullscreen / activated / resizing 随 configure 下发，title / app_id / min-max size / window geometry 全部记录，交互式 move/resize 与 minimize 以信号交给 compositor 仲裁；`configure_bounds`（v4）+ `wm_capabilities`（v5） | xdg, toplevel-state |
| 协议 | `xdg_popup` + `xdg_positioner` v3 — 菜单 / tooltip / 下拉框：anchor / gravity / offset 完整求解，`grab`（点击外部关闭）、`reposition` + `repositioned`、父级销毁时级联 `popup_done` | popup |
| 协议 | `wl_subcompositor` / `wl_subsurface` — 子表面树、相对定位、`place_above`/`place_below` 堆叠、**sync/desync 语义**（同步子表面的 commit 缓存到父级 commit 时原子提交）；`Surface::surface_tree()` / `surface_at()` 供渲染与命中测试共用 | subsurface |
| 协议 | `wl_seat` v5 — 键盘（xkb keymap）+ 指针 + 触摸，焦点 enter/leave + 事件路由；滚轮（平滑 axis / 离散 notch / axis_stop）、`set_cursor`（发信号给 compositor 合成光标）；**焦点安全**：seat 订阅 `Surface::destroy`，被销毁的聚焦表面自动清空，无悬空指针 | seat, seat-input |
| 协议 | `wl_data_device_manager` v3 — 剪贴板（选区随键盘焦点转移）+ 拖放（drag 期间 seat 把指针交给 data device，enter/motion/drop 全流程，dnd actions）；数据经管道在客户端之间直传，compositor 不读内容 | data-device, dnd |
| 协议 | `zwp_primary_selection_device_manager_v1` — X11 式中键粘贴选区 | data-device |
| 协议 | `linux-drm-syncobj-v1` — 显式 GPU 同步：导入客户端 DRM timeline syncobj，commit 时按 acquire point 有界等待（超时不卡死主循环），buffer 被替换时 signal release point | syncobj |
| 协议 | `wl_output` v4 — geometry / mode / scale / name / description / done（客户端 map 前需要；`name` 是 `grim -o` 等工具寻址输出用的） | — |
| 协议 | `xdg-output-unstable-v1`（`zxdg_output_manager_v1` v3）— 输出的**逻辑**位置与尺寸。wl_output 只描述物理模式；要把截图摆到画布上的工具（grim / slurp / 录屏器）读的是这个。缺了它 `grim` 只会警告并写出 0×0 的 PNG | — |
| 协议 | 截图/录屏 — `wlr-screencopy-unstable-v1` (v3) + `ext-image-copy-capture-v1` + `ext-image-capture-source-v1`：客户端捕获整块输出到 **wl_shm 或 dmabuf** buffer（`grim` 截图、`wf-recorder` 录屏），逐帧回调 GPU 合成结果；dmabuf 目标 LINEAR 走 mmap，tiled 走 Vulkan 导出，ext 路径广告 dmabuf device + modifier | dmabuf |

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
| backend | `WaylandBackend`（嵌套）：连父 compositor 开窗、wl_shm 呈现合成帧；**转发父 compositor 输入**（指针 enter/leave/motion/button、滚轮 axis/discrete/stop 按 `wl_pointer.frame` 聚合、键盘按键 + 修饰键），经命中测试路由到 seat；**原生窗口装饰**：以 `xdg-decoration-unstable-v1` 客户端身份向父 compositor 请求 server-side 装饰（附 title + app_id），拿到宿主桌面的真标题栏，协商结果由 `decoration_mode()` 报告 | wayland-nested |
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
# 或 LUMINARIA_BACKEND=headless 强制无头模式；LUMINARIA_OUTPUT=1280x800 改输出尺寸
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

## 完整 Compositor 路线图（缺失协议 / 部分）

达到"能日常用的完整 Wayland compositor"还需实现以下内容。按**客户端是否可用**分层。
✓=已有，✗=缺，△=部分。参照：wlroots 有 73 个 type 实现，luminaria 现有 13 个。

### P0 — 不做真实程序直接跑不起来 ✅ 全部完成

- ✓ **`linux-dmabuf-v1` (v3) + GBM 分配器** — GPU 客户端（浏览器 / GL·Vulkan app /
      游戏 / 视频）走 dmabuf 而非 wl_shm。GBM 开 render node 做分配器/校验；向客户端广告
      GPU 实际支持的 modifier 列表（经 Vulkan `VK_EXT_image_drm_format_modifier` 查询）。
      导入：LINEAR 走 mmap，其余 modifier（tiled/vendor）走 Vulkan external-memory
      (`VK_EXT_external_memory_dma_buf` + `VK_KHR_external_memory_fd`) 导入并读回 RGBA。
      **仍待做（正交优化，不属 P0 门槛）：** 零拷贝 scanout —— 目前统一读回 CPU RGBA 走软件
      合成，直接把 dmabuf 当 KMS scanout / 渲染目标（DRM AddFB2）是后续性能项，见「非协议部分」。
- ✓ **explicit sync**（`linux-drm-syncobj-v1`）— `import_timeline` 把客户端 DRM timeline
      syncobj 导进 render node；commit 时按 acquire point 做**有界** `drmSyncobjTimelineWait`
      （默认 50 ms，可调；超时只丢一帧，不卡死事件循环），buffer 被下一次 commit 替换时
      `drmSyncobjTimelineSignal` release point —— 即原本发 `wl_buffer.release` 的时刻。
- ✓ **`xdg_popup` + `xdg_positioner`** — 菜单 / tooltip / 下拉框 / 右键菜单。positioner v3
      全部请求实现，anchor / gravity / offset 求解出 popup 矩形；`grab` 语义（点击外部
      dismiss，子 popup 先于父 popup 关闭）、`reposition` + `repositioned`、父 xdg_surface
      销毁时级联 `popup_done`。**注：** `constraint_adjustment`（flip/slide/resize）已解析
      但未施加 —— 需要父窗口在输出上的绝对位置，shell 层拿不到；屏幕内的菜单（常态）位置正确，
      贴边的可能溢出。
- ✓ **xdg-shell 窗口状态**：maximize / fullscreen / activated / resizing 随 configure 的
      states 数组下发；`configure_bounds`（v4）、`wm_capabilities`（v5）；min/max size、
      title、app_id、window geometry 全部记录；interactive move/resize、minimize 以信号交给
      compositor 仲裁（无人接管时自动应答，客户端不会挂住）。
- ✓ **`wl_subcompositor`**（subsurface）— 子表面树 + 相对定位 + `place_above`/`place_below`
      堆叠 + **sync/desync**：同步子表面的 commit 缓存下来，父级 commit 时整棵子树原子提交。
      `Surface::surface_tree()` 给渲染和命中测试同一份 z 序列表。
- ✓ **`wl_data_device`** — 复制粘贴 + 拖放。选区跟随键盘焦点；`receive` 把客户端给的管道 fd
      直接转交源客户端，字节不经过 compositor。拖放期间 seat 把指针从 `wl_pointer` 切到
      `wl_data_device`（enter/motion/drop），支持 dnd actions 协商与 `finish`。
- ✓ **primary-selection**（中键粘贴）— `zwp_primary_selection_device_manager_v1`，同样跟随焦点。
- ✓ `wl_seat`：键盘 + 指针 + **触摸**（down/motion/up/frame/cancel）+ **滚轮**
      （平滑 axis、离散 notch + `axis_discrete`、`axis_source`、`axis_stop`）。嵌套后端按
      `wl_pointer.frame` 聚合父 compositor 的滚动事件后转发。
- ✓ **光标渲染** — `wl_pointer.set_cursor` 记录客户端光标 surface + hotspot 并发
      `cursor_changed` 信号；`tinyluminaria` 把它合成在最上层，客户端没设光标时画内置箭头。
      指针焦点离开或光标 surface 销毁时自动清除。
- ✓ **seat 焦点安全** — 键盘/指针/触摸焦点与光标 surface 各自订阅 `Surface::destroy`
      （RAII `Signal::Connection`），表面销毁即清空焦点并发出焦点变更信号，不再有裸指针悬空。

### P1 — 真实桌面必须

- ✗ **多输出 + 热插拔** + output layout + 每输出 scale/transform（现单输出固定）
- △ **`xdg-decoration`** — **客户端一侧已做**：嵌套后端向父 compositor 请求 server-side
      装饰，我们自己的窗口在宿主桌面上有原生标题栏（`WaylandBackend::decoration_mode()`
      报告协商结果；父方无此 global 或坚持 client-side 时不画，如实上报）。
      **服务端一侧仍缺**：我们还没有 `zxdg_decoration_manager_v1` global，自己的客户端
      因此拿不到装饰协商（要么无标题栏，要么双标题栏）。
- ✗ **`presentation-time`**（正确 vsync 反馈）— `frame` 事件目前在 commit 时触发而非实际展示时，需对齐输出真实 vblank
- ✗ **damage tracking** — 现每帧全量合成整个输出；应追踪 surface + output damage，只重绘脏区域
- ✗ **HiDPI**：`wp-fractional-scale-v1` + `wp-viewporter`
- ✗ **IME**：`text-input-v3` + `input-method-v2` — 中文/日文输入法
- ✗ **libseat** 会话管理 + VT 切换（现靠 logind ACL，不能安全切 VT）
- ✗ **光标**：`cursor-shape-v1` + 实际绘制光标（`set_cursor` 现为空实现）
- △ **libinput 接入 `tinyluminaria`** — 裸机键盘/鼠标仅在嵌套后端可用；DRM/TTY 路径发出了输入事件但未路由到 seat。参照嵌套的命中测试→seat 接线。
- △ **键图一致性** — 嵌套后端转发父 compositor 的 xkb 修饰键掩码，对照我们自己的 keymap；标准布局没问题，非美式布局/group 需采用父 keymap 或对齐。

### P2 — 完整功能 / 桌面外壳

- ✗ **layer-shell**（`wlr-layer-shell` 或 `ext-*`）— 面板 / 状态栏 / 壁纸 / 锁屏层
- ✗ **`ext-session-lock-v1`** — 锁屏
- ✓ **截图/录屏**：`wlr-screencopy-unstable-v1` (v3) + `ext-image-copy-capture-v1` + `ext-image-capture-source-v1` — 客户端捕获整块输出到 **wl_shm 或 dmabuf** buffer。三个 global 一起注册，每个可捕获输出经 `add_output()` 挂上 capture 回调，客户端请求时回调填 RGBA（当前帧 GPU 合成结果）。dmabuf 目标：`set_renderer()` 后 wlr 路径发 `linux_dmabuf` 事件、ext 路径广告 `dmabuf_device` + 每格式 modifier；写入时 LINEAR 走 mmap，tiled 走 `VulkanRenderer::export_dmabuf`。`grim` 截图、`wf-recorder` 录屏皆可。
- ✗ **`wlr-foreign-toplevel-management`** — 任务栏列窗口
- ✗ **`xdg-activation`** — 焦点转移 / 紧急提示
- ✗ **idle**：`ext-idle-notify` + idle-inhibit（视频防息屏）
- ✗ **output-management + gamma-control**（夜间模式）
- ✗ **data-control**（剪贴板管理器）
- ✗ **relative-pointer + pointer-constraints**（游戏/3D 锁定光标）、tablet-v2、pointer-gestures

### 非协议部分

- △ **真正的 XWM**（现 xwayland 极简）— 窗口堆叠、焦点、`_NET_*` hints、ICCCM/EWMH、override-redirect、X 剪贴板桥、`WL_SURFACE_ID` 关联
- △ **DRM atomic 模式设置 + GBM scanout**（现 `drm.cpp` 279 行，legacy pageflip + dumb buffer，真机未验证）——注：客户端 dmabuf 导入的 GBM 分配器已就位，此处指 scanout buffer 直出
- △ **渲染管线**：damage、多层混合、透明/圆角、离屏缓冲

### 建议实现顺序（最短能用路径）

1. ~~**dmabuf import + GBM 分配器**~~ — ✓ 已做（任意 modifier：LINEAR mmap + Vulkan 导入；零拷贝 scanout 待办）
2. ~~**xdg_popup + 窗口状态**~~ — ✓ 已做，解锁绝大多数 GTK/Qt app
3. ~~**data_device（剪贴板）**~~ — ✓ 已做；**xdg-decoration** 仍缺（见 P1）
4. **多输出 + damage + presentation-time** ← 当前下一步
5. **layer-shell + session-lock** — 有这个才算"桌面"
6. **text-input / input-method** — 中文输入

---

## 已知限制

- **无独立于 Shift/Ctrl 的修饰键状态**（除父 compositor 报告的之外，嵌套模式）— 实践中够用。
- **嵌套窗口装饰取决于父 compositor**：KDE/Plasma、wlroots 系给 server-side（有原生标题栏）；
  GNOME/Mutter 不实现 `xdg-decoration`，窗口裸奔。我们**不画** client-side 装饰 ——
  「原生装饰」按定义就是宿主画的，自绘一套是另一回事。
- **popup 的 `constraint_adjustment` 未施加**（见 P0 条目）：贴近输出边缘的菜单可能溢出。
- **DRM 路径在当前环境未在真机上验证**（无裸 VT）。
- **`linux-drm-syncobj` 的 acquire 等待是 CPU 阻塞**（有界超时）：因为合成本身就是 CPU
  读回。做到零拷贝 scanout 后应改成把 fence 直接交给 KMS。
- 抽象 `Backend`/`Output` 已就位，以上待实现项按同一模式扩展即可接入。

---

## 说明

用 `-std=c++23` 构建（依赖的最新特性是 `std::expected`，其余是 C++20/17）。内存安全
由构造保证：每个 C 句柄都 RAII 包装，每个信号监听析构自动摘链，无需手动 `wl_list_remove`。

---

## 截图

![tinyluminaria 嵌套运行，里面是 konsole 跑 fastfetch](docs/screenshot.png)

`tinyluminaria`（本库自带的参考 compositor）**嵌套**跑在 KDE Plasma 里 —— 最外层那条标题栏是
宿主画的**原生装饰**（经 `xdg-decoration-unstable-v1` 向父 compositor 请求 server-side）。
框内是我们自己合成的 800×600 输出，里面跑着 konsole，konsole 里跑 fastfetch。
注意 fastfetch 自己认出了 `WM: tinyluminaria (Wayland)` 与 `Display (luminaria virtual output)`。

复现：

```sh
WAYLAND_DISPLAY=wayland-0 ./build/examples/tinyluminaria &     # 嵌套；打印自己的 socket
WAYLAND_DISPLAY=<打印的 socket> konsole --nofork               # 然后在里面敲 fastfetch
```
