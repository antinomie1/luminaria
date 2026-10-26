# Luminaria Rust API 文档

Luminaria 不仅仅是一个现成的混成器，更是一个灵活的现代化 C++23 Wayland 混成器**库**。

Rust 绑定与 C++ 库逐类型对应：像在 C++ 中一样，按需挑选与组合各个模块（`core`、`util`、`backend`、`protocol`、`gpu`、`desktop`、`shell`），构建无头测试环境、嵌套调试工具、专用嵌入式显示服务或自定义渲染管线的混成器。

---

## 模块总览

| 模块 | 对应 C++ 命名空间与模块 | 核心职责 |
|---|---|---|
| **[`core`](#1-core-模块)** | `luminaria:display`, `event_loop`, `expected` | Wayland 服务端根对象、事件循环驱动、文件描述符绑定、错误类型 |
| **[`util`](#2-util-模块)** | `luminaria:box`, `color`, `transform`, `region`, `keymap` | 纯几何矩形运算、颜色与色彩空间、朝向变换、不相交区域、XKB 键表与像素布局 |
| **[`backend`](#3-backend-模块)** | `luminaria:output`, `headless`, `wayland`, `drm`, `libinput` | 虚拟无头输出、嵌套窗口后端、裸机 DRM/KMS 输出、libinput 设备驱动与输入事件 |
| **[`protocol`](#4-protocol-模块)** | `luminaria:compositor`, `xdg_shell`, `seat`, `layer_shell` 等 | Wayland 核心协议对象（Compositor, SurfaceId, Seat 等）及 16 个核心扩展管理器 |
| **[`gpu`](#5-gpu-模块)** | `luminaria.gpu:*` | Vulkan 1.3 渲染器、即时帧账本 `Frame`、硬件光标、直出评估 `DirectScanout`、截屏协议 |
| **[`desktop`](#6-desktop-模块)** | `luminaria.desktop:*` | 任务栏同步、ext-workspace 工作区、ext-session-lock 锁屏、wlr-output-management 显示管理 |
| **[`shell`](#7-shell-模块)** | `luminaria:output_layout`, `scene`, `cpu_compositor` | 即时模式场景项定义、反向命中测试、多显示器坐标排版、纯 CPU 软件合成器 |

---

## 1. `core` 模块

### `Display`
拥有 `wl_display` 与 Wayland 事件循环的根对象。

```rust
use luminaria::core::Display;
use std::time::Duration;

// 1. 创建 Display 上下文
let display = Display::new()?;

// 2. 绑定客户端连接 Socket
let socket_name = display.add_socket_auto()?; // 例如 "wayland-0"
// 或指定名称：display.add_socket("wayland-custom")?;

// 3. 启用系统内置的 wl_shm 共享内存协议
display.init_shm()?;

// 4. 获取事件循环句柄（非拥有型）
let event_loop = display.event_loop();

// 5. 驱动事件分发（None 为阻塞等待，Some(0) 为立即非阻塞轮询）
let dispatched_events = display.dispatch(Some(Duration::ZERO))?;

// 6. 向所有客户端刷新待发送的缓冲区数据
display.flush_clients();

// 7. 进入阻塞运行直到被 terminate
// display.run();
// display.terminate();
```

#### 外部事件循环嵌入
`Display` 与 `EventLoop` 实现了 `std::os::fd::AsFd` 与 `AsRawFd`，可以直接注册至外部事件循环（如 `epoll`、`mio`、`calloop`）：
```rust
use std::os::fd::AsRawFd;

let raw_fd = display.as_raw_fd();
// 注册到外部 poller 监听可读事件...
// 当可读时调用非阻塞派发：
display.dispatch(Some(std::time::Duration::ZERO))?;
display.flush_clients();
```

### `KeyRouter`
辅助工具，用于记录已被快捷键机制消费（吞噬）的按下按键，确保对应的释放事件（Key Release）同步被吞噬，避免按键状态在客户端处卡死。

---

## 2. `util` 模块

包含高频调用的纯值几何与数学工具，完全在 Rust 栈内存中运行，零 FFI 损耗。

### `Rect`
二维整数边界矩形：
- `Rect::new(x, y, width, height)`
- `rect.is_empty() -> bool`
- `rect.contains(x: f64, y: f64) -> bool`
- `rect.intersects(&other) -> bool`
- `rect.intersection(&other) -> Option<Rect>`：两矩形相交重叠区域。
- `rect.united(&other) -> Rect`：最小包围外框。
- `rect.resized(edges, dx, dy, min)`：按边缘掩码（`util::edge::{TOP, BOTTOM, LEFT, RIGHT}`，即 xdg_toplevel 的 resize edge 位）进行交互式缩放计算。

### `Color`
不预乘的 RGBA 浮点颜色（0.0 ~ 1.0）：
- `Color::rgb(r, g, b)` / `Color::rgba(r, g, b, a)`
- `color.premultiplied() -> Color`：转换为预乘 Alpha 颜色。
- `color.to_argb8888() -> u32` / `Color::from_argb8888(u32)`：与打包的 32 位 ARGB 像素值互转。

### `Transform`
Wayland 8 种屏幕旋转与镜像朝向枚举：
- `Normal`, `Rotate90`, `Rotate180`, `Rotate270`, `Flipped`, `Flipped90`, `Flipped180`, `Flipped270`
- `t.swaps_axes() -> bool`：是否交换宽高轴向。
- `t.flipped() -> bool`：是否包含镜像反转。
- `t.rotation() -> i32`：旋转角度（0, 90, 180, 270）。
- `t.invert() -> Transform`：反向变换。
- `t.apply_size(w, h) -> (i32, i32)` / `t.apply_point(w, h, x, y) -> (i32, i32)`：变换坐标与尺寸。

### `Region`
由互不相交（Disjoint）的一组矩形构成的像素区域。底层采用 Ping-Pong 缓冲区算法，在进行多层遮挡剔除时避免产生任何堆内存分配：
- `Region::new()` / `Region::from_rect(rect)`
- `region.add(rect)` / `region.subtract_rect(rect)` / `region.subtract(&other_region)`
- `region.intersect_rect(rect)` / `region.intersect(&other_region)`
- `region.contains(x, y) -> bool`
- `region.extents() -> Rect`：获取整体外包围盒。
- `region.rects() -> Vec<Rect>`：读取切分后的所有无交集矩形切片。

### `KeymapState`
XKB 键盘编译与修饰键状态机：
```rust
use luminaria::util::KeymapState;

// 从系统布局名编译
let keymap = KeymapState::from_layout("us")?;
// 或从客户端传输的 xkb 文本字符串编译
// let keymap = KeymapState::from_text(&keymap_str)?;

let text = keymap.text(); // 发送给客户端的完整 keymap 文本
let keysym = keymap.keysym(1); // evdev code 1 (KEY_ESC) -> XKB keysym
let (depressed, latched, locked, group) = keymap.modifiers();
```

---

## 3. `backend` 模块

### `Output`
表示一块物理或虚拟显示输出表面：
- `output.width()`, `output.height()`：物理分辨率。
- `output.logical_width()`, `output.logical_height()`：结合缩放和朝向后的逻辑分辨率。
- `output.scale()`, `output.set_scale(scale)`：HiDPI 整数缩放。
- `output.transform()`, `output.set_transform(transform)`：旋转/翻转朝向。
- `output.schedule_frame()`：请求一次硬件帧事件。
- `output.modes()`, `output.current_mode()`, `output.set_mode(w, h, refresh_mhz)`：模式管理。
- `output.commit(color)`：提交全屏纯色。
- `output.commit_frame(rgba_bytes, width, height)`：提交整帧软件渲染像素。
- `output.scanout_modifiers(drm_format)`：查询硬件扫描线支持的 DRM 格式修饰符列表。
- `output.move_cursor(x, y)` / `output.hide_cursor()`：驱动硬件独立 KMS 光标平面。

### 常用后端
- **`HeadlessBackend`**：无头后端。
  ```rust
  let mut backend = HeadlessBackend::new(&display, 16)?; // 16ms 帧间隔
  let mut output = backend.add_output(1920, 1080)?;
  backend.start()?;
  ```
- **`WaylandBackend`**：宿主嵌套窗口后端。
  ```rust
  let mut backend = WaylandBackend::new(&display)?;
  let mut output = backend.add_output(1280, 800)?;
  backend.start()?;
  ```
- **`DrmBackend`** / **`LibinputBackend`** / **`Session`**：裸机驱动栈（须在独立空闲 VT 环境下运行）。

---

## 4. `protocol` 模块

负责向客户端通告和处理 Wayland 协议规范。

### `Compositor` 与 `SurfaceId`
- `Compositor::new(&display)?`：发布 `wl_compositor`。
- **`SurfaceId`**：表面的代际标识符。严禁在事件调度之间持有裸 `Surface*` 指针。调用方法前即时解析，若表面已销毁则方法安全静默无害：
  - `surface_id.surface_width()`, `surface_id.surface_height()`
  - `surface_id.buffer_width()`, `surface_id.buffer_height()`
  - `surface_id.buffer_scale()`, `surface_id.buffer_transform()`
  - `surface_id.has_buffer() -> bool`
  - `surface_id.damage() -> Vec<Rect>`：当前帧损伤区域。
  - `surface_id.send_frame_done(time_ms)`：释放帧回调节拍。
  - `surface_id.accepts_input(sx, sy) -> bool`：基于输入区域的命中测试。

### `Seat`
发布 `wl_seat`，控制键盘焦点、鼠标光标、按键与触摸：
```rust
let mut seat = Seat::new(&display, "seat0")?;
seat.set_capabilities(true, true, true); // keyboard, pointer, touch

// 键盘控制
seat.set_keymap(&keymap_text);
seat.set_keyboard_focus(Some(surface_id));
seat.notify_key(28 /* KEY_ENTER */, true);

// 指针控制
seat.pointer_enter(surface_id, 10.0, 20.0);
seat.pointer_motion(15.0, 25.0);
seat.pointer_button(272 /* BTN_LEFT */, true);
seat.pointer_axis(0.0, 12.0); // 滚轮滚动

// 触摸屏触点
seat.touch_down(surface_id, 0 /* slot */, 100.0, 200.0);
seat.touch_motion(0, 105.0, 205.0);
seat.touch_up(0);
seat.touch_frame();
```

### `XdgShell`
发布 `xdg_wm_base`（支持 toplevels 和 popups）：
```rust
let mut xdg_shell = XdgShell::new(&display)?;
xdg_shell.set_bounds(1920, 1080);
```

### 全套协议扩展管理器（开箱即用）
所有管理器均采用统一构建规范，内部 RAII 管理协议生命周期：
```rust
let _subcompositor   = Subcompositor::new(&display)?;
let _layer_shell     = LayerShell::new(&display)?;
let _viewporter      = Viewporter::new(&display)?;
let _fractional      = FractionalScaleManager::new(&display)?;
let _activation      = XdgActivation::new(&display)?;
let _decoration      = XdgDecorationManager::new(&display)?;
let _cursor_shape    = CursorShapeManager::new(&display)?;
let _idle_inhibit    = IdleInhibitManager::new(&display)?;
let _idle_notify     = IdleNotifier::new(&display)?;
let _single_pixel    = SinglePixelBufferManager::new(&display)?;
let _tearing         = TearingControlManager::new(&display)?;
let _content_type    = ContentTypeManager::new(&display)?;
let _fifo            = FifoManager::new(&display)?;
let _commit_timing   = CommitTimingManager::new(&display)?;
let _presentation    = Presentation::new(&display)?;
let _bg_effect       = BackgroundEffectManager::new(&display)?;
let _icon_mgr        = XdgToplevelIconManager::new(&display)?;

// 依赖 Seat 的席位协议
let _data_device     = DataDeviceManager::new(&display, &seat)?;
let _primary_sel     = PrimarySelectionManager::new(&display, &seat)?;
let _relative_ptr    = RelativePointerManager::new(&display, &seat)?;
let _ptr_constraints = PointerConstraints::new(&display, &seat)?;
let _text_input      = TextInputManager::new(&display, &seat)?;
```

---

## 5. `gpu` 模块

对应 `luminaria.gpu` 扩展，包含基于 Vulkan 1.3 的高性能图形管线与裸机加速支持。

### `VulkanRenderer`
- `VulkanRenderer::new(&VulkanOptions::default())`：初始化 Vulkan 1.3 上下文（要求 Dynamic Rendering、Timeline Semaphore 与 dmabuf 扩展支持）。
- `vulkan.scanout_modifiers(drm_format)`：读取显卡支持的内存布局 modifier。

### `SceneRenderer` 与 `SceneOutput`
全自动具备 GPU 混合及 CPU 降级能力的场景呈现引擎：
```rust
use luminaria::gpu::{SceneRenderer, SceneOutput, VulkanOptions, SceneOutcome};
use luminaria::util::Color;

let mut renderer = SceneRenderer::new(&VulkanOptions::default())?;
let mut state = SceneOutput::new();

// 在输出模式变更或初始化时重置缓冲区绑定
renderer.reset(&mut state, &mut output, 0x34325241 /* DRM_FORMAT_ARGB8888 */)?;

// 提交场景列表进行呈现
let outcome = renderer.present(&mut state, &mut output, &scene_items, Color::rgb(0.0, 0.0, 0.0))?;
match outcome {
    SceneOutcome::Composited => { /* GPU 成功合成 */ }
    SceneOutcome::Scanout => { /* 客户端全屏零拷贝直出 */ }
    SceneOutcome::Unchanged => { /* 画面与上一帧完全相同，跳过重绘 */ }
    SceneOutcome::Cpu => { /* 降级至 CPU 软件混合 */ }
}
```

### `Frame`
逐输出的即时模式 GPU 帧账本。如果需要手动绕过 `SceneRenderer` 直接驱动 Vulkan 帧绘制，可使用 `Frame`：
- `Frame::new(&mut output, &mut vulkan)?`
- `frame.begin(viewport)`
- `frame.place_rect(rect, color)`
- `frame.submit(background)`
- `frame.send_frame_done(time_ms)`

### `DirectScanout`
全屏直出评估器，自动探测全屏客户端 buffer 是否满足当前输出的 DRM 格式与修饰符要求，满足时直接送交 CRTC 显示，跳过 GPU 混合步骤。

---

## 6. `desktop` 模块

对应 `luminaria.desktop` 模块，提供受权限保护的桌面级协议服务。

- **`ForeignToplevelManager`**：为外部任务栏（waybar 等）提供窗口列表同步与激活事件响应：
  ```rust
  let mut ftm = ForeignToplevelManager::new(&display)?;
  ftm.track(&mut xdg_shell); // 自动镜像跟踪 xdg_shell 内部所有窗口生命周期
  ```
- **`WorkspaceManager`**：为分页器提供 `ext-workspace` 支持：
  ```rust
  let mut wm = WorkspaceManager::new(&display)?;
  let group = wm.create_group();
  let ws = wm.create_workspace(group, "Workspace 1");
  wm.activate_workspace(ws);
  ```
- **`SessionLockManager`**：`ext-session-lock` 锁屏协议。锁定期间仅允许绘制锁屏界面（`is_locked()`）。
- **`OutputManager`**：为 `kanshi`、`wlr-randr` 等外部多屏配置工具提供查询与动态布局修改接口。
- **`DataControlManager`**：为 `wl-clipboard` 等无窗口剪贴板工具提供特权读取与写入接口。
- **`InputMethodManager`**：为 Fcitx / IBus 等输入法引擎提供候选词窗口绑定与键盘抢占支持。

---

## 7. `shell` 模块

### `OutputLayout`
维护多屏幕在全局逻辑坐标系中的摆放格局：
```rust
use luminaria::shell::OutputLayout;

let mut layout = OutputLayout::new()?;
layout.add(&mut output1, 0, 0);
layout.add(&mut output2, 1920, 0); // 拼接在右侧

let box1 = layout.output_box(&output1);
```

### `SceneItem` 与 `scene_hit_test`
纯即时模式绘制项，支持丰富的现代混成器视觉特效：
```rust
use luminaria::shell::{SceneItem, scene_hit_test};
use luminaria::util::{Rect, Color};

let items = vec![
    // 纯色背景矩形
    SceneItem::rect(Rect::new(0, 0, 1920, 1080), Color::rgb(0.1, 0.1, 0.1)),

    // 客户端窗口（附带圆角、阴影和磨砂模糊）
    SceneItem::surface(surface_id, Rect::new(100, 100, 800, 600), 100, 100)
        .with_corner_radius(12.0)
        .with_shadow(Color::rgba(0.0, 0.0, 0.0, 0.6), 16.0)
        .with_blur(false /* xray */)
        .with_tag(42),

    // 高亮轮廓线
    SceneItem::border(Rect::new(100, 100, 800, 600), 2, Color::rgb(0.4, 0.6, 0.9)),
];

// 反向命中测试（从最顶层向最底层检索）：
let hit = scene_hit_test(&items, 150.0, 150.0);
if hit.hit {
    println!("命中表面: {:?}, 自定义标记: {}", hit.surface, hit.tag);
}
```

### `CpuCompositor`
纯软件 CPU 光栅化混合器，无需 GPU 支持即可将 `Rect` 与图像合成至线性内存 RGBA 缓冲区，适用于无头环境或截屏输出。

---

## 8. 实战：从零组合一个混成器

以下代码从零组装一个具备 Wayland 核心协议与 CPU 软件合成能力的混成器，并合成一帧。完整可用的混成器（窗口栈、点击置顶、快捷键）见 `examples/minimal.rs`：

```rust
use std::time::Duration;
use luminaria::core::Display;
use luminaria::backend::HeadlessBackend;
use luminaria::protocol::{Compositor, XdgShell, Seat, OutputGlobal};
use luminaria::shell::{CpuCompositor, OutputLayout, SceneItem};
use luminaria::util::{Color, Rect};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. 初始化核心显示与事件循环
    let display = Display::new()?;
    let socket = display.add_socket_auto()?;
    println!("WAYLAND_DISPLAY={socket}");
    display.init_shm()?;

    // 2. 创建无头输出后端并启动
    let mut backend = HeadlessBackend::new(&display, 16)?;
    let mut output = backend.add_output(1024, 768)?;
    backend.start()?;

    // 3. 注册协议接口
    let _compositor = Compositor::new(&display)?;
    let mut xdg_shell = XdgShell::new(&display)?;
    xdg_shell.set_bounds(1024, 768);
    let mut seat = Seat::new(&display, "seat0")?;
    seat.set_capabilities(true, true, false);

    // 4. 多屏坐标排版
    let mut layout = OutputLayout::new()?;
    layout.add(&mut output, 0, 0);

    // 5. 向客户端发布输出全局描述符
    let _out_global = OutputGlobal::new(&display, 1024, 768, "HEADLESS-1")?;

    // 6. 构造场景并完成一帧合成
    let mut cpu_comp = CpuCompositor::new()?;
    let items = vec![
        SceneItem::rect(Rect::new(0, 0, 1024, 768), Color::rgb(0.12, 0.12, 0.18)),
        SceneItem::rect(Rect::new(80, 80, 400, 300), Color::rgb(0.2, 0.5, 0.8)),
        SceneItem::border(Rect::new(80, 80, 400, 300), 2, Color::rgb(1.0, 1.0, 1.0)),
    ];

    for item in &items {
        cpu_comp.composite_rect(item.rect, item.color);
    }
    let pixels = cpu_comp.finish(1024, 768, Color::rgb(0.0, 0.0, 0.0));
    output.commit_frame(&pixels, 1024, 768)?;

    // 7. 单步驱动事件循环
    display.dispatch(Some(Duration::ZERO))?;
    display.flush_clients();

    println!("Custom compositor loop tick completed successfully.");
    Ok(())
}
```
