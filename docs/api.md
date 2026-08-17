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
4. [后端与硬件抽象 (`luminaria` / `luminaria.gpu`)](#4-后端与硬件抽象-luminaria--luminariagpu)
   - [4.1 Backend & Output 抽象](#41-backend--output-抽象)
   - [4.2 Headless & Wayland 嵌套后端](#42-headless--wayland-嵌套后端)
   - [4.3 DrmBackend & Session & Libinput (Bare-metal)](#43-drmbackend--session--libinput-bare-metal)
5. [渲染器与合成引擎 (`luminaria` / `luminaria.gpu`)](#5-渲染器与合成引擎-luminaria--luminariagpu)
   - [5.1 CpuCompositor (CPU 立即模式合成)](#51-cpucompositor-cpu-立即模式合成)
   - [5.2 VulkanRenderer (GPU 零拷贝渲染)](#52-vulkanrenderer-gpu-零拷贝渲染)
   - [5.3 CursorTheme & Text/Font](#53-cursortheme--textfont)
6. [外壳层与即时模式帧账本 (`luminaria` / `luminaria.gpu`)](#6-外壳层与即时模式帧账本-luminaria--luminariagpu)
   - [6.1 Frame (GPU 帧账本与摆位差异追踪)](#61-frame-gpu-帧账本与摆位差异追踪)
   - [6.2 Scene & SceneRenderer (统一场景抽象)](#62-scene--scenerenderer-统一场景抽象)
   - [6.3 DirectScanout (直出决策)](#63-directscanout-直出决策)
   - [6.4 OutputLayout & Pointer & PopupTree & LayerManager](#64-outputlayout--pointer--popuptree--layermanager)
7. [Wayland 核心协议对象 (`luminaria`)](#7-wayland-核心协议对象-luminaria)
   - [7.1 Compositor & Surface & Subcompositor](#71-compositor--surface--subcompositor)
   - [7.2 XdgShell & LayerShell](#72-xdgshell--layershell)
   - [7.3 Seat & DataDevice & PrimarySelection](#73-seat--datadevice--primaryselection)
   - [7.4 PresentationTime & TearingControl & Fifo & CommitTiming](#74-presentationtime--tearingcontrol--fifo--committiming)
   - [7.5 Viewporter & FractionalScale & ContentType & XdgDecoration](#75-viewporter--fractionalscale--contenttype--xdgdecoration)
   - [7.6 PointerConstraints & RelativePointer & CursorShape](#76-pointerconstraints--relativepointer--cursorshape)
   - [7.7 TextInput & IdleInhibit & IdleNotify & BackgroundEffect](#77-textinput--idleinhibit--idlenotify--backgroundeffect)
8. [GPU 专属 Wayland 协议 (`luminaria.gpu`)](#8-gpu-专属-wayland-协议-luminariagpu)
   - [8.1 LinuxDmabuf](#81-linuxdmabuf)
   - [8.2 DrmSyncobj (显式同步)](#82-drmsyncobj-显式同步)
   - [8.3 Screencopy (截图与捕获)](#83-screencopy-截图与捕获)
9. [桌面专用特权协议 (`luminaria.desktop`)](#9-桌面专用特权协议-luminariadesktop)
   - [9.1 Workspace (工作区)](#91-workspace-工作区)
   - [9.2 ForeignToplevel (顶层窗口监控)](#92-foreigntoplevel-顶层窗口监控)
   - [9.3 DataControl (剪贴板管理器)](#93-datacontrol-剪贴板管理器)
   - [9.4 SessionLock (锁屏)](#94-sessionlock-锁屏)
   - [9.5 InputMethod (输入法引擎)](#95-inputmethod-输入法引擎)
   - [9.6 DesktopGlobals 便捷装配](#96-desktopglobals-便捷装配)
10. [X11 桥接 (`luminaria.xwayland`)](#10-x11-桥接-luminariaxwayland)
11. [架构约束、生命周期与安全规矩](#11-架构约束生命周期与安全规矩)

---

## 1. 架构概述与核心设计哲学

Luminaria 定位为**有主见的 Wayland 混成器 C++23 库**。抽象层次介于裸写 wlroots 与纯自研 X11 窗口管理器之间。

```
+-------------------------------------------------------------------------+
|                        Compositor Application                           |
|  (Window Placement, Tiling, Workspace Policy, Keybindings, Workflows)   |
+------------------------------------+------------------------------------+
|       luminaria.desktop            |         luminaria.xwayland         |
| (Workspace, ForeignToplevel, Lock) |    (Xwayland fork/exec, XWM)       |
+------------------------------------+------------------------------------+
|                         luminaria.gpu                                   |
|   (VulkanRenderer, Frame Ledger, DirectScanout, DRM, Libinput, Session) |
+-------------------------------------------------------------------------+
|                           luminaria (Core)                              |
|   (Display, EventLoop, Surface, Compositor, Seat, CpuCompositor, Scene) |
+-------------------------------------------------------------------------+
|               libwayland-server, Linux KMS/DRM, Vulkan-Hpp              |
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

Luminaria 废弃了传统 C/C++ 的公共头文件体系（无 `include/`），完全基于 C++23 命名模块构建：

| 模块名称 | 职责边界与依赖要求 | 典型应用场景 |
|---|---|---|
| `import luminaria;` | 核心主接口。无 Vulkan/DRM/Libinput/Libseat/XCB 依赖。包含 Display/EventLoop、核心协议、CPU 合成、Scene 表、Nested/Headless 后端。 | 基础混成器、无头测试、软件渲染、CI 测试环境 |
| `import luminaria.gpu;` | GPU 与裸机扩展。依赖 Vulkan-Hpp、libdrm、libinput、libseat。包含 `Frame` 帧账本、`VulkanRenderer`、`DirectScanout`、DRM/Libinput 后端与显式同步。 | 生产级裸机混成器、GPU 加速嵌套混成器 |
| `import luminaria.desktop;` | 桌面特权协议扩展。包含工作区、任务栏控制、剪贴板管理、锁屏与输入法引擎协议。 | 完整桌面环境（Panel、Dock、Lockscreen） |
| `import luminaria.xwayland;` | X11 兼容桥接。管理 Xwayland 进程并桥接基础 XWM。 | 运行传统 X11 客户端 |

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
  - `Connection connect(std::function<void(Event&)> slot)`: 注册观察者。
  - `void emit(Event& event)`: 触发信号（重入安全：触发期间新增的槽在下一轮才生效，移除的槽立即标记失效）。
  - `std::size_t slot_count() const noexcept`: 当前活跃槽位计数。
  - **`Signal<Event>::Connection`**: RAII 订阅句柄，移动语义。析构或调用 `disconnect()` 自动退订；当 `Signal` 先销毁时自动安全失效。

### 3.4 Geometry & Color & Math
- **`Box`** (`luminaria:box`): `{ int x, y, width, height; }`
  - `bool empty() const`, `bool contains(int px, int py) const`
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
- **`KeyRouter`** (`luminaria:input_router`): 快捷键吞噬追踪与修饰键同步器。
  - `void consume(uint32_t keycode)` / `bool unconsume(uint32_t keycode)`
  - `static void sync_modifiers(KeymapState&, Seat&, const ModifiersEvent&)`
  - `static void release_held_modifiers(KeymapState&, Seat&, const std::set<uint32_t>&)`

---

## 4. 后端与硬件抽象 (`luminaria` / `luminaria.gpu`)

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
  - 虚接口（由硬件后端重写）：`scanout_modifiers()`, `import_scanout()`, `commit_scanout()`, `set_mode()`, `modes()`.

### 4.2 Headless & Wayland 嵌套后端
- **`HeadlessBackend`** (`luminaria:headless`):
  - `static Result<HeadlessBackend> create(EventLoop loop, int width = 800, int height = 600)`
  - `Output& create_output(int width, int height)`: 动态创建虚拟无头输出。
- **`WaylandBackend`** (`luminaria:wayland`):
  - `static Result<WaylandBackend> create(EventLoop loop, Display& display, int width, int height, std::string_view title)`
  - 嵌套在宿主 Wayland 混成器内运行，自动转发输入事件与父级 keymap，支持父级 dmabuf 直出。

### 4.3 DrmBackend & Session & Libinput (Bare-metal, `luminaria.gpu`)
- **`Session`** (`luminaria.gpu:session`): 基于 libseat 的 VT 与设备权限仲裁。
  - `static Result<Session> create(EventLoop loop, std::string_view seat_name = "")`
  - `int open_device(std::string_view path)` / `void close_device(int fd)`
  - `bool active() const`, `Signal<SessionActive>& active_changed()`
- **`DrmBackend`** (`luminaria.gpu:drm`): KMS Atomic Modesetting 裸机显示驱动。
  - `static Result<DrmBackend> create(EventLoop loop, Session& session, std::string_view card_path = "")`
  - 监听 udev connector 热插拔，为每个活跃显示器构建 `DrmOutput`；支持硬件光标平面（Hardware Cursor Plane）、Atomic 翻页与显式 Fence（`IN_FENCE_FD` / `OUT_FENCE_PTR`）。
- **`LibinputBackend`** (`luminaria.gpu:libinput`): 裸机输入设备事件源。
  - `static Result<LibinputBackend> create(EventLoop loop, Session& session)`
  - 暴露输入信号：`key()`, `pointer_motion()`, `pointer_button()`, `pointer_axis()`, `touch_down()`, `touch_up()`, `touch_motion()`.
  - `const KeymapState& keymap_state() const`: 获取底座共享的 XKB 状态。

---

## 5. 渲染器与合成引擎 (`luminaria` / `luminaria.gpu`)

### 5.1 CpuCompositor (CPU 立即模式合成, `luminaria`)
专为无头模式、嵌套回退、测试及截屏设计的软件合成器：
- **`CpuView`**: `{ SurfaceId root; int x, y; Box clip; }`（`clip` 限制表面树绘制边界，平铺混成器无需手工擦除溢出）。
- **`CpuImage`**: `{ Box box; std::span<const Pixel> pixels; }`。
- **`using CpuItem = std::variant<RectFill, CpuView, CpuImage>;`**
- **`CpuCompositor`**:
  - `void composite(int width, int height, Color background, std::span<const CpuItem> items)`: 预乘 alpha source-over 混合。
  - `std::span<const Pixel> pixels() const`: 获取合成后的像素跨度。
- **`void send_frame_done(std::span<const SurfaceId> ids, uint32_t time_ms)`**: 自由函数，为 CPU 路径批量应答帧回调。

### 5.2 VulkanRenderer (GPU 零拷贝渲染, `luminaria.gpu`)
基于 `VK_EXT_external_memory_dma_buf` 与 `VK_EXT_image_drm_format_modifier` 的 GPU 渲染器：
- **`GpuTexture`**: 缓存客户端导入的 dmabuf 或上传的 shm 像素。
- **`ScanoutTarget`**: 分配并导出为 dmabuf 的渲染目标（KMS 直接上屏）。
- **`OffscreenTarget`**: 离屏渲染目标，兼具 Render Target 与 `GpuTexture` 能力（ADR 0005）。
- **`VulkanRenderer`**:
  - `static Result<VulkanRenderer> create(...)`: 初始化 Vulkan 实例与物理设备。
  - `Status render_to(ScanoutTarget& target, Color background, std::span<const GpuTextureFill> fills, const Region& repaint, const OutputMapping& mapping, RenderSync sync)`: 执行多边形合并渲染，支持不透明遮挡剔除、独立 Damage Scissor、Fence 异步编排。
  - `Result<std::vector<Pixel>> read_scanout(ScanoutTarget& target)`: 回读渲染结果至 CPU（用于无 dmabuf 呈现）。

### 5.3 CursorTheme & Text/Font
- **`CursorTheme`** (`luminaria:cursor_theme`):
  - `static Result<CursorTheme> load(std::string_view name, int size)`
  - `const CursorImage* get(std::string_view shape_name) const`
- **`Font`** (`luminaria.text`):
  - `static std::optional<Font> open(std::string_view pattern)`
  - `int measure(std::string_view utf8) const`, `int height() const`, `int ascent() const`

---

## 6. 外壳层与即时模式帧账本 (`luminaria` / `luminaria.gpu`)

### 6.1 Frame (GPU 帧账本与摆位差异追踪, `luminaria.gpu`)
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
  - 图元类型：`Kind::surface_tree`、`Kind::solid_rect`、`Kind::border`、`Kind::image`、`Kind::shadow`、`Kind::blur`。
  - `std::uint64_t tag`: 混成器自定义标识符（库不解析，命中测试原样返回）。
- **`SceneHit scene_hit_test(std::span<const SceneItem> scene, double x, double y)`**: 全图元命中测试。
- **`SceneRenderer`** (`luminaria.gpu:scene_renderer`):
  - 将 `std::vector<SceneItem>` 自动分发至 `Frame` 或 `CpuCompositor`，透明处理运行时 GPU 降级。

### 6.3 DirectScanout (直出决策, `luminaria.gpu`)
- **`DirectScanout`** (`luminaria.gpu:direct_scanout`): 评估单全屏表面是否满足 KMS 硬件直出要求（无旋转、无裁剪、格式匹配），并管理导入缓存与 `Surface::hold_buffer()`。

### 6.4 OutputLayout & Pointer & PopupTree & LayerManager
- **`OutputLayout`** (`luminaria:output_layout`): 全局逻辑坐标系管理。
  - `void add(Output&, int x, int y)`, `void add_auto(Output&)`, `void remove(const Output&)`
  - `Output* at(int x, int y) const`, `Box box_of(const Output&) const`
- **`Pointer`** (`luminaria:pointer`): 全局光标坐标与约束状态。
- **`PopupTree`** (`luminaria:popup`): XDG 弹出菜单（Popup）树管理与 Grab 仲裁。
- **`LayerManager`** (`luminaria:layer`): Layer-shell 四层拓扑与排版计算器。

---

## 7. Wayland 核心协议对象 (`luminaria`)

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
  - **`Toplevel`**: `configure(int w, int h, std::span<const State> states)`, `close()`, `set_bounds(int max_w, int max_h)`.
    - 请求信号：`request_maximize`, `request_fullscreen`, `request_minimize`, `request_move`, `request_resize`, `request_window_menu`.
  - **`Popup`**: `configure(const Box& geometry)`, `reposition(...)`, `popup_done()`.
- **`LayerShell`** (`luminaria:layer_shell`): `zwlr_layer_shell_v1` 实现。
  - `static Result<LayerShell> create(Display& display)`
  - `Signal<LayerSurface&>& new_surface()`
  - **`LayerSurface`**: `configure(int w, int h)`, `layer()`, `anchor()`, `exclusive_zone()`, `margin()`, `keyboard_interactivity()`.

### 7.3 Seat & DataDevice & PrimarySelection
- **`Seat`** (`luminaria:seat`):
  - `static Result<Seat> create(Display& display, std::string_view name)`
  - 权限声明：`void set_capabilities(bool pointer, bool keyboard, bool touch)`
  - 键盘输入：`void set_keymap(std::string_view text)`, `void notify_key(uint32_t code, bool pressed)`, `void notify_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)`
  - 指针输入：`void notify_pointer_motion(double sx, double sy)`, `void notify_pointer_button(uint32_t button, bool pressed)`, `void notify_pointer_axis(...)`
  - 焦点控制：`void set_keyboard_focus(Surface* surface)`, `void set_pointer_focus(Surface* surface, double sx, double sy)`
  - 信号：`cursor_change()`, `pointer_focus_changed()`, `keyboard_focus_changed()`
- **`DataDeviceManager` & `PrimarySelectionManager`** (`luminaria:data_device`): 剪贴板、主选区与拖放（DnD）支持。

### 7.4 PresentationTime & TearingControl & Fifo & CommitTiming
- **`PresentationTime`** (`luminaria:presentation_time`): `wp_presentation_time` 精准时间戳协议。
  - `void notify_presented(Surface& surface, const PresentationFeedback& feedback)`
  - `void notify_discarded(Surface& surface)`
- **`TearingControlManager`** (`luminaria:tearing_control`): 允许游戏等低延迟客户端申请异步撕裂翻页。
- **`FifoManager` & `CommitTimingManager`** (`luminaria:fifo`, `luminaria:commit_timing`): 提交节拍栅栏与定时提交控制。

### 7.5 Viewporter & FractionalScale & ContentType & XdgDecoration
- **`Viewporter`** (`luminaria:viewporter`): 客户端源裁剪与目标拉伸支持。
- **`FractionalScaleManager`** (`luminaria:fractional_scale`): `wp_fractional_scale_v1`（以 120 为基数的分数缩放通告）。
- **`ContentTypeManager`** (`luminaria:content_type`): 视频、游戏、图片等画质与显示模式提示。
- **`XdgDecorationManager`** (`luminaria:xdg_decoration`): 服务端/客户端装饰协商（默认答 Client-side）。

### 7.6 PointerConstraints & RelativePointer & CursorShape
- **`PointerConstraints`** (`luminaria:pointer_constraints`): 指针锁定（Locked Pointer）与约束（Confined Pointer）。
- **`RelativePointerManager`** (`luminaria:relative_pointer`): 相对运动事件（无界 FPS 视角控制）。
- **`CursorShapeManager`** (`luminaria:cursor_shape`): `wp_cursor_shape_v1` 标准系统光标形状协商。

### 7.7 TextInput & IdleInhibit & IdleNotify & BackgroundEffect
- **`TextInputManager`** (`luminaria:text_input`): `zwp_text_input_v3` 客户端文本输入。
- **`IdleInhibitManager`** (`luminaria:idle_inhibit`): 阻止系统休眠/锁屏。
- **`IdleNotifier`** (`luminaria:idle_notify`): 空闲状态超时监控。
- **`BackgroundEffectManager`** (`luminaria:background_effect`): 亚克力模糊区域协商。

---

## 8. GPU 专属 Wayland 协议 (`luminaria.gpu`)

### 8.1 LinuxDmabuf
- **`LinuxDmabuf`** (`luminaria.gpu:linux_dmabuf`):
  - `static Result<LinuxDmabuf> create(Display& display, dev_t main_device, std::span<const DmabufFormat> formats)`
  - 广播混成器支持的 DRM 格式与 Modifier 列表，零拷贝导入客户端 GPU 缓冲。

### 8.2 DrmSyncobj (显式同步)
- **`DrmSyncobjManager`** (`luminaria.gpu:drm_syncobj`):
  - `wp_linux_drm_syncobj_manager_v1` 协议支持，基于 Timeline Semaphore 实现无 CPU 阻塞的 GPU Acquire/Release Fence 编排。

### 8.3 Screencopy (截图与捕获)
- **`ScreencopyManager`** (`luminaria.gpu:screencopy`):
  - 整合 `zwlr_screencopy_manager_v1` 与 `ext_image_copy_capture_manager_v1`。
  - `void set_cursor_source(std::function<CursorCapture(Output&)> source)`: 支持捕获独立硬件光标或合成光标层。

---

## 9. 桌面专用特权协议 (`luminaria.desktop`)

### 9.1 Workspace (工作区)
- **`WorkspaceManager`** (`luminaria.desktop:workspace`): `ext-workspace-v1`
  - 管理工作区组与工作区句柄，通告活跃状态并处理客户端切换请求。

### 9.2 ForeignToplevel (顶层窗口监控)
- **`ForeignToplevelManager`** (`luminaria.desktop:foreign_toplevel`): `zwlr_foreign_toplevel_management_v1`
  - 供任务栏（Dock/Panel）列出、聚焦、最小化、最大化与关闭其他应用的顶层窗口。

### 9.3 DataControl (剪贴板管理器)
- **`DataControlManager`** (`luminaria.desktop:data_control`): `zwlr_data_control_v1`
  - 允许剪贴板管理器监听并注入剪贴板与主选区数据。

### 9.4 SessionLock (锁屏)
- **`SessionLockManager`** (`luminaria.desktop:session_lock`): `ext-session-lock-v1`
  - 独占锁屏协议。**严格遵循 Fails Closed 原则**：锁屏客户端意外崩溃时保持会话锁定，防止凭据暴露。

### 9.5 InputMethod (输入法引擎)
- **`InputMethodManager`** (`luminaria.desktop:input_method`): `zwp_input_method_v2`
  - 输入法键盘抓取与候选字浮窗，与 `TextInput` 自动桥接。

### 9.6 DesktopGlobals 便捷装配
- **`DesktopGlobals`** (`luminaria.desktop:desktop_globals`): 一键式为 Display 注册所有特权桌面协议。

---

## 10. X11 桥接 (`luminaria.xwayland`)

- **`Xwayland`** (`luminaria.xwayland`):
  - `static Result<Xwayland> create(Display& display, Compositor& compositor)`: 启动 Xwayland 进程并建立双向 Socket。
  - `Signal<XwaylandReady>& ready()`: X Server 启动并设置 `$DISPLAY` 完成信号。
  - 内置极简 XWM，自动响应 MapRequest 与 ConfigureRequest。

---

## 11. 架构约束、生命周期与安全规矩

为确保系统稳健性，使用 Luminaria 时必须严格遵守以下法则：

1. **对象销毁逆序**：`VulkanRenderer` 必须在 `Display` 之前声明（确保 `Frame` 与其持有的 `GpuTexture` 先于渲染器析构）。
2. **表面引用跨事件循环隔离**：禁止跨 Dispatch 存储 `Surface*`，必须存储 `SurfaceId`，并在使用前通过 `surface_from_id(id)` 动态解析。
3. **坐标系与像素对齐**：
   - 布局、命中测试、子表面偏移**一律使用逻辑尺寸**（`surface_width()` / `surface_height()`）。
   - 仅在着色器渲染与内存拷贝时使用缓冲区物理像素（`buffer_width()` / `buffer_height()`）。
4. **帧回调时钟同步**：
   - `wl_surface.frame` 严禁在 commit 时直接回复。
   - 正常呈现时在 `Output::present` 中调用 `Frame::send_frame_done(time_ms)`。
   - `submit()` 返回 `Presented::unchanged` 时，必须在当次 `Output::frame` 处理器内补发 `send_frame_done()`，避免无 Damage 提交的客户端永久冻结。
5. **客户端内存防御性校验**：直接读取 `wl_shm` 或 DMA 缓冲时，必须先经 `layout_fits()` 校验 stride、offset 与内存长度，杜绝野指针越界。
