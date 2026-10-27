# Luminaria API 与协议接口完整参考手册

> 本文档详细记录 Luminaria 库对外暴露的所有模块、类、函数、结构体、信号与 Wayland 协议接口，并深入剖析其系统架构与设计约束。
> 
> 术语定义以 [CONTEXT.md](../CONTEXT.md) 为准；架构分层与不可逆设计决策参阅 [architecture.md](./architecture.md) 与 [adr/](./adr/)。

---

## 目录

1. [架构概述与核心设计哲学](#1-架构概述与核心设计哲学)
2. [C++23 模块划分与引入规范](#2-c23-模块划分与引入规范)
3. [核心运行时与通用设施 (`luminaria`)](#3-核心运行时与通用设施-luminaria)
   - [3.1 Display & EventLoop](#31-display--eventloop)
   - [3.2 Expected & Handle](#32-expected--handle)
   - [3.3 Signal & Observer](#33-signal--observer)
   - [3.4 Geometry & Color & Math](#34-geometry--color--math)
   - [3.5 Keymap & Input Routing](#35-keymap--input-routing)
4. [后端与硬件抽象 (`luminaria.backend`)](#4-后端与硬件抽象-luminariabackend)
   - [4.1 Backend & Output 抽象](#41-backend--output-抽象)
   - [4.2 输入：InputSource](#42-输入inputsource-luminariainput_event)
   - [4.3 Headless & Wayland 嵌套后端](#43-headless--wayland-嵌套后端)
   - [4.4 DrmBackend & Session & Libinput (Bare-metal)](#44-drmbackend--session--libinput-bare-metal-luminariabackend)
   - [4.5 AutoBackend：自动挑后端](#45-autobackend自动挑后端-luminariashellauto_backend)
5. [渲染器与合成引擎 (`luminaria.render`)](#5-渲染器与合成引擎-luminariarender)
   - [5.1 CpuCompositor (CPU 立即模式合成)](#51-cpucompositor-cpu-立即模式合成)
   - [5.2 VulkanRenderer (GPU 零拷贝渲染)](#52-vulkanrenderer-gpu-零拷贝渲染)
   - [5.3 CursorTheme & Text/Font](#53-cursortheme--textfont)
6. [外壳层与即时模式帧账本 (`luminaria.shell`)](#6-外壳层与即时模式帧账本-luminariashell)
   - [6.1 Frame (GPU 帧账本与摆位差异追踪)](#61-frame-gpu-帧账本与摆位差异追踪)
   - [6.2 Scene & SceneRenderer (统一场景抽象)](#62-scene--scenerenderer-统一场景抽象)
   - [6.3 DirectScanout (直出决策)](#63-directscanout-直出决策)
   - [6.4 OutputLayout & Pointer & PopupTree & LayerManager](#64-outputlayout--pointer--popuptree--layermanager)
7. [Wayland 核心协议对象 (`luminaria`)](#7-wayland-核心协议对象-luminaria)
   - [7.0 CoreGlobals & OutputGlobal](#70-coreglobals--outputglobal)
   - [7.1 Compositor & Surface & Subcompositor](#71-compositor--surface--subcompositor)
   - [7.2 XdgShell & LayerShell](#72-xdgshell--layershell)
   - [7.3 Seat & DataDevice & PrimarySelection](#73-seat--datadevice--primaryselection)
   - [7.4 PresentationTime & TearingControl & Fifo & CommitTiming](#74-presentationtime--tearingcontrol--fifo--committiming)
   - [7.5 Viewporter & FractionalScale & ContentType & XdgDecoration](#75-viewporter--fractionalscale--contenttype--xdgdecoration)
   - [7.6 PointerConstraints & RelativePointer & CursorShape](#76-pointerconstraints--relativepointer--cursorshape)
   - [7.7 TextInput & IdleInhibit & IdleNotify & BackgroundEffect](#77-textinput--idleinhibit--idlenotify--backgroundeffect)
8. [渲染与缓冲协议 (`luminaria.render`)](#8-渲染与缓冲协议-luminariarender)
   - [8.1 LinuxDmabuf](#81-linuxdmabuf)
   - [8.2 DrmSyncobj (显式同步)](#82-drmsyncobj-显式同步)
   - [8.3 Screencopy (截图与捕获)](#83-screencopy-截图与捕获)
9. [桌面专用特权协议 (`luminaria.desktop`)](#9-桌面专用特权协议-luminariadesktop)
   - [9.1 Workspace (工作区)](#91-workspace-工作区)
   - [9.2 ForeignToplevel (顶层窗口监控)](#92-foreigntoplevel-顶层窗口监控)
   - [9.3 DataControl (剪贴板管理器)](#93-datacontrol-剪贴板管理器)
   - [9.4 SessionLock (锁屏)](#94-sessionlock-锁屏)
   - [9.5 InputMethod (输入法引擎)](#95-inputmethod-输入法引擎)
   - [9.6 GammaControl (伽马曲线调节)](#96-gammacontrol-伽马曲线调节)
   - [9.7 CtmControl (色彩变换矩阵)](#97-ctmcontrol-色彩变换矩阵)
   - [9.8 VirtualKeyboard (虚拟键盘输入注入)](#98-virtualkeyboard-虚拟键盘输入注入)
   - [9.9 VirtualPointer (虚拟指针输入注入)](#99-virtualpointer-虚拟指针输入注入)
10. [架构约束、生命周期与安全规矩](#10-架构约束生命周期与安全规矩)

---

## 1. 架构概述与核心设计哲学

Luminaria 定位为**有主见的纯 Wayland 机制 C++23 库**：公共机制由库提供，窗口布局与
策略由上层混成器决定。

```
+-------------------------------------------------------------------------+
|                        Compositor Application                           |
|  (Window Placement, Tiling, Workspace Policy, Keybindings, Workflows)   |
+-------------------------------------------------------------------------+
|                         luminaria.shell                                 |
|  (Frame Ledger, SceneRenderer, Cursor, DirectScanout, AutoBackend)      |
+------------------------------------+------------------------------------+
|       luminaria.desktop            |         luminaria.render           |
| (Workspace, ForeignToplevel, Lock) | (VulkanRenderer, CpuCompositor,    |
|                                    |  linux-dmabuf, screencopy, syncobj)|
+------------------------------------+------------------------------------+
|                         luminaria.backend                               |
|       (KMS/DRM, Libinput, Session, Headless, Wayland-nested)            |
+-------------------------------------------------------------------------+
|                           luminaria (Core)                              |
|   (Display, EventLoop, Surface, Compositor, Seat, Math/Box/Region)      |
+-------------------------------------------------------------------------+
```

### 核心设计原则
1. **机制与策略分离**：库实现公共机制（帧调度、damage 记账、遮挡剔除、fence 同步、光标合成）；混成器保留全局坐标系、焦点仲裁与每帧的最终绘制掌控权。
2. **按需索取驱动（Demand-driven Frame Pump）**：输出的 `frame` 事件必须通过 `Output::schedule_frame()` 显式预约。空闲桌面实现**零 GPU 提交、零翻页、零时钟空转**。
3. **即时模式（Immediate-Mode，无持久场景树）**：混成器每帧提交 z 序摆位列表（`Placement` 或 `SceneItem`），`Frame` 通过比对前后帧摆位列表的拓扑差异（List Diff）自动推导 Damage 区域。
4. **内存安全靠类型系统构造**：
   - 跨调度表面引用强制采用代际标识符 `SurfaceId`（防 Use-After-Free）。
   - C 资源使用 `CUnique` 与 `UniqueFd` 自动 RAII 释放。
   - `Signal<Event>::Connection` 析构自动注销，避免监听器悬空。
   - 所有客户端内存索引前强制通过 `layout_fits()` 校验。

---

## 2. C++23 模块划分与引入规范

Luminaria 废弃了传统 C/C++ 的公共头文件体系（无 `include/`），基于 C++23 命名模块构建，按 ADR 0008 单向按层分立：

| 模块名称 | 职责边界与依赖要求 | 典型应用场景 |
|---|---|---|
| `import luminaria;` | 核心主接口。仅依赖 `wayland-server` 与 `xkbcommon`。包含 Display/EventLoop、核心协议、数学/几何值类型与 Backend/Output 抽象。 | 机制基底、纯协议测试、嵌入式无硬件环境 |
| `import luminaria.backend;` | 设备与后端驱动扩展。依赖 libdrm、libinput、libseat。包含 DRM/KMS、Libinput、Session、Headless、Wayland 嵌套。不依赖 GPU。 | 裸机/虚拟输出与输入管理、CPU 软渲染嵌入式系统 |
| `import luminaria.render;` | 渲染引擎扩展。依赖 Vulkan、GBM、libdrm。包含 Vulkan 1.3 渲染器、CPU 立即模式合成器与 dmabuf/syncobj/screencopy 缓冲协议。 | GPU 加速渲染、无头截图、离屏渲染 |
| `import luminaria.shell;` | 混成器全功能外壳。re-export 了 `backend` 与 `render`。包含 Frame 帧账本、SceneRenderer 调度器、Cursor、DirectScanout、AutoBackend。 | 生产级混成器、常规混成器首选入口 |
| `import luminaria.desktop;` | 桌面特权协议扩展。包含工作区、任务栏控制、剪贴板管理、锁屏与输入法引擎协议。只依赖 core。 | 完整桌面环境（Panel、Dock、Lockscreen） |

---

## 3. 核心运行时与通用设施 (`luminaria`)

### 3.1 Display & EventLoop
- **`Display`** (`luminaria:display`): `wl_display` 的唯一 RAII 所有者。
  - `static Result<Display> create()`: 创建 Wayland Display 实例。
  - `EventLoop event_loop() const`: 获取关联事件循环的非拥有视图。
  - `Result<std::string> add_socket_auto()`: 自动创建并绑定 `$WAYLAND_DISPLAY` 套接字。
  - `Status init_shm()`: 初始化 Wayland 内置共享内存（`wl_shm`）支持。
  - `void run()`: 启动主循环，阻塞直至 `terminate()`。
  - `void terminate()`: 请求事件循环在下一时钟周期退出。
  - `wl_display* c_ptr() const noexcept`: 逃逸接口（获取底层 C 指针）。
  > **析构保障**：Display 析构时先调用 `wl_display_destroy_clients()`，确保客户端资源的销毁监听器正确触发，避免内存泄漏。

- **`EventLoop` & `EventSource`** (`luminaria:event_loop`):
  - `void EventLoop::once(std::function<void()> fn)`: 在下一轮 idle 周期执行一次性任务。
  - `EventSource EventLoop::add_timer(std::function<void()> fn)`: 创建可复用定时器。
  - `EventSource EventLoop::add_fd(int fd, std::function<void()> fn)`: 监听文件描述符可读事件。
  - `void EventSource::arm(unsigned ms)`: 启动/更新定时器（ms 毫秒后触发）。
  - `void EventSource::disarm()`: 停止定时器。
  - `bool EventSource::valid() const noexcept`: 是否有效持有源。

### 3.2 Expected & Handle
- **`Result<T>` & `Status`** (`luminaria:expected`):
  - `struct Error { std::string message; int code = 0; };`
  - `template <class T> using Result = std::expected<T, Error>;`
  - `using Status = Result<void>;`
  - `fail(std::string message, int code = 0)` / `ok()`
- **`CUnique<T, Fn>` & `UniqueFd`** (`luminaria:handle`):
  - `CUnique<T, Fn>`: 针对 C 句柄的 `std::unique_ptr` 封装。
  - `UniqueFd`: 拥有所有权的 Linux 文件描述符封装，默认 `-1`，析构自动 `close()`。支持 `release()`、`reset(fd)`、`duplicate()`。

### 3.3 Signal & Observer
- **`Signal<Event>`** (`luminaria:signal`): 类型安全的事件发布/订阅机制。
  - `Connection connect(F&& slot)`: 注册观察者。`slot` 收事件，或者什么都不收（`[&] { ... }`）——用不着事件的回调不必再把事件类型写一遍。
  - `void emit(Event& event)`: 触发信号（重入安全：触发期间新增的槽在下一轮才生效，移除的槽立即标记失效）。
  - `std::size_t slot_count() const noexcept`: 当前活跃槽位计数。
  - **`Signal<Event>::Connection`**: RAII 订阅句柄，移动语义。析构或调用 `disconnect()` 自动退订；当 `Signal` 先销毁时自动安全失效。
- **`Connections`** (`luminaria:signal`): 一袋任意信号类型的连接，给不值得单独起名字的那些。`connections += sig.connect(...)`；
  信号已经死掉的连接是惰性的，袋子长大时顺手清掉，所以一个反复开关的窗口不会攒下垃圾。袋子析构时全部断开，
  **要声明在回调所捕获的一切之后**——这也让回调不会在 `Display` 析构、客户端对象被销毁时碰到已经没了的局部变量。
- **`match(variant, handlers...)` / `Overloaded`** (`luminaria:match`): 一个变体的每个分支一个处理函数，取代每个使用 `InputEvent` 的
  代码上方那三行 overload 惯用法。
- **`to_optional(Result<T>)`** (`luminaria:expected`): “建不出来就算了”——有值给值，否则 `nullopt`。`Result` 就是 `std::expected`，
  `and_then` / `transform` / `or_else` / `value_or` 本来就有。

### 3.4 Geometry & Color & Math
- **`Box`** (`luminaria:box`): `{ int x, y, width, height; }`
  - `bool empty() const`, `bool contains(int px, int py) const`
  - `Size size() const`, `Point center() const`；`Size` 是 `{ int width, height; }`，`Point` 是 `{ double x, y; }`。
  - `Box intersection(const Box& o) const`, `Box union_with(const Box& o) const`
- **`Color`** (`luminaria:color`): `{ float r, g, b, a; }`（各分量范围 `[0.0f, 1.0f]`）。
- **`Pixel`** (`luminaria:pixel`): `{ uint8_t r, g, b, a; }`（未预乘或预乘 8 位 RGBA）。
- **`Region`** (`luminaria:region`): 不相交矩形集合（Pixman/wl_region 语义）。
  - `void add(const Box&)`, `void add(const Region&)`
  - `void subtract(const Box&)`, `void subtract(const Region&)`
  - `void intersect(const Box&)`, `void translate(int dx, int dy)`
  - `bool contains(int px, int py) const`, `bool intersects(const Box&) const`
  - `Box extents() const`, `void coalesce()`
- **`Transform`** (`luminaria:transform`): 8 种 Wayland 输出变换（`normal`, `_90`, `_180`, `_270`, `flipped`, `flipped_90`, `flipped_180`, `flipped_270`）。
  - `transform_swaps_axes(Transform)`, `transform_rotation(Transform)`, `transform_invert(Transform)`, `transform_compose(a, b)`
  - `Box transform_box(Transform t, int scale, Box logical, int device_w, int device_h)`
- **`pixel_layout`** (`luminaria:pixel_layout`):
  - `bool layout_fits(int w, int h, int64_t stride, int64_t offset, size_t size)`: 跨步与偏移内存边界硬校验，防御非受信客户端越界攻击。

### 3.5 Keymap & Input Routing
- **`KeymapState`** (`luminaria:keymap`): 基于 libxkbcommon 的 RAII 键盘状态机。
  - `static Result<KeymapState> from_layout(std::string_view layout)` / `from_text(std::string_view text)`
  - `const std::string& text() const`: 获取 XKB 文本（用于传给客户端）。
  - `void update_key(uint32_t evdev_code, bool pressed)`: 推进按键状态。
  - `void update_modifiers(ModifiersEvent e)`: 推进修饰键状态。
  - `uint32_t keysym(uint32_t evdev_code) const`: 解析 keysym。
  - `ModifiersEvent modifiers() const`: 获取当前有效修饰键掩码。
- **`KeyRouter`** (`luminaria:input_router`): 在混成器快捷键与焦点客户端之间分配键盘，每个键保持完整。
  - `template <class Binding> void route(Seat&, const KeyEvent&, Binding&& binding)`：按下先交给 `binding`（`bool(const KeyEvent&)`），答 true 则这个键连同它的松开都留下；其余交给 `Seat`。松开丢失（切 VT）后的再次按下按新按下处理。
- 与之配合的 `Seat::release_keys()`（松开客户端以为按着的键并清修饰键）、`Seat::follow(InputSource&, bool touch = false)`（立即采用并跟随输入源的 keymap 与 capabilities）、`Session::switch_vt_for(keysym)`（Ctrl+Alt+Fn）。

---

## 4. 后端与硬件抽象 (`luminaria.backend`)

### 4.1 Backend & Output 抽象
- **`Backend`** (`luminaria:backend`):
  - `Signal<Output&>& new_output()`: 新显示输出发现信号。
- **`Output`** (`luminaria:output`):
  - `void schedule_frame()`: 预约下一帧（向后端申请唤醒）。
  - `void commit(Color color)`: 提交纯色填充帧。
  - `void commit_frame(std::span<const Pixel> rgba, int width, int height)`: 提交 CPU 像素帧。
  - 属性访问：`width()`, `height()`, `scale()`, `set_scale(int)`, `transform()`, `set_transform(Transform)`, `name()`, `description()`, `make()`, `model()`.
  - 核心信号：
    - `Signal<FrameEvent>& frame()`: 渲染节拍到达信号（携带 `time_ms`, `refresh_nsec`, `predicted_presentation_ms`）。
    - `Signal<PresentEvent>& present()`: 帧上屏呈现完成信号。
    - `Signal<OutputDestroy>& destroy()`: 输出断开信号。
    - `Signal<OutputModeChange>& mode_changed()`: 模式/分辨率变更信号。
  - 虚接口（由硬件后端重写）：`scanout_modifiers()`, `import_scanout()`, `commit_scanout()`, `set_mode()`, `modes()`, `vrr_capable()`, `vrr_enabled()`, `set_vrr()`.
  - **可变刷新率**：`vrr_capable()`（显示器 + 连接器都支持；headless / 嵌套恒为假）、`vrr_enabled()`（已请求，即使还没随提交落到硬件）、`Status set_vrr(bool)`（随下一次提交生效；在不支持的输出上开启是错误，关闭永远成功；内核事后拒绝则退回关闭并把 `vrr_capable()` 置假）。何时开是混成器的策略。

### 4.2 输入：`InputSource` (`luminaria:input_event`)
- **`InputSource`**：输入从哪来的统一接口，嵌套的 `WaylandBackend` 与裸机的 `LibinputBackend` 都是它。两者的差别在于到来的是 `InputEvent` 的哪些分支（相对移动与触摸来自 libinput，绝对移动与离开来自嵌套窗口），而不在于怎么到来，所以混成器只写一个处理函数。
  - `Signal<InputEvent>& input()`：全部输入事件，按设备产生的顺序。
  - `const std::string& keymap() const`：按键所依据的 keymap 文本；之后的变化以 `KeymapChange` 到来。交给 `Seat::set_keymap()`。
  - `InputCapabilities capabilities() const`：设备当前能做什么；之后的变化以 `InputCapabilities` 到来。
- **`InputEvent`** = `std::variant<KeyEvent, ModifiersEvent, KeymapChange, PointerMotionEvent, PointerMotionAbsEvent, PointerLeaveEvent, PointerButtonEvent, PointerAxisEvent, Touch{Down,Motion,Up,Frame,Cancel}Event, InputCapabilities, TabletDeviceAdded, TabletDeviceRemoved, TabletTool{Proximity,Axis,Tip,Button}Event, TabletPad{Button,Ring,Strip}Event>`。
  - 数位板事件：`device` 是板子（或 pad）、`tool` 是笔，都是后端自己的 id；位置按板子的有效区域归一化到 [0,1]（与触摸同理，映射到哪块输出是混成器的事，交给 `TabletManager::feed()`）。`TabletAxes::changed` 位掩码说明更新里哪些轴有意义，proximity-in 带全部。
  - `KeyEvent` 带 `keysym` 与 `modifiers`：后端按自己的 keymap 读出，取**这个键之前**的状态（先按 Logo 再按 Return，到来的是带着 Logo 的 Return）。快捷键直接对着它匹配，混成器不需要自己的 xkb 状态。
  - `PointerMotionAbsEvent::output` 指明坐标属于哪个输出。
  - `PointerAxisEvent` 交给 `Seat::pointer_axis(const PointerAxisEvent&)`：滚轮同时报刻度和平滑量，刻度优先，否则每格滚两次。

### 4.3 Headless & Wayland 嵌套后端
- **`HeadlessBackend`** (`luminaria:headless`):
  - `static Result<HeadlessBackend> create(EventLoop loop, int width = 800, int height = 600)`
  - `Output& create_output(int width, int height)`: 动态创建虚拟无头输出。
  - `HeadlessOutput`: 虚拟输出。支持 `requires_commit_pixels()`（默认为 true 兼容读取 `last_frame()`，可设为 false 避免逐帧 CPU 回读；环境变量 `LUMINARIA_NO_READBACK=1` 可全局禁用同步读回）。
  - `void set_requires_commit_pixels(bool required)`: 开关逐帧 CPU 提交；为 false 时只有在测试调用 `last_frame()` 时才懒惰回读。
- **`WaylandBackend`** (`luminaria:wayland`):
  - `static Result<WaylandBackend> create(EventLoop loop, Display& display, int width, int height, std::string_view title)`
  - 嵌套在宿主 Wayland 混成器内运行，支持父级 dmabuf 直出。
  - 是一个 `InputSource`：转发父级的输入，按键按父级 keymap 读出 keysym，`keymap()` 就是父级的 keymap。
  - `Signal<HostCloseRequest> close_requested`：父级要求关窗（不是输入，不经 `input()`）。

### 4.4 DrmBackend & Session & Libinput (Bare-metal, `luminaria.backend`)
- **`Session`** (`luminaria.backend:session`): 基于 libseat 的 VT 与设备权限仲裁。
  - `static Result<Session> create(EventLoop loop, std::string_view seat_name = "")`
  - `int open_device(std::string_view path)` / `void close_device(int fd)`
  - `bool active() const`, `Signal<SessionActive>& active_changed()`
- **`DrmBackend`** (`luminaria.backend:drm`): KMS Atomic Modesetting 裸机显示驱动。
  - `static Result<DrmBackend> create(EventLoop loop, Session& session, std::string_view card_path = "")`
  - 监听 udev connector 热插拔与 GPU 节点热插拔，为每个活跃显示器构建 `DrmOutput`；支持硬件光标平面（Hardware Cursor Plane）、Atomic 翻页与显式 Fence（`IN_FENCE_FD` / `OUT_FENCE_PTR`）。
  - `Signal<DrmDeviceRemoved> device_removed`: 本显卡被移除/拔出时发射，自动安全清理关联输出。
  - `Signal<DrmDeviceAdded> device_added`: 检测到系统中插入新显卡设备节点时发射。
- **`LibinputBackend`** (`luminaria.backend:libinput`): 裸机输入设备事件源。
  - `static Result<LibinputBackend> create(EventLoop loop, Session& session)`
  - 是一个 `InputSource`：相对移动、按钮、滚轮、按键、修饰键（仅在变化时）、触摸、设备增删引起的能力变化，全经 `input()`。`set_keymap()` 成功时也发 `KeymapChange`。
  - `const KeymapState& keymap_state() const`: 获取底座共享的 XKB 状态。

### 4.5 AutoBackend：自动挑后端 (`luminaria.shell:auto_backend`)
每个混成器开头都是同一段：有父混成器就嵌套，有 seat 就上裸机，否则无头；开显卡、加入 seat、让 libinput 走同一个会话、
让渲染器选中屏幕背后那块 GPU。`AutoBackend` 就是这一段。
- **`BackendKind`** = `automatic | nested | drm | headless`；`Result<BackendKind> parse_backend_kind(std::string_view)`
  解析 `"auto" | "nested" | "drm" | "headless"`（示例里 `LUMINARIA_BACKEND` 的取值），`backend_kind_name()` 反过来。
  读环境变量仍是调用者的事。
- **`BackendOptions`**：`kind`（默认 `automatic`）、`drm_device`（空 = 第一块接了显示器的卡）、`width`/`height`
  （嵌套与无头自带的那一块输出，默认 1280×800）、`title`（嵌套窗口标题）。
- **`static Result<AutoBackend> create(EventLoop loop, const BackendOptions& = {})`**：
  - `automatic` 依次试：`WaylandBackend` + 一个窗口；**能拿到 libseat `Session` 时**的 `DrmBackend`（ssh、CI 因此不会误上 VT），
    DRM 失败就放掉会话；最后 `HeadlessBackend` + 一块输出。
  - 按名字要某一种时，失败就是错误而不回退；要 `drm` 时没有会话也照跑（切 VT 不安全，原因见 `session_error()`）。
  - DRM 上同时建好同一会话上的 `LibinputBackend`，并设 `VulkanOptions::drm_device = drm_device_id(drm.fd())`。
- 取用：`kind()`（不会是 `automatic`）、`Backend& backend()`（订阅 `new_output`）、`InputSource* input()`（嵌套后端或
  libinput，无头为空）、`WaylandBackend* nested()`（`close_requested` 由调用者回答）、`DrmBackend* drm()`、
  `LibinputBackend* libinput()`、`Session* session()`（VT 切换与 `activity()`，只在 DRM 上）、`session_error()`、
  `Signal<DrmDeviceRemoved>& gpu_removed()`、`Signal<DrmDeviceAdded>& gpu_added()`、
  `const VulkanOptions& vulkan_options()`（交给 `SceneRenderer` / `VulkanRenderer`）。
- **`Status start()`**：先 libinput，后后端。
- 会话、显卡后端、libinput 都归它所有，按"会话最后析构"的顺序声明；它是 pimpl，所以移动它不会让 `Session*` 失效。
  和别的后端一样，要声明在各 global 之后（输出最后一次 `present` 还要找得到 presentation global）。

---

## 5. 渲染器与合成引擎 (`luminaria.render`)

### 5.1 CpuCompositor (CPU 立即模式合成, `luminaria.render:cpu_compositor`)
专为无头模式、嵌套回退、测试及截屏设计的软件合成器：
- **`CpuView`**: `{ SurfaceId root; int x, y; Box clip; }`（`clip` 限制表面树绘制边界，平铺混成器无需手工擦除溢出）。
- **`CpuImage`**: `{ Box box; std::span<const Pixel> pixels; }`。
- **`using CpuItem = std::variant<RectFill, CpuView, CpuImage>;`**
- **`CpuCompositor`**:
  - `void composite(int width, int height, Color background, std::span<const CpuItem> items)`: 预乘 alpha source-over 混合。
  - `std::span<const Pixel> pixels() const`: 获取合成后的像素跨度。
- **`void send_frame_done(std::span<const SurfaceId> ids, uint32_t time_ms)`**: 自由函数，为 CPU 路径批量应答帧回调。

### 5.2 VulkanRenderer (GPU 零拷贝渲染, `luminaria.render:vulkan`)
基于 `VK_EXT_external_memory_dma_buf` 与 `VK_EXT_image_drm_format_modifier` 的 GPU 渲染器：
- **`GpuTexture`**: 缓存客户端导入的 dmabuf 或上传的 shm 像素。
- **`ScanoutTarget`**: 分配并导出为 dmabuf 的渲染目标（KMS 直接上屏）。
- **`OffscreenTarget`**: 离屏渲染目标，兼具 Render Target 与 `GpuTexture` 能力（ADR 0005）。
- **`VulkanRenderer`**:
  - `static Result<VulkanRenderer> create(...)`: 初始化 Vulkan 实例与物理设备。
  - `Status render_to(ScanoutTarget& target, Color background, std::span<const GpuTextureFill> fills, const Region& repaint, const OutputMapping& mapping, RenderSync sync)`: 执行多边形合并渲染，支持不透明遮挡剔除、独立 Damage Scissor、Fence 异步编排。
  - `Status read_scanout(ScanoutTarget& target, std::vector<Pixel>& out)`: 回读渲染结果至 CPU（32 位整字快速回读，消除逐字节标量循环与二次拷贝）。

### 5.3 CursorTheme & Text/Font
- **`CursorTheme`** (`luminaria:cursor_theme`):
  - `static Result<CursorTheme> load(std::string_view name, int size)`
  - `const CursorImage* get(std::string_view shape_name) const`
- **`Font`** (`luminaria.text`):
  - `static std::optional<Font> open(std::string_view pattern)`
  - `int measure(std::string_view utf8) const`, `int height() const`, `int ascent() const`

---

## 6. 外壳层与即时模式帧账本 (`luminaria.shell`)

### 6.1 Frame (GPU 帧账本与摆位差异追踪, `luminaria.shell:frame`)
`Frame` 是外壳层的核心账本，每输出一份，按帧重构：
- **核心构建流程**：
  - `void begin(Box logical_view)`: 开启新一帧记录。
  - `void place(Surface& surface, int x, int y, PlacementTransform transform = {})`: 放置客户端表面树（自动展开子表面）。
  - `void place_rect(int x, int y, int width, int height, Color color)`: 放置混成器纯色矩形（边框/面板）。
  - `void place(const GpuTexture& texture, int x, int y, int width, int height, ...)`: 放置纹理（光标/壁纸）。
  - `GroupScope begin_group(int width, int height)` / `void compose_group(GroupScope, ...)`: 开启并合成离屏特效组（ADR 0005）。
  - `void place_xray_blur(Surface& surface, int x, int y)`: 放置 X-Ray 亚克力背景模糊。
  - `Result<Presented> submit(Color background)`: 完成合成并提交翻页。
- **摆位差异与 Damage 记账 (List Diff)**：
  `submit()` 将当前帧摆位与上一次上屏摆位进行逐项对比，自动为发生位移、缩放、层级改变的图元计算旧矩形与新矩形 Damage。
- **状态返回值 `enum class Presented`**:
  - `composited`: 正常 GPU 合成并翻页。
  - `scanout`: 单全屏窗口免合成直出。
  - `unchanged`: 画面与屏上一致，跳过翻页与渲染（进入零功耗静止）。
  - `fallback`: GPU 提交失败，需回退至 CPU 合成。
- **辅助接口**：
  - `SurfaceId surface_at(double x, double y) const`: 在当前摆位中进行精准像素命中测试。
  - `void invalidate()`: 标记摆位失效并申请下一帧（用于窗口移动等唤醒）。
  - `void damage_all()`: 强制整屏重绘（用于背景色改变或直出恢复）。
  - `void animate()`: 声明当前处于连续动画状态。
  - `void send_frame_done(uint32_t time_ms)`: 为本帧所有绘制的表面批量应答 `wl_surface.frame`。

### 6.2 Scene & SceneRenderer (统一场景抽象)
跨 GPU 与 CPU 路径的统一高层场景表示：
- **`SceneItem`** (`luminaria:scene`):
  - 图元类型：`Kind::surface`、`Kind::rect`、`Kind::border`、`Kind::image`；透明度、圆角、模糊与投影是图元属性。
  - `shadow_box` 可让投影包围大于图元本身的外框（例如客户端表面外的服务端装饰）；空矩形表示使用 `box`。
  - `std::uint64_t tag`: 混成器自定义标识符（库不解析，命中测试原样返回）。
- **`SceneHit scene_hit_test(std::span<const SceneItem> scene, double x, double y)`**: 全图元命中测试。
- **`std::optional<ScenePoint> scene_surface_origin(scene, SurfaceId)`**：某表面（含子表面）的原点画在哪。
- **`SceneHit scene_route_pointer(Seat&, scene, double x, double y)`**：指针路由——有隐式 grab 时按 grab 表面自己的坐标发运动，否则把焦点给命中的表面或清掉焦点。
- **`SceneRenderer`** (`luminaria.shell:scene_renderer`):
  - 将 `std::vector<SceneItem>` 自动分发至 `Frame` 或 `CpuCompositor`，透明处理运行时 GPU 降级。
  - `Status reset(SceneOutput&, Output&, uint32_t drm_format = XRGB8888)`：每块输出调一次，之后的 mode 变化自己重新分配。
  - `present(state, output, scene, background)`（输出坐标）与 `present(state, output, scene, background, const OutputLayout&)`（全局坐标）。
  - `SceneStats stats() const`：`present()` 到目前为止按结果（composited / scanout / unchanged / cpu）数过的帧，看空闲屏幕是不是真的安静下来。

### 6.3 DirectScanout (直出决策, `luminaria.shell:direct_scanout`)
- **`DirectScanout`** (`luminaria.shell:direct_scanout`): 评估单全屏表面是否满足 KMS 硬件直出要求（无旋转、无裁剪、格式匹配），并管理导入缓存与 `Surface::hold_buffer()`。

### 6.4 OutputLayout & Pointer & PopupTree & LayerManager
- **`OutputLayout`** (`luminaria:output_layout`): 全局逻辑坐标系管理。不可移动：它盯着装进来的每块输出，销毁的自己离开、换 mode 的保留位置换上新尺寸。
  - `void add(Output&, int x, int y)`, `void add_auto(Output&)`, `void remove(const Output&)`
  - `Output* at(int x, int y) const`, `Box box_of(const Output&) const`
  - `Output* primary() const`, `Box primary_box() const`：第一块放进来的输出（窗口没有更好主意时去的那块），没有时为空 / 空盒。
  - `Signal<OutputLayoutChange> changed`：任何输出的格子变了（含它自己做的变化）。
- **`Pointer`** (`luminaria:pointer`): 全局光标坐标与约束状态。
- **`PopupTree`** (`luminaria:popup`): XDG 弹出菜单（Popup）树管理与 Grab 仲裁。`explicit PopupTree(XdgShell&)` 从创建到销毁自己跟踪 shell 的每个 popup，`Signal<PopupTreeChange> changed` 在映射 / 取消映射 / 重定位 / 销毁时广播；layer surface 的 popup 用 `set_parent()` 指定父表面。
- **`LayerManager`** (`luminaria:layer`): Layer-shell 四层拓扑与排版计算器。

---

## 7. Wayland 核心协议对象 (`luminaria`)

### 7.0 CoreGlobals & OutputGlobal
- **`CoreGlobals`** (`luminaria:core_globals`): `static Result<CoreGlobals> create(Display&, std::string seat_name = "seat0")` 一次创建每个可用混成器都要的 global——wl_shm、wl_compositor、wl_subcompositor、xdg-shell、wl_seat、剪贴板与 primary selection、single-pixel-buffer、presentation-time、viewporter、fractional-scale、cursor-shape、xdg-decoration——并按借用关系排好析构顺序。访问器：`compositor()`、`shell()`、`seat()`、`data_device()`、`presentation()`、`fractional_scale()`、`cursor_shape()` 等。linux-dmabuf（需要渲染器）与 desktop 模块的特权协议仍单独创建。
- **`OutputGlobal`** (`luminaria:output_global`): `create(Display&, Output&, OutputLayout&, std::string name = {})` 建一个跟随输出与布局的 wl_output（+ xdg-output）：mode 变化重发 mode / scale / transform，布局变化重发位置，输出销毁后停止跟随；空名字取 `luminaria-<n>`。`create(Display&, width, height, name)` 留给不来自后端的输出。

### 7.1 Compositor & Surface & Subcompositor
- **`Compositor`** (`luminaria:compositor`):
  - `static Result<Compositor> create(Display& display)`
  - `Signal<Surface&>& new_surface()`: 新表面创建信号。
- **`Surface`** (`luminaria:compositor`):
  - 代际 ID：`SurfaceId id() const noexcept`（使用 `surface_from_id(id)` 解析）。
  - 几何尺寸：`int surface_width() const`, `int surface_height() const`, `int buffer_width() const`, `int buffer_height() const`.
  - 表面树：`std::vector<SurfaceTreeEntry> surface_tree() const`（深度优先展开子表面）。
  - 缓冲接口：`BufferContents current_buffer_rgba()`, `std::optional<DmabufAttributes> current_buffer_dmabuf()`.
  - Damage 管理：`const Region& damage() const`, `void clear_damage()`.
  - 命中与区域：`bool accepts_input(double sx, double sy) const`, `const Region& opaque_region() const`, `const Region& input_region() const`, `const Region& blur_region() const`.
  - 节拍反馈：`void send_frame_done(uint32_t time_ms)`.
  - 首选参数推荐：`void set_preferred_buffer_scale(int)`, `void set_preferred_transform(Transform)`.
  - 核心信号：`commit()`, `destroy()`, `invalidated()`.
- **`Subcompositor`** (`luminaria:subcompositor`): 管理子表面的相对位移、z 序与同步/非同步提交模式。

### 7.2 XdgShell & LayerShell
- **`XdgShell`** (`luminaria:xdg_shell`):
  - `static Result<XdgShell> create(Display& display)`
  - 信号：`new_toplevel()`, `new_popup()`
  - `set_bounds(int w, int h)` / `set_bounds(Size)`；`focus(Seat&, Toplevel*)`：让这个窗口（空指针 = 没有）独自拿到 activated 状态和 `seat` 的键盘焦点，原来的那个取消激活，焦点窗口销毁时自己忘掉——混成器不必再留“哪个是激活的”这个变量。
  - **`Toplevel`**: `configure(int w, int h)` / `configure(Size)`（返回 serial）, `close()`, `set_bounds(int max_w, int max_h)`.
    - 请求信号：`request_maximize`, `request_fullscreen`, `request_minimize`, `request_move`, `request_resize`, `request_window_menu`.
  - **`Popup`**: `configure(const Box& geometry)`, `reposition(...)`, `popup_done()`.
- **`LayerShell`** (`luminaria:layer_shell`): `zwlr_layer_shell_v1` 实现。
  - `static Result<LayerShell> create(Display& display)`
  - `Signal<LayerSurface&>& new_surface()`
  - **`LayerSurface`**: `configure(int w, int h)`, `layer()`, `anchor()`, `exclusive_zone()`, `margin()`, `keyboard_interactivity()`.

### 7.3 Seat & DataDevice & PrimarySelection
- **`Seat`** (`luminaria:seat`):
  - `static Result<Seat> create(Display& display, std::string_view name)`
  - 权限声明：`void set_capabilities(bool keyboard, bool pointer, bool touch)`，或直接 `set_capabilities(InputCapabilities)`
  - 键盘输入：`void set_keymap(std::string_view text)`, `void notify_key(uint32_t code, bool pressed)`, `void notify_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)` / `notify_modifiers(const ModifiersEvent&)`
  - 指针输入：`void pointer_motion(double sx, double sy)`, `void pointer_button(uint32_t button, bool pressed)`, `void pointer_axis(double dx, double dy)`, `pointer_axis_discrete(...)`, `pointer_axis_stop(...)`，以及把后端的一帧滚动按「刻度优先」转成以上三者的 `pointer_axis(const PointerAxisEvent&)`
  - 焦点控制：`void set_keyboard_focus(Surface* surface)`, `void set_pointer_focus(Surface* surface, double sx, double sy)`
  - 信号：`cursor_change()`, `pointer_focus_changed()`, `keyboard_focus_changed()`
- **`DataDeviceManager` & `PrimarySelectionManager`** (`luminaria:data_device`): 剪贴板、主选区与拖放（DnD）支持。

### 7.4 PresentationTime & TearingControl & Fifo & CommitTiming
- **`PresentationTime`** (`luminaria:presentation_time`): `wp_presentation_time` 精准时间戳协议。
  - `void notify_presented(Surface& surface, const PresentationFeedback& feedback)`
  - `void notify_discarded(Surface& surface)`
- **`TearingControlManager`** (`luminaria:tearing_control`): 允许游戏等低延迟客户端申请异步撕裂翻页。
- **`FifoManager` & `CommitTimingManager`** (`luminaria:fifo`, `luminaria:commit_timing`): 提交节拍栅栏与定时提交控制。被扣住的提交在 `Surface` 上按序排队（`deferred_commit_count()`、`deferred_commit_time()` 看队头），每次放行条件满足就依次应用，不再塌缩。

### 7.5 Viewporter & FractionalScale & ContentType & XdgDecoration
- **`Viewporter`** (`luminaria:viewporter`): 客户端源裁剪与目标拉伸支持。
- **`FractionalScaleManager`** (`luminaria:fractional_scale`): `wp_fractional_scale_v1`。`set_scale(Surface&, double scale)` 同时发精确值（线上以 120 为基数）和向上取整的 `preferred_buffer_scale` 整数提示。
- **`ContentTypeManager`** (`luminaria:content_type`): 视频、游戏、图片等画质与显示模式提示。
- **`XdgDecorationManager`** (`luminaria:xdg_decoration`): 服务端/客户端装饰协商（默认答 Client-side）。
- **`XdgToplevelIconManager`** (`luminaria:xdg_toplevel_icon`): 发布 `xdg_toplevel_icon_manager_v1`；`icon_for(toplevel)` 返回客户端图标名和已复制的 RGBA 尺寸集，`changed()` 在图标替换时发信号。

### 7.6 PointerConstraints & RelativePointer & CursorShape & PointerGestures
- **`PointerConstraints`** (`luminaria:pointer_constraints`): 指针锁定（Locked Pointer）与约束（Confined Pointer）。
- **`RelativePointerManager`** (`luminaria:relative_pointer`): 相对运动事件（无界 FPS 视角控制）。
- **`CursorShapeManager`** (`luminaria:cursor_shape`): `wp_cursor_shape_v1` 标准系统光标形状协商。
- **`PointerGestures`** (`luminaria:pointer_gestures`): `zwp_pointer_gestures_v1` (v3)。支持 `swipe`（多指滑动）、`pinch`（双指缩放旋转）与 `hold`（多指按住）手势事件下发。

### 7.7 TextInput & IdleInhibit & IdleNotify & BackgroundEffect
- **`TextInputManager`** (`luminaria:text_input`): `zwp_text_input_v3` 客户端文本输入。
- **`IdleInhibitManager`** (`luminaria:idle_inhibit`): 阻止系统休眠/锁屏。
- **`IdleNotifier`** (`luminaria:idle_notify`): 空闲状态超时监控。
- **`BackgroundEffectManager`** (`luminaria:background_effect`): 亚克力模糊区域协商。

### 7.8 Tablet (`luminaria:tablet`)
- **`TabletManager`**: `zwp_tablet_manager_v2` (v2)。
  - 设备：`add_tablet(TabletInfo)` / `add_tool(TabletToolInfo)` / `add_pad(TabletPadInfo)`（返回不复用的 id）与对应的 `remove_*`；后绑定的客户端也能拿到已有设备。
  - 笔：`tool_proximity_in(tool, tablet, Surface&)` / `tool_proximity_out` / `tool_down` / `tool_up` / `tool_motion(x, y)`（表面坐标）/ `tool_pressure` / `tool_distance` / `tool_tilt` / `tool_rotation` / `tool_slider` / `tool_wheel` / `tool_button` / `tool_frame(time_ms)`。事件只发给笔下表面所属的客户端；`tool_proximity_out` 自己带 `frame`。
  - pad：`pad_enter` / `pad_leave` / `pad_button` / `pad_ring` / `pad_strip` / `pad_mode_switch`。
  - `void feed(const InputEvent&, const Box& area, const TabletLocator& locate)`：一步到位。`area` 是板子 [0,1] 方块对应的布局盒子，`locate(x, y)` 回答「这一点下面是哪个表面、表面坐标多少」（典型是 `scene_hit_test`）；设备和笔自己建、自己撤，笔跨窗口自己换焦点，pad 跟笔。非数位板事件被忽略，所以可以把每个 `InputEvent` 都递进去。
  - `Signal<TabletToolCursorRequest>& cursor_request()`；静态 `tool_id_of(wl_resource*)` 把 `wp_cursor_shape` 请求里的 `tablet_tool` 解析成笔 id。
  - 没有接进 `minimal`：和触摸一样，映射到哪块输出、命中哪个表面是混成器的决定。

### 7.9 Color (`luminaria:color_management`)
- **`ColorManager`**: `wp_color_manager_v1` (v3)。`create(display, ColorSupport = ColorSupport::srgb_only())`。
  - `ColorSupport` 说明混成器兑现得了什么：命名原色 / 传递函数 / 渲染意图列表，加 `set_primaries`、`set_tf_power`、`set_luminances`、`set_mastering_display_primaries`、`extended_target_volume` 开关。`srgb_only()`（默认）只发 sRGB，多要就是协议错误；`full()` 全开，给自己有色彩管线的混成器。ICC、`windows_scrgb`、`windows_bt2100` 不支持。
  - `set_output_description(OutputGlobal&, ImageDescription)` / `output_description()`：输出是什么（默认 sRGB）；变化时已问过的客户端收到 `image_description_changed`。`set_default_preferred()` / `set_preferred(Surface&, ImageDescription)`：表面最好用什么色彩空间渲染；变化时收到 `preferred_changed2`。
  - 相等的色彩空间共享一个 identity（只要有人还拿着）。
- **`ColorRepresentationManager`**: `wp_color_representation_manager_v1`。`create(display, ColorRepresentationSupport)`，默认只收 RGB（premultiplied electrical、identity、full range）；渲染器读不了 YUV，就别声明。
- 读客户端声明：`Surface::image_description()`（空 = 没声明，当 sRGB）、`render_intent()`、`color_representation()`；都是粘性双缓冲状态。`ImageDescription::is_srgb()` 是「不需要转换」的判断，`is_hdr()`。**两个渲染器都不在色彩空间之间换算**。

---

## 8. 渲染与缓冲协议 (`luminaria.render`)

### 8.1 LinuxDmabuf
- **`LinuxDmabuf`** (`luminaria.render:linux_dmabuf`):
  - `static Result<LinuxDmabuf> create(Display& display, dev_t main_device, std::span<const DmabufFormat> formats)`
  - 广播混成器支持的 DRM 格式与 Modifier 列表，零拷贝导入客户端 GPU 缓冲。

### 8.2 DrmSyncobj (显式同步)
- **`DrmSyncobjManager`** (`luminaria.render:drm_syncobj`):
  - `wp_linux_drm_syncobj_manager_v1` 协议支持，基于 Timeline Semaphore 实现无 CPU 阻塞的 GPU Acquire/Release Fence 编排。

### 8.3 Screencopy (截图与捕获)
- **`ScreencopyManager`** (`luminaria.render:screencopy`):
  - 整合 `zwlr_screencopy_manager_v1` 与 `ext_image_copy_capture_manager_v1`。
  - `void set_cursor_source(std::function<CursorCapture(Output&)> source)`: 支持捕获独立硬件光标或合成光标层。
  - **单窗口捕获**：`Status enable_toplevel_capture(Display&, ToplevelResolveFunc)` 发布 `ext_foreign_toplevel_image_capture_source_manager_v1`，客户端手里的 `ext_foreign_toplevel_handle_v1` 于是能当 ext-image-copy-capture 的源。`ToplevelResolveFunc` 是 `Toplevel*(wl_resource* handle)`——用桌面模块的 `ForeignToplevelList::toplevel_for`（`luminaria.render` 按 ADR 0008 不依赖 `luminaria.desktop`，所以是函数而不是类型）。窗口从自己的表面树合成，与遮挡、最小化、布局无关；按窗口自己的 commit 出帧，没变化就把 `capture` 扣住；缩放时回 `buffer_constraints` 并重发尺寸；销毁时 `stopped`。`set_toplevel_capture(ToplevelCaptureFunc)` 换掉默认的 CPU 渲染（默认读 shm / 单像素 / LINEAR dmabuf，tiled dmabuf 要自己读回）。

---

## 9. 桌面专用特权协议 (`luminaria.desktop`)

### 9.1 Workspace (工作区)
- **`WorkspaceManager`** (`luminaria.desktop:workspace`): `ext-workspace-v1`
  - 管理工作区组与工作区句柄，通告活跃状态并处理客户端切换请求。

### 9.2 ForeignToplevel (顶层窗口监控)
- **`ForeignToplevelManager`** (`luminaria.desktop:foreign_toplevel`): `zwlr_foreign_toplevel_management_v1`
  - 供任务栏（Dock/Panel）列出、聚焦、最小化、最大化与关闭其他应用的顶层窗口。

- **`ForeignToplevelList`** (`luminaria.desktop:foreign_toplevel_list`): `ext_foreign_toplevel_list_v1`
  - 只读窗口列表（标题、app id、不透明标识符），不能激活或关闭。`track(XdgShell&)` / `add(Toplevel&)`；`identifier(toplevel)`；静态 `toplevel_for(wl_resource* handle)` 把句柄解析回窗口，是 `ScreencopyManager::enable_toplevel_capture()` 的接口；`toplevel_count()`。

### 9.3 DataControl (剪贴板管理器)
- **`DataControlManager`** (`luminaria.desktop:data_control`): `zwlr_data_control_v1`
  - 允许剪贴板管理器监听并注入剪贴板与主选区数据。

### 9.4 SessionLock (锁屏)
- **`SessionLockManager`** (`luminaria.desktop:session_lock`): `ext-session-lock-v1`
  - 独占锁屏协议。**严格遵循 Fails Closed 原则**：锁屏客户端意外崩溃时保持会话锁定，防止凭据暴露。

### 9.5 InputMethod (输入法引擎)
- **`InputMethodManager`** (`luminaria.desktop:input_method`): `zwp_input_method_v2`
  - 输入法键盘抓取与候选字浮窗，与 `TextInput` 自动桥接。

### 9.6 GammaControl (伽马曲线调节)
- **`GammaControlManager`** (`luminaria.desktop:gamma_control`): `zwlr_gamma_control_manager_v1`
  - 管理输出的 16 位 RGB 查找表（夜间模式、色温调节），支持设置通过 shm fd 传输的 gamma table，销毁时自动还原默认查找表。

### 9.7 CtmControl (色彩变换矩阵)
- **`CtmControlManager`** (`luminaria.desktop:ctm_control`): `hyprland_ctm_control_v1` (v2)
  - 允许特权客户端配置输出的 3×3 行优先色彩变换矩阵（CTM），带非负值检验与独占互斥控制。

### 9.8 VirtualKeyboard (虚拟键盘输入注入)
- **`VirtualKeyboardManager`** (`luminaria.desktop:virtual_keyboard`): `zwp_virtual_keyboard_v1`
  - 接收虚拟键盘注入事件，提供 `key_event()` 与 `modifiers_event()` 信号。

### 9.9 VirtualPointer (虚拟指针输入注入)
- **`VirtualPointerManager`** (`luminaria.desktop:virtual_pointer`): `zwlr_virtual_pointer_v1` (v2)
  - 接收虚拟指针相对位移、绝对坐标定位、按钮、滚轮与帧同步事件。

---

## 10. 架构约束、生命周期与安全规矩

---


为确保系统稳健性，使用 Luminaria 时必须严格遵守以下法则：

1. **对象销毁逆序**：`SceneRenderer` / `VulkanRenderer` 必须在缓存其纹理的对象（`SceneOutput` / `Frame`、`Cursor`、`LinuxDmabuf`）之前声明，确保它们先于渲染器析构；不必早于 `Display`。
2. **表面引用跨事件循环隔离**：禁止跨 Dispatch 存储 `Surface*`，必须存储 `SurfaceId`，并在使用前通过 `surface_from_id(id)` 动态解析。
3. **坐标系与像素对齐**：
   - 布局、命中测试、子表面偏移**一律使用逻辑尺寸**（`surface_width()` / `surface_height()`）。
   - 仅在着色器渲染与内存拷贝时使用缓冲区物理像素（`buffer_width()` / `buffer_height()`）。
4. **帧回调时钟同步**：
   - `wl_surface.frame` 严禁在 commit 时直接回复。
   - 正常呈现时在 `Output::present` 中调用 `Frame::send_frame_done(time_ms)`。
   - `submit()` 返回 `Presented::unchanged` 时，必须在当次 `Output::frame` 处理器内补发 `send_frame_done()`，避免无 Damage 提交的客户端永久冻结。
5. **客户端内存防御性校验**：直接读取 `wl_shm` 或 DMA 缓冲时，必须先经 `layout_fits()` 校验 stride、offset 与内存长度，杜绝野指针越界。
6. **运行中途动态移除 Global 的安全模式**：使用 `WlGlobal::reset()` 或 `destroy_global_safe()`，先通过 `wl_global_remove()` 撤回公告，并在事件循环定时器上延迟释放底层的 `wl_global`，避免客户端飞行中的 `wl_registry.bind` 触发致命协议错误；配合 `GlobalBindingTracker` 自动登记绑定的 manager 资源并在全局对象析构时执行 `detach_resource()`，彻底杜绝 use-after-free。
