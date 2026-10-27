# Luminaria Rust API 文档

Luminaria 是写 Wayland 混成器的**库**，不是现成的混成器：它提供协议、后端与渲染，窗口放哪、谁拿键盘、按键做什么由使用者决定。

Rust 绑定与 C++ 库逐类型对应：像在 C++ 中一样，按需挑选与组合各个模块（`core`、`util`、`backend`、`protocol`、`gpu`、`desktop`、`text`、`shell`），构建无头测试环境、嵌套调试工具、专用嵌入式显示服务或自定义渲染管线的混成器。

## 特性开关：按模块付费

C++ 的模块边界（ADR 0003）在绑定里是一比一复制的：**每个模块一个 `#[cxx::bridge]`
（`src/ffi.rs` / `src/ffi_gpu.rs` / `src/ffi_desktop.rs` / `src/ffi_text.rs`）、一个 shim 翻译单元
（`luminaria_rs*.cpp`）**，cargo 特性决定编译哪些、链接哪些。关掉的模块不编译、不链接，
也不探测它的系统库。

| 特性 | 默认 | C++ 模块 | Rust 模块 / 类型 | 额外链接的系统库 |
|---|---|---|---|---|
| *（始终有）* | — | `luminaria` | `core`、`util`、`backend`（`Output`/`HeadlessBackend`/`WaylandBackend`）、`protocol`、`shell` | wayland-server、wayland-client、xkbcommon |
| `gpu` | ✅ | `luminaria.gpu` | `gpu`，以及 `backend::{DrmBackend, LibinputBackend, Session}` | vulkan、libdrm、libinput、libudev、gbm、libseat |
| `desktop` | ✅ | `luminaria.desktop` | `desktop` | —（依赖 `gpu`） |
| `text` | ❌ | `luminaria.text` | `text` | fcft、fontconfig、pixman |
| `raw-vulkan` | ❌ | —（隐含 `gpu`） | `VulkanRenderer::raw_handles()`、`SceneRenderer::raw_vulkan_handles()` | — |
| `mint` / `glam` | ❌ | — | `util` 里几何类型的 `From`/`Into` | — |

```sh
cargo test --no-default-features                 # 只有核心：ldd 里没有 vulkan/gbm/libseat/libinput
cargo test --no-default-features --features gpu  # 裸机/GPU，不要桌面特权协议
cargo test                                       # 默认 = gpu + desktop，与 0.0.1 之前完全一致
cargo test --features text                       # 加上 Font
```

`desktop` 依赖 `gpu`：`DesktopGlobals::create` 要拿一个 `VulkanRenderer`。`raw-vulkan`
依赖 `gpu`：没有渲染器就没有句柄。

---

## 模块总览

| 模块 | 对应 C++ 命名空间与模块 | 核心职责 |
|---|---|---|
| **[`core`](#1-core-模块)** | `luminaria:display`, `event_loop`, `expected` | Wayland 服务端根对象、事件循环驱动、文件描述符绑定、错误类型 |
| **[`util`](#2-util-模块)** | `luminaria:box`, `color`, `transform`, `region`, `keymap` | 纯几何矩形运算、颜色与色彩空间、朝向变换、不相交区域、XKB 键表与像素布局 |
| **[`backend`](#3-backend-模块)** | `luminaria:output`, `headless`, `wayland`（`drm`/`libinput` 属 `luminaria.gpu`） | 虚拟无头输出、嵌套窗口后端；特性 `gpu` 下还有裸机 DRM/KMS 输出、libinput 设备驱动 |
| **[`protocol`](#4-protocol-模块)** | `luminaria:compositor`, `xdg_shell`, `seat`, `layer_shell` 等 | Wayland 核心协议对象（Compositor, SurfaceId, Seat 等）及 16 个核心扩展管理器 |
| **[`gpu`](#5-gpu-模块)** | `luminaria.gpu:*` | Vulkan 1.3 渲染器、即时帧账本 `Frame`、硬件光标、直出评估 `DirectScanout`、截屏协议 |
| **[`desktop`](#6-desktop-模块)** | `luminaria.desktop:*` | 任务栏同步、ext-workspace 工作区、ext-session-lock 锁屏、wlr-output-management 显示管理 |
| **[`shell`](#7-shell-模块)** | `luminaria:output_layout`, `scene`, `cpu_compositor` | 即时模式场景项定义、反向命中测试、多显示器坐标排版、纯 CPU 软件合成器 |
| **[`text`](#10-text-模块特性-text)** | `luminaria.text` | fcft 字形栅格化与字体度量 |

---

## 借用关系：析构顺序由编译器检查

C++ 对象互相持有引用：`lm::Cursor` 存着 `SceneRenderer&`、`OutputLayout&`、`Seat&`，每个 global 都挂在
`wl_display` 上。绑定把这些关系写成 Rust 的生命周期参数，被引用的一方先 drop 就编译不过：

| 类型 | 借用的对象 |
|---|---|
| 所有协议 global（`Compositor<'d>`、`Seat<'d>`、`XdgShell<'d>`、`Presentation<'d>`……）、`HeadlessBackend<'d>`、`WaylandBackend<'d>`、`Session<'d>`、`EventLoop<'d>` | `Display` |
| `DrmBackend<'a>`、`LibinputBackend<'a>` | `Display`，以及传入的 `Session` |
| `DataDeviceManager<'a>`、`PrimarySelectionManager<'a>`、`RelativePointerManager<'a>`、`PointerConstraints<'a>`、`TextInputManager<'a>` | `Display`、`Seat` |
| `DataControlManager<'a>` | `Display`、`DataDeviceManager` |
| `InputMethodManager<'a>` | `Display`、`Seat`、`TextInputManager` |
| `OutputManager<'a>` | `Display`、`OutputLayout` |
| `ForeignToplevelManager<'d>` | `Display`；`track(&shell)` 之后还有那个 `XdgShell` |
| `LinuxDmabuf<'a>`、`DesktopGlobals<'a>` | `Display`，以及传入的 `VulkanRenderer` 或 `SceneRenderer` |
| `Cursor<'a>` | `SceneRenderer`、`OutputLayout`、`Seat` |
| `SceneOutput<'r>` | 第一次 `reset()` / `present()` 它的 `SceneRenderer`（它缓存该渲染器的纹理与渲染目标） |
| `Frame<'r>` | `VulkanRenderer` |
| `Toplevel<'s>`、`Popup<'s>`、`LayerSurface<'s>`、`LockSurface<'m>`、`SessionLock<'m>`、`PointerConstraint<'m>`、`TextInput<'m>`、`InputMethod<'m>` | 发出它的那个 global（只是查询句柄，见 §4"事件"） |

几点说明：

- **借用是共享借用。** 被借的类型——`Seat`、`XdgShell`、`OutputLayout`、`SceneRenderer`、`Session`——方法都取
  `&self`，借出之后照常使用。这是安全的：包装类型都不是 `Send`/`Sync`，C++ 从不回调 Rust，也不把内部引用交给 Rust。
- **只写 lifetime 不够，还要 `Drop`。** 没有 `Drop` 的结构体，dropck 允许它在借用结束后才被隐式析构，
  `drop(seat)` 放在 `Cursor` 之前照样能编译——而 `Cursor` 的析构恰好要碰那个 `Seat`。所以每个带 lifetime 的包装都
  带一个有 `Drop` 的标记字段。`Cursor` 的文档里有一个 `compile_fail` doctest 守着这一点。
- **`SceneRenderer` 不借任何东西**，所以"renderer 比 Display 活得久"仍然写得出来：最先声明它即可。
  `set_presentation(Some(&p))` 因此没有借用：renderer 一般比 `Presentation` 活得久，这条链接由 shim 从两头维护，
  哪边先死哪边负责断开，之后反馈只是不再发送。
- **`Output` 不进生命周期。** 真正的 `lm::Output` 归后端所有，显示器随时可能被拔掉，这不是 Rust 的作用域能表达的。
  所以和输出有关的都在运行时兜底：`Output` 自己变成空操作（`is_alive()` 为 false）；`OutputLayout` 与 `SceneOutput`
  自动解除绑定；`DirectScanout` 不再调用它；`Frame` 随输出一起销毁，之后 `reset` / `submit` 返回错误、其余方法为空操作。
- **结构体不能同时装被借者和借用者**（自引用）。把被借的放在外层——`main` 的局部变量，或一个只装它们的结构体——
  再让装 `Cursor<'a>`、`SceneOutput<'a>` 的结构体借用它们，如 `examples/minimal.rs` 的 `Screen<'a>`、`Backend<'a>`。

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

// 4. 获取事件循环句柄（非拥有型，借用 display：EventLoop<'_>）
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

### `Pixel`
一个 RGBA8 像素，字节顺序与 C++ `luminaria::Pixel` 相同（`r, g, b, a`），也就是
`SceneRenderer::read_back()` 返回、`Output::commit_frame()` 接收的缓冲区格式。

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
- **`DrmBackend`** / **`LibinputBackend`** / **`Session`**：裸机驱动栈（须在独立空闲 VT 环境下运行）。显示器经 `DrmBackend::next_event()` 到来与离开，输入见下一节，VT 切换经 `Session::next_activity()`；见[第 9 节](#9-逃生口和别的-rust-crate-共用底层对象)。

### 输入：`InputSource`
嵌套的 `WaylandBackend` 与裸机的 `LibinputBackend` 都实现 `InputSource` trait，给出同一个 `InputEvent`。
两者的差别在于到来的是哪些分支（相对移动 `Motion` 与触摸来自 libinput，绝对移动 `MotionAbsolute` 与 `PointerLeave`
来自嵌套窗口），而不在于怎么到来，所以混成器只写一个循环：

```rust
if let Some(source) = backend.input() { // -> Option<&mut dyn InputSource>
    while let Some(event) = source.next_input_event() {
        match event {
            InputEvent::Key { key, pressed, keysym, modifiers } => { /* 快捷键，否则 seat.notify_key */ }
            InputEvent::Modifiers(m) => seat.notify_modifiers(m.depressed, m.latched, m.locked, m.group),
            InputEvent::KeymapChange(text) => { seat.set_keymap(&text); }
            InputEvent::Axis(axis) => seat.pointer_axis_event(axis),
            // Motion / MotionAbsolute / PointerLeave / Button / Capabilities / Touch*
            _ => {}
        }
    }
}
```

- `InputEvent::Key` 的 `keysym` 与 `modifiers` 是后端按 `keymap()` 读出、取**这个键之前**的状态：先按 Logo
  再按 Return，到来的是带着 Logo 的 Return。快捷键直接对着它匹配，不需要自己的 `KeymapState`。读数在 C++ 信号里
  就拍下，所以同一批后面的按键改掉 xkb 状态也不影响它。
- `MotionAbsolute { output }` 是 `OutputId`，指明坐标属于哪块输出。
- `Seat::pointer_axis_event(PointerAxis)`：滚轮同时报刻度和平滑量，刻度优先；分别调 `pointer_axis` 与
  `pointer_axis_discrete` 会让每格滚两次。
- `keymap()` / `capabilities()` 给出启动时已有的状态，之后的变化以 `KeymapChange` / `Capabilities` 到来。
- 父级要求关窗不是输入：`WaylandBackend::take_close_request()`。

---

## 4. `protocol` 模块

负责向客户端通告和处理 Wayland 协议规范。

### 事件：`next_event()`

混成器是被 C++ 的 `Signal` 驱动的。绑定不让 C++ 回调 Rust，而是每个 shim 把信号排进自己的队列，
Rust 在 `Display::dispatch` 返回后用 `next_event()` 一个个取走：

```rust
display.dispatch(Some(Duration::from_millis(16)))?;
while let Some(event) = shell.next_event() { /* XdgEvent */ }
while let Some((surface, event)) = layers.next_event() { /* LayerSurfaceEvent */ }
while let Some(event) = seat.next_event() { /* SeatEvent */ }
```

- **事件晚于信号到达。** 信号在 `dispatch` 里发出，Rust 看到时对象可能已经又变了、甚至没了。所以客户端创建的对象
  不以引用交给 Rust，而是以名字：有自己 surface 的（toplevel、popup、layer surface、锁屏 surface、指针约束、
  输入法弹窗）用那个 surface 的 `SurfaceId`，其余（锁、idle 通知与抑制器、text input、输入法）用绑定分配、
  永不复用的 id。`shell.toplevel(id)` 之类返回的句柄每次调用都重新查找；对象销毁后 getter 返回默认值、setter 不做事。
- **替 Rust 立刻做的事，由 shim 自己做。** 对象销毁时必须马上放开它的地方——`PopupTree`、`LayerManager`、
  `OutputLayout`——shim 监听销毁信号，不等 Rust 取到 `Destroy` 事件。
- **要同步回答的信号无法转发。** `XdgShell` 的 popup 约束查询改成事先告知（`set_popup_constraint_areas`）；
  xdg-activation 的发 token 请求总是批准。
- **绑定总在监听。** luminaria 对 `request_maximize` / `request_fullscreen` / `request_minimize` 有一个
  "没人监听就自己批准"的兜底；绑定每个都监听，所以这个兜底不会发生，回答是 Rust 的事（`examples/minimal.rs` 照兜底的样子回答）。
- **队列不取就一直长。** 用了某个 global，就在每次 `dispatch` 之后把它的队列取空。

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
  - `surface_id.surface_tree() -> Vec<SurfaceAt>`：这棵树（子表面在内，后到前），每项的 `x`/`y`
    是相对根表面的偏移；`SurfaceAt { surface, x, y }`。
  - `surface_id.set_preferred_buffer_scale(scale)` / `set_preferred_buffer_transform(t)`：
    告诉客户端按哪个缩放、哪个朝向准备 buffer（`wl_surface` v6 起）。

### `Seat`
发布 `wl_seat`，控制键盘焦点、鼠标光标、按键与触摸：
```rust
let seat = Seat::new(&display, "seat0")?;
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

`seat.next_event()` 报告路由的变化，无论谁引起：`SeatEvent::KeyboardFocus(Option<SurfaceId>)`、
`PointerFocus(Option<SurfaceId>)`、`CursorChange { surface, hotspot, hidden }`（指针焦点所在的客户端设置了光标；
指针离开时光标回到混成器自己的，报为 `surface: None, hidden: false`）。

### `XdgShell`
发布 `xdg_wm_base`（toplevel 与 popup）。`next_event()` 返回 `XdgEvent::Toplevel(SurfaceId, ToplevelEvent)` 或
`XdgEvent::Popup(SurfaceId, PopupEvent)`：

- `ToplevelEvent`：`Created`、`Map`、`Unmap`、`Destroy`；客户端的请求 `RequestMaximize(bool)`、`RequestFullscreen(bool)`、
  `RequestMinimize`、`RequestMove { serial }`、`RequestResize { serial, edges }`（`util::edge` 位掩码）、
  `RequestWindowMenu { serial, x, y }`；`IdentityChange`（标题 / app id）、`StateChange`（混成器的 setter 真的改了状态）、
  `ParentChange(Option<SurfaceId>)`。
- `PopupEvent`：`Created { parent }`（layer surface 的 popup 为 `None`）、`Map`、`Unmap`、`Destroy`、`Reposition`、`Grab { serial }`。

```rust
let shell = XdgShell::new(&display)?;
shell.set_bounds(1920, 1080);
while let Some(event) = shell.next_event() {
    match event {
        XdgEvent::Toplevel(s, ToplevelEvent::Created) => {
            let t = shell.toplevel(s).unwrap();
            t.set_maximized(true);
            t.configure(1920, 1080);
        }
        XdgEvent::Toplevel(s, ToplevelEvent::RequestFullscreen(on)) => {
            if let Some(t) = shell.toplevel(s) { t.set_fullscreen(on) }
        }
        XdgEvent::Popup(p, PopupEvent::Created { .. }) => { popups.add(&shell, p); }
        _ => {}
    }
}
```

- `Toplevel`：`title()`、`app_id()`、`parent()`、`is_mapped()`、`min_size()`、`max_size()`、`geometry()`（窗口几何，
  不含客户端阴影）、`pending_size()`、`states()`；`configure(w, h) -> serial`、`set_maximized` / `set_fullscreen` /
  `set_activated` / `set_resizing` / `set_tiled` / `set_minimized`、`close()`。`shell.toplevels()` 列出全部。
- `Popup`：`parent()`、`is_mapped()`、`has_grab()`、`geometry()`（相对父窗口几何）、`dismiss()`。
- `set_popup_constraint_areas(&[PopupArea { parent, parent_box, usable }])`：每个可能当 popup 父亲的 surface 画在哪、
  在哪块屏上。shell 摆 popup 时在 `dispatch` 里同步查询，只能用事先告诉它的；场景一变就更新。不在表里的父亲不受约束。

### `LayerShell`
发布 `zwlr_layer_shell_v1`（面板、壁纸、通知、锁屏界面）。`next_event()` 返回 `(SurfaceId, LayerSurfaceEvent)`：
`Created`、`Map`、`Unmap`、`Destroy`、`StateChange`（提交了新的层 / 锚点 / 尺寸 / 边距 / 独占区，重新排版）、
`NewPopup(SurfaceId)`（客户端把这个没有 xdg 父亲的 popup 挂到本 layer surface 上，这是唯一知道它归属的地方）。

`layers.layer_surface(id)` 的句柄：`scope()`、`layer()`、`anchor()`（`util::edge` 位掩码）、`desired_size()`、
`exclusive_zone()`、`exclusive_edge()`、`margins()`、`keyboard_interactivity()`、`is_mapped()`、`pending_size()`、
`has_output()` / `is_on(&output_global)`（客户端点名的输出）、`configure(w, h)`、`close()`、
`arrange(full, &mut usable) -> Rect`。整块输出一起排版用 `shell::LayerManager`（§7）。

### 其余协议的事件

| global | `next_event()` 返回 | 查询与回答 |
|---|---|---|
| `IdleInhibitManager` | `IdleInhibitEvent::{NewInhibitor { inhibitor, surface }, InhibitorDestroy, Changed(bool)}` | `is_inhibited()`、`inhibitor_count()`、`set_visible(id, bool)`（只有可见的抑制器生效） |
| `IdleNotifier` | `IdleNotifierEvent::{NewNotification, NotificationDestroy, StateChange { notification, idled }}` | `notify_activity()`、`set_inhibited()`、`notification(id)` |
| `XdgActivation` | `ActivationRequest { surface, token_valid, app_id, requesting_surface, serial }` | 给不给焦点由混成器决定 |
| `PointerConstraints` | `PointerConstraintEvent::{New, Destroy, RegionChange}(SurfaceId)` | `constraint(surface)`：`kind()`、`region()`、`cursor_hint()`、`activate()`（只在指针焦点在该 surface 时生效）、`deactivate()`；`active_constraint()` |
| `TextInputManager` | `TextInputEvent::{New, Enable, Disable, Commit, Destroy}(TextInputId)` | `focused()`、`text_input(id)`：`state()`、`surrounding_text()`、`send_preedit_string` / `send_commit_string` / `send_delete_surrounding_text` / `send_done` |
| `DataDeviceManager`、`PrimarySelectionManager` | `next_selection_change() -> Option<Vec<String>>`（新所有者提供的 MIME 类型，空表示清空） | `selection_mime_types()`、`selection_receive(mime, OwnedFd)`（写进管道写端，dispatch 后读另一端） |

### 协议扩展管理器
所有管理器均采用统一构建规范，内部 RAII 管理协议生命周期：
```rust
let _subcompositor   = Subcompositor::new(&display)?;
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
- `VulkanRenderer::new(&VulkanOptions::default())`：初始化 Vulkan 1.3 上下文（要求 Dynamic Rendering、Timeline Semaphore 与 dmabuf 扩展支持）。裸机上传 `VulkanOptions { drm_device: DrmDeviceId::from_fd(&drm) }`，让渲染器就是显示所在的那块 GPU。
- `vulkan.scanout_modifiers(drm_format)`：读取显卡支持的内存布局 modifier。

### `SceneRenderer` 与 `SceneOutput`
全自动具备 GPU 混合及 CPU 降级能力的场景呈现引擎：
```rust
use luminaria::gpu::{SceneRenderer, SceneOutput, VulkanOptions, SceneOutcome};
use luminaria::util::Color;

let renderer = SceneRenderer::new(&VulkanOptions::default())?;
let mut state = SceneOutput::new(); // 第一次 reset/present 后借用 renderer

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

### `Cursor`
指针及其图像：主题、cursor-shape、客户端精灵、KMS 光标平面。`Cursor::new(&renderer, &layout, &seat)?`
借用这三者直到它 drop（见上文"借用关系"）。`move_to` / `move_by` / `leave` / `position` / `refresh`。

### `Frame`
逐输出的即时模式 GPU 帧账本。如果需要手动绕过 `SceneRenderer` 直接驱动 Vulkan 帧绘制，可使用 `Frame`。
它借用 `VulkanRenderer`；输出被销毁时它随之失效（`reset` / `submit` 返回错误）：
- `Frame::new(&mut output, &vulkan)?`
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
  ftm.track(&xdg_shell); // 自动镜像跟踪 xdg_shell 内部所有窗口生命周期；此后 xdg_shell 被 ftm 借住
  ```
- **`WorkspaceManager`**：为分页器提供 `ext-workspace` 支持：
  ```rust
  let mut wm = WorkspaceManager::new(&display)?;
  let group = wm.create_group();
  let ws = wm.create_workspace(group, "Workspace 1");
  wm.activate_workspace(ws);
  ```
- **`SessionLockManager`**：`ext-session-lock` 锁屏协议。`next_event()` 返回 `SessionLockEvent`：`NewLock(LockId)`、
  `NewSurface { lock, surface }`、`SurfaceMap`、`SurfaceDestroy`、`LockDestroy { lock, unlocked }`。盖住所有输出后
  `locks.lock(id).send_locked()`，拒绝用 `send_finished()`；`lock_surface(surface)` 的句柄 `is_on(&output_global)`、
  `configure(w, h)`。它**失败即锁定**：锁屏客户端没解锁就死了，`unlocked` 为 false，`is_locked()` 仍为 true，
  绘制与输入应跟随 `is_locked()` 而不是锁屏客户端是否还在。
- **`OutputManager`**：为 `kanshi`、`wlr-randr` 等外部多屏配置工具提供查询与动态布局修改接口。
- **`DataControlManager`**：为 `wl-clipboard` 等无窗口剪贴板工具提供特权读取与写入接口。
- **`InputMethodManager`**：为 Fcitx / IBus 等输入法引擎提供候选词窗口绑定与键盘抢占支持；与 `TextInputManager`
  之间的桥接它自己完成。`next_event()` 返回 `InputMethodEvent`：`New`、`GrabChange { input_method, grabbed }`、`Destroy`、
  `NewPopupSurface { input_method, surface }`、`PopupSurfaceDestroy`。抢占键盘期间按键经 `input_method(id).grab_send_key(...)`
  送给输入法，候选窗口放在 `popup_text_input_rectangle(surface)` 旁边。

---

## 7. `shell` 模块

### `OutputLayout`
维护多屏幕在全局逻辑坐标系中的摆放格局：
```rust
use luminaria::shell::OutputLayout;

let layout = OutputLayout::new()?;
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

### `PopupTree`
已映射的 popup 及其位置：`popups.add(&shell, popup)`（layer surface 的 popup 用 `add_with_parent(&shell, popup, layer_surface)`）、
`set_mapped`、`append_to(&mut scene)`（每个 popup 接在父 surface 之上、按偏移摆放、带父窗口的 tag，子菜单嵌套）、
`contains`、`active_grab`、`dismiss_grabs_outside(seat.pointer_focus())`（点在所有 popup 外面就关掉抢占的那些，返回 true
时这次点击不该交给客户端）。popup 销毁时它自己放手，不借用 `XdgShell`。

### `LayerManager`
按输出管理 layer surface：`add(&layers, surface, output_id)`（`output_id` 由混成器自定）、`set_mapped`、
`arrange(output_id, full, usable) -> Rect`（配置每个 surface，返回扣掉独占区后剩下的区域，交给 `XdgShell::set_bounds`
并在其中摆窗口）、`append_to(&mut scene, output_id, Layer::Top)`、`keyboard_target(surface)`、`close_output(output_id)`。
同样自己放开销毁的 surface。

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
    let xdg_shell = XdgShell::new(&display)?;
    xdg_shell.set_bounds(1024, 768);
    let seat = Seat::new(&display, "seat0")?;
    seat.set_capabilities(true, true, false);

    // 4. 多屏坐标排版
    let layout = OutputLayout::new()?;
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

---

## 9. 逃生口：和别的 Rust crate 共用底层对象

绑定之外总有 luminaria 没覆盖的东西：自定义协议、自己读 DRM 属性、用 ash 往同一块 GPU 上提交工作。
为此绑定把几样底层对象**借**出去。四条共同的规矩：

- **只借不转让。** 借出的指针、fd、句柄与借出它的 luminaria 对象同寿，不得关闭、销毁或另存到它死之后。
- **不反向接管。** luminaria 不接收外来的 `wl_display`、DRM fd 或 `VkDevice`；对象始终由 luminaria 创建和销毁。
- **C++ 从不调用 Rust** 这条规矩只约束绑定自己。通过逃生口注册的 libwayland 回调会在 `display.dispatch()`
  里被直接调用：回调里不要 panic（2024 edition 的 `extern "C"` 遇到 unwind 直接 abort），也不要回头调用
  luminaria 的包装类型——照 shim 的做法，入队，等 `dispatch()` 返回再处理。
- **没有稳定性承诺**（ADR 0004），这一节的签名随 C++ 接口一起变。

### 9.1 `Display::as_ptr()`：原始 `wl_display*`

`Display::as_ptr() -> *mut c_void` 返回 luminaria 自己的 `wl_display`。拿到指针是安全的，用它是 `unsafe` 的；
指针在 `Display` drop 之前有效。

**能做的**：用 libwayland-server 的 C API（`wayland-sys` 的 `server` feature，或自己写 `extern "C"`）
`wl_global_create` 注册额外的 global、`wl_resource_create` 建资源、`wl_display_add_client_created_listener` 之类。
接口描述（`wl_interface`）可以手写，也可以取 `wayland-scanner` 生成代码里的 C 接口指针。
`tests/test_raw_display.rs` 在 luminaria 的 display 上用这种方式注册了一个 global，一个 libwayland 客户端在
registry 里同时看到它和 luminaria 的 global，并且绑定成功。

**不能做的**：把 `wayland-server` 的 `Display<D>`（即 `wayland-backend` 的 server `Backend`）挂到这个指针上。
`wayland-backend` 0.3.17 的服务端只会自己 `wl_display_create`，没有 `from_foreign_display`（只有客户端那半边有）。
所以高层的 `wayland-server` 类型系统用不上，只能走 C API；这要等上游加接口，绑定这边绕不过去。

**销毁顺序**，和 CLAUDE.md 里 C++ 一侧的规矩是同一条：

1. **global 先于 `Display` 死。** 把 `wl_global_destroy` 放进某个 Rust 对象的 `Drop`，并且**把这个对象声明在
   `Display` 之后**（局部变量按声明的逆序析构）。漏掉 `wl_global_destroy` 也不会出错（`wl_display_destroy`
   会一并释放），但那样 global 的 user data 必须活到 `Display` 之后。
2. **客户端的资源比 global 活得久。** 仍连着的客户端的资源，要等 `Display` drop 时
   （`wl_display_destroy_clients`）才销毁，那时 Rust 侧的 global 和它的状态都可能已经没了。资源的 destroy
   回调只能碰资源自己持有的东西（例如自己那份 `Arc`），或者由 global 的 `Drop` 把回指针清空。
   `test_raw_display.rs` 让客户端在 `Display` drop 时仍然连着，专门覆盖这一点。
3. **`wl_display_set_global_filter` 只有一个槽。** `DataControlManager` 的过滤器（C++ `set_filter`）用的就是它；
   Rust 侧另设一个会和它互相覆盖，所以不要设。

### 9.2 `DrmBackend: AsFd`，以及 `DrmDeviceId`

`DrmBackend` 实现 `AsFd` / `AsRawFd`，借出 card 的 fd（C++ `DrmBackend::fd()`）。DRM master 与 KMS 状态归
luminaria：借去的 fd 可以给 `drm` crate 做查询（connector、property、驱动名），`gbm` 需要自己拥有 fd 时用
`BorrowedFd::try_clone_to_owned()` 复制一份；但**不要**拿它做 modeset 或 atomic commit，那会和 luminaria
自己提交的 atomic 状态打架。

`gpu::DrmDeviceId` 对应 C++ 的 `DrmDeviceId`（设备节点的 major/minor），`DrmDeviceId::from_fd(&drm)` /
`DrmDeviceId::from_path("/dev/dri/card0")` 分别对应两个 `drm_device_id()` 重载。`VulkanOptions::drm_device`
随之从 `Option<String>` 改成 `Option<DrmDeviceId>`，这样渲染器选中的是这块屏幕所在的 GPU：

```rust
let drm = DrmBackend::new(&display, None, session.as_ref())?;
let options = VulkanOptions { drm_device: DrmDeviceId::from_fd(&drm) };
let renderer = SceneRenderer::new(&options)?;
```

### 9.3 裸机输入、显示器热插拔、VT 切换

绑定原先拿不到 `LibinputBackend` 的输入，也拿不到 `DrmBackend` 找到的显示器，所以 Rust 写不了裸机混成器。
补法与嵌套后端一样：shim 把信号排进队列，Rust 在 `dispatch()` 返回后取。

- **`LibinputBackend` 是一个 `InputSource`**（见 §3「输入」）：按键、修饰键、相对移动、按钮、滚轮
  （平滑量 + 刻度 + 停止标记）、触摸（down/motion/up/frame/cancel，坐标归一化到触摸屏）、设备能力变化。
  另有 `set_keymap()`，以及对应 `create_path()` 的 `LibinputBackend::new_path()`。
- **`DrmBackend::next_event() -> Option<DrmEvent>`**：`OutputAdded(Output)` 与 `OutputRemoved(OutputId)`。
  `start()` 时已有的显示器也以 `OutputAdded` 到来。显示器被拔掉后，手里的 `Output` 仍可安全调用，只是一切都成了
  空操作（`Output::is_alive()` 为 false），收到 `OutputRemoved` 时丢掉即可。`Output::id()` 是比较身份用的。
- **`OutputLayout::add_auto()` / `bounds()`**：从左到右排；所有输出的外框，相对指针就夹在里面。
  **`Cursor::position()`** 读回 `move_by()` 夹过之后指针在哪，不必在 Rust 里再算一遍。
- **`OutputLayout` 在显示器销毁时自动把它移除。** 这不是策略：C++ 的混成器在 `Output::destroy` 回调里同步移除，
  Rust 只能等到 `dispatch()` 返回才知道，而在那之前 `Cursor` 可能已经在用这个 layout。shim 自己挂 destroy
  信号，避免留下悬垂指针。`SceneOutput` 本来就会自行解绑。
- **`Output::take_mode_changed()`**：`set_mode()` 生效后为真一次，此时要 `reset()` 渲染状态、重新排版。
- **`Session::next_activity() -> Option<bool>`**：VT 切走 / 切回。切走时该把发给客户端的按下状态全部放开。

### 9.4 Vulkan 原始句柄（cargo feature `raw-vulkan`）

ADR 0003 约束的是**核心模块**收什么协议；Vulkan 本来就在 `luminaria.gpu` 里。C++ 侧给 `VulkanRenderer`
加了 `handles()`，返回的 `VulkanHandles` 全是 `void*` 和整数，导出接口仍然不带任何 Vulkan 头，没有越过
任何模块边界，所以可以做。

打开 `raw-vulkan` 后有 `VulkanRenderer::raw_handles()` 与 `SceneRenderer::raw_vulkan_handles()`（CPU 降级时为
`None`），得到 `RawVulkanHandles`：instance、physical device、device、queue（都是 `u64`，正好是 ash
`Handle::from_raw` 要的类型）、queue family、API 版本、已启用的设备扩展。用 ash 时：
`ash::Instance::load(entry.static_fn(), vk::Instance::from_raw(h.instance))`。

为什么要用 feature 打开：句柄一旦被外部使用，下面几件事就不再由本库保证，得由调用者负责：

- **同步。** `VkQueue` 要求调用方自己做外部同步。luminaria 在调用它的那个线程上提交，外部工作也只能在这个线程上、
  在两次 luminaria 调用之间提交。luminaria 的 timeline semaphore 和 in-flight 记账不知道外部提交的存在。
- **寿命。** renderer drop 之前，外部代码必须先等自己的工作完成（`vkQueueWaitIdle` 或自己的 fence），并销毁自己
  创建的对象。
- **特性。** 设备只启用了列出的扩展，外加 Vulkan 1.3 的 synchronization2 / dynamicRendering / timelineSemaphore。
  wgpu 这类需要更多特性的库不一定能在这个设备上建起来；luminaria 不会为外部调用者多开特性。
- **测试兜底失效。** "稳态 `render_to` 不分配"和"校验层零报错"这两条测试保证，覆盖不到外部代码提交的工作。

### 9.5 `examples/minimal.rs` 与 C++ 版的对齐情况

有了 9.2 和 9.3，`minimal.rs` 加上了与 `minimal.cpp` 相同的后端选择：`LUMINARIA_BACKEND=auto|nested|drm|headless`。
auto 依次尝试嵌套、能拿到 libseat 会话时的 DRM、无头；显卡取第一个参数或 `LUMINARIA_DRM_DEVICE`；
Ctrl+Alt+Fn 切 VT；多显示器按插入顺序从左到右排，窗口铺满第一块；显示器热插拔、模式变化、VT 切走时放开
按键、DRM 上每秒一行帧统计，都与 C++ 版一致。有了 §4 的事件，弹出菜单也一致了：`PopupTree` 把 popup 放进场景，
点在菜单外面关掉它，`set_popup_constraint_areas` 让贴边的菜单翻转；窗口按窗口几何摆放；`OutputGlobal`
告诉客户端位置、模式、缩放与朝向。C++ 版没有 layer shell 与锁屏，所以 Rust 版也没有（两者保持同步）；
它们的用法见 `tests/test_layer_shell.rs`、`tests/test_session_lock.rs`。

与 C++ 版曾有的差别现已全部补齐，两个示例在行为上已经一致：

- **输入只有一条路径**：两个示例都把嵌套和 libinput 当作同一个 `InputSource`，一个处理函数。之前各写两遍，
  Rust 的嵌套路径把滚轮的刻度和平滑量都发给客户端，每格滚两次；C++ 版与 DRM 路径只发刻度。现在这条规则在
  `Seat::pointer_axis_event` 里，两边都调它。按键的 keysym 也由后端给出，嵌套路径不再自己维护一份 `KeymapState`。

- **首选缓冲缩放与分数缩放**：窗口 map 时把输出的整数缩放与分数缩放发给客户端——`SurfaceId::set_preferred_buffer_scale(scale)`、
  `SurfaceId::set_preferred_buffer_transform(transform)` 对应 `wl_surface.preferred_buffer_scale` /
  `preferred_buffer_transform`（`wl_surface` v6 起），`FractionalScaleManager::set_scale(surface, scale_120ths)`
  对应 `wp_fractional_scale_v1.preferred_scale`（120 分之一；另有 `set_scale_f64(1.5)`）。输出缩放变化后要重新发。
- **子表面树**：`SurfaceId::surface_tree() -> Vec<SurfaceAt>` 按后到前的顺序给出这棵树（含子表面），
  每一项的 `x`/`y` 是它相对根表面的偏移。按住按钮、抓取落在子表面上时，用它算出该子表面在屏幕上的原点
  （场景项的位置 + 偏移），指针坐标才不会按整个窗口的原点算。

另有两条与「对象活得比输出久」相关的约定，热插拔时靠它们兜底：`Frame`（借用 `VulkanRenderer` 的那个）
在输出销毁时被 shim 直接丢弃，之后 `reset` / `submit` 返回错误、其余方法是空操作；`DirectScanout`
在 C++ 侧自己把 `Output*` 置空，所以输出拔掉之后 `available()` 为 false、`id_for()` 为 `None`。这两条由
`tests/test_escape_hatches.rs` 用一个被丢掉的 headless 后端守着。

现已补全的跨 crate 互操作与功能支持：

- **`LinuxDmabuf` 与 GPU 共享**：可通过 `LinuxDmabuf::from_scene_renderer(&display, &renderer)`（或 `renderer.create_linux_dmabuf(&display)`）直接从 `SceneRenderer` 创建；`examples/minimal.rs` 已接驳该协议，GPU 客户端可直接走零拷贝 dmabuf。
- **光标形状协议 (`cursor-shape`)**：通过 `Cursor::new_with_shapes(&renderer, &layout, &seat, &shapes)` 将 `CursorShapeManager` 接入光标引擎，自动呈现客户端请求的命名光标形状，已在 `examples/minimal.rs` 中完整启用。
- **图像数据输入 (`SceneItem::image`)**：`SceneRenderer::set_image(serial, width, height, rgba: &[u8])` 接受紧凑排布的原始 RGBA `&[u8]` 字节切片（`image` crate 的图像缓冲区可直接传入），与场景中的 `SceneItem::image(rect, serial)` 对应；GPU 路径按 serial 自动去重缓存与动态上传，CPU 路径自动回退合成。
- **显示器配置 (`OutputGlobal`)**：支持完整的 `position()`、`logical_position()`、`size()`、`logical_size()`、`scale()`、`transform()`、`refresh_mhz()`、`name()` 查询与设置，并提供 `with_properties()` 构造器。
- **分类错误类型 (`core::ErrorKind`)**：`core::Error` 现包含可模式匹配的 `ErrorKind`（`err.kind()`），涵盖 `Init`、`EventLoop`、`Backend`、`Gpu`、`Protocol`、`Shell`、`InvalidArgument`、`Other`；保留 `Display` 与 `std::error::Error` 实现，可无缝配合 `?` 操作符、`anyhow` 与 `thiserror`。
- **几何类型转换 (`mint` / `glam` 特性)**：提供可选 cargo 特性 `mint` 与 `glam`，为 `Rect`、`Color`、`Point` 提供 `From`/`Into` 实现，避免调用者受孤儿规则限制而编写 newtype 包装。
- **事件循环集成 (`calloop` 与 `tokio`)**：
  - **`calloop`**：`Display` 实现了 `AsFd`，可将其文件描述符通过 `calloop::generic::Generic` 注册至 calloop 事件循环（详见 `examples/calloop.rs`）。就绪时调用 `display.dispatch(Some(Duration::ZERO))` 排干底层事件与定时器，随后调用各协议包装对象的 `next_event()` 排干事件队列。
  - **`tokio`**：注意 luminaria 所有类型均为 `!Send`（所有 Wayland 对象、后端句柄与 C++ 状态均绑定至创建 `wl_display` 的单线程主循环）。因此在 tokio 中必须使用单线程运行时（`tokio::runtime::Builder::new_current_thread()`）或在 `tokio::task::LocalSet` 内运行；使用 `tokio::io::unix::AsyncFd::new(display.as_fd())` 监听可读就绪事件，随后执行非阻塞 `display.dispatch(Some(Duration::ZERO))` 并清空就绪标记（`guard.clear_ready()`）。

整个绑定现已完整转发全部信号（C++ 产生信号时将 `EventRaw` 入队，Rust 侧通过对应包装类型的 `next_event()` 取出）：

- **`Compositor`**：`CompositorEvent::NewSurface`，以及每个 surface 的 `Commit`、`Destroy`、`Rendered` 与全局的 `Invalidated`。
- **`Output`**：`OutputEvent::Present(PresentEvent)`（亦可通过 `output.next_present()` 获取，包含硬件时钟与单调时间戳、刷新周期与垂直同步标志）。
- **`WaylandBackend`**：作为 `InputSource` 转发 `Key`（带 keysym）、`MotionAbsolute`（带输出）、`PointerLeave`、`Button`、`Modifiers`、`Axis`（平滑滚动量、离散步进刻度与停止标记）、`KeymapChange` 与 `Capabilities`；关窗请求经 `take_close_request()`。
- **需同步回答的信号**（遵照 C++ 绝不调用 Rust 的规矩，策略预先配在 shim 上）：
  - xdg-activation 的 `ActivationTokenRequest`：默认批准（可通过 `set_grant_tokens(bool)` 预设策略），同时入队到 `next_event()`。
  - cursor-shape 的 `CursorShapeRequest`：通过 `Cursor::new_with_shapes(&renderer, &layout, &seat, &shapes)` 将 `CursorShapeManager` 接进 `Cursor`，在 C++ 信号发射期间直接由光标引擎同步呈现所请求的光标；`CursorShapeManager::next_event()` 亦可获取该请求。
- **桌面协议请求**：
  - `XdgDecorationManager`：`next_event() -> Option<DecorationRequest>`（携带表面、客户端偏好与最终决议），可由 `set_default_mode` 预先配置默认模式。
  - `ForeignToplevelManager`：`next_event() -> Option<ForeignToplevelRequest>`（最大化、最小化、全屏、激活、关闭等请求）。
  - `WorkspaceManager`：`next_event() -> Option<WorkspaceRequest>`（激活、反激活、创建、移除等工作区请求）。
  - `OutputManager`：`next_event() -> Option<OutputConfigRequest>`（测试与应用请求，可通过 `set_auto_verdict` 设置默认裁决）。
  - `XdgToplevelIconManager`：`next_event() -> Option<ToplevelIconChange>`，并提供 `icon_name(surface)` 查询客户端设定的图标名。

DRM 路径编译通过，嵌套 / headless 实际跑过；它在真实空闲 VT 上的运行还没有验证过。

---

## 10. `text` 模块（特性 `text`）

对应 `luminaria.text`：fcft 字形栅格化与字体度量。它是**第四个** C++ 模块，独立于 `luminaria`，
因为不画文字的混成器不该被迫链接 fontconfig——`Font` 只是把 `luminaria::Font` 原样包一层。
真正需要它的地方通常是画层表面（panel、通知、锁屏提示）的像素或生成一块 `SceneItem::image`。

```rust
use luminaria::text::Font;
use luminaria::util::Color;

let mut font = Font::open("monospace:size=12").expect("a monospace font");
let mut canvas = vec![0u8; 200 * 32 * 4];             // RGBA8，与 read_back/commit_frame 同布局
let pen = font.draw(&mut canvas, 200, 32, 4, font.ascent(), "hello",
                    Color::rgb(1.0, 1.0, 1.0), 200)?;  // 返回最后一个放得下的字形的笔位
```

- `Font::open(pattern) -> Option<Font>`：fontconfig 模式（`"monospace:size=11"`）；没有匹配的字体时为 `None`。
- `height()` / `ascent()` / `measure(&str) -> i32`：行高、基线偏移、字符串推进宽度。
- `draw(...)`：画进紧凑排布的 RGBA8 缓冲区；`right_edge` 之外的字符串被截断。缓冲区小于 `width * height * 4`
  时返回 `ErrorKind::InvalidArgument`，不会越界写。
- `decode_utf8(&str) -> Vec<char>`：C++ `luminaria::decode_utf8` 的转发；格式错误的字节变成 U+FFFD
  （`&str` 里本来也放不进非法字节）。
