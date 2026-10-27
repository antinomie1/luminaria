# Luminaria Rust API 文档

Luminaria 是写 Wayland 混成器的**库**，不是现成的混成器：它提供协议、后端与渲染，窗口放哪、谁拿键盘、按键做什么由使用者决定。

Rust 绑定与 C++ 库逐类型对应：像在 C++ 中一样，按需挑选与组合各个模块（`core`、`util`、`backend`、`protocol`、`gpu`、`desktop`、`text`、`shell`），构建无头测试环境、嵌套调试工具、专用嵌入式显示服务或自定义渲染管线的混成器。

## 特性开关：按模块付费

C++ 的模块边界（ADR 0008）在绑定里是一比一复制的：**每个模块一个 `#[cxx::bridge]`、一个 shim 翻译单元**，
cargo 特性决定编译哪些、链接哪些。关掉的模块不编译、不链接，也不探测它的系统库。

| 特性 | 默认 | C++ 模块 | Rust 模块 / 类型 | 额外链接的系统库 |
|---|---|---|---|---|
| *（始终有）* | — | `luminaria` | `core`、`util`、`backend::Output`、`protocol`、`shell::OutputLayout` | wayland-server、wayland-client、xkbcommon |
| `backend` | — | `luminaria.backend` | `backend`（`HeadlessBackend`/`WaylandBackend`/`DrmBackend`/`LibinputBackend`/`Session`） | libdrm、libinput、libudev、libseat |
| `render` | — | `luminaria.render` | `render`（`VulkanRenderer`/`CpuCompositor`/`LinuxDmabuf`/`Screencopy`） | vulkan、libdrm、gbm |
| `shell` / `gpu` | ✅ | `luminaria.shell` | 混成器整栈：`gpu` 门面（`Frame`/`SceneRenderer`/`Cursor`/`DirectScanout`）+ `AutoBackend` | 上述两行全部库 |
| `desktop` | ✅ | `luminaria.desktop` | `desktop`（任务栏、工作区、锁屏、输出管理、剪贴板、输入法） | — |
| `text` | ❌ | `luminaria.text` | `text` | fcft、fontconfig、pixman |
| `raw-vulkan` | ❌ | —（隐含 `render`） | `VulkanRenderer::raw_handles()`、`SceneRenderer::raw_vulkan_handles()` | — |
| `mint` / `glam` | ❌ | — | `util` 里几何类型的 `From`/`Into` | — |

```sh
cargo test --no-default-features                 # 只有核心：ldd 里没有 vulkan/gbm/libseat/libinput
cargo test --no-default-features --features backend # 只有设备与后端
cargo test --no-default-features --features render  # 只有渲染器
cargo test                                       # 默认 = gpu + desktop，整套流水线
cargo test --features text                       # 加上 Font
```

`desktop` 与底层渲染互不依赖。`raw-vulkan` 依赖 `render`：没有渲染器就没有句柄。

---

## 模块总览

| 模块 | 对应 C++ 命名空间与模块 | 核心职责 |
|---|---|---|
| **[`core`](#1-core-模块)** | `luminaria:display`, `event_loop`, `expected`, `input_router` | Wayland 服务端根对象、事件循环驱动、文件描述符绑定、错误类型、`KeyRouter` |
| **[`event`](#事件一条流)** | —（绑定自己的） | `Event`：库报告的一切，按信号触发顺序 |
| **[`util`](#2-util-模块)** | `luminaria:box`, `color`, `transform`, `region`, `keymap`, `blur` | 纯几何矩形运算、颜色与色彩空间、朝向变换、不相交区域、XKB 键表与模糊参数 |
| **[`backend`](#3-backend-模块)** | `luminaria.backend:*`（基础 Output 在 `luminaria`） | 虚拟无头输出、嵌套窗口后端；特性 `backend` 下还有裸机 DRM/KMS 输出、libinput 设备驱动、libseat 会话；特性 `shell` 下有 `AutoBackend` |
| **[`protocol`](#4-protocol-模块)** | `luminaria:compositor`, `xdg_shell`, `seat`, `layer_shell` 等 | Wayland 核心协议对象（Compositor, SurfaceId, Seat, LayerShell 等）及核心扩展管理器 |
| **[`render`](#5-render-模块特性-render)** | `luminaria.render:*` | Vulkan 1.3 渲染器、纯 CPU 软件合成器 `CpuCompositor`、`LinuxDmabuf`、显式同步 `DrmSyncobj`、截屏协议 |
| **[`shell`](#7-shell-模块)** | `luminaria.shell:*` | 即时模式场景项定义、反向命中测试、多显示器坐标排版、PopupTree、LayerManager |
| **[`gpu`](#5-gpu-模块)** | `luminaria.shell` + `luminaria.render` | 混成器整栈门面（re-export 了 `render` 全部图元，以及即时帧账本 `Frame`、硬件光标 `Cursor`、`SceneRenderer`、直出评估 `DirectScanout`） |
| **[`desktop`](#6-desktop-模块)** | `luminaria.desktop:*` | 任务栏同步、ext-workspace 工作区、ext-session-lock 锁屏、wlr-output-management 显示管理 |
| **[`text`](#10-text-模块特性-text)** | `luminaria.text` | fcft 字形栅格化与字体度量 |

---

## 借用关系：析构顺序由编译器检查

C++ 对象互相持有引用：`lm::Cursor` 存着 `SceneRenderer&`、`OutputLayout&`、`Seat&`，每个 global 都挂在
`wl_display` 上。绑定把这些关系写成 Rust 的生命周期参数，被引用的一方先 drop 就编译不过：

| 类型 | 借用的对象 |
|---|---|
| 所有协议 global（`Compositor<'d>`、`Seat<'d>`、`XdgShell<'d>`、`Presentation<'d>`……）、`HeadlessBackend<'d>`、`WaylandBackend<'d>`、`AutoBackend<'d>`、`Session<'d>`、`EventLoop<'d>` | `Display` |
| `AutoBackend::session()` 借出的 `Session<'_>` | 那个 `AutoBackend`（会话归它所有） |
| `DrmBackend<'a>`、`LibinputBackend<'a>` | `Display`，以及传入的 `Session` |
| `CoreGlobals<'d>` | `Display`（它装着的两个选区管理器借的 `Seat` 归它自己，所以作为一个值传来传去） |
| `DataDeviceManager<'a>`、`PrimarySelectionManager<'a>`、`RelativePointerManager<'a>`、`PointerConstraints<'a>`、`TextInputManager<'a>` | `Display`、`Seat` |
| `DataControlManager<'a>` | `Display`、`DataDeviceManager` |
| `InputMethodManager<'a>` | `Display`、`Seat`、`TextInputManager` |
| `OutputManager<'a>` | `Display`、`OutputLayout` |
| `ForeignToplevelManager<'d>` | `Display`；`track(&shell)` 之后还有那个 `XdgShell` |
| `LinuxDmabuf<'a>` | `Display`，以及传入的 `VulkanRenderer` 或 `SceneRenderer` |
| `Cursor<'a>` | `SceneRenderer`、`OutputLayout`、`Seat` |
| `Frame<'r>` | `VulkanRenderer` |
| `Toplevel<'s>`、`Popup<'s>`、`LayerSurface<'s>`、`LockSurface<'m>`、`SessionLock<'m>`、`PointerConstraint<'m>`、`TextInput<'m>`、`InputMethod<'m>` | 发出它的那个 global（只是查询句柄，见 §4"事件"） |

几点说明：

- **借用是共享借用。** 被借的类型——`Seat`、`XdgShell`、`OutputLayout`、`SceneRenderer`、`Session`——方法都取
  `&self`，借出之后照常使用。这是安全的：包装类型都不是 `Send`/`Sync`，C++ 从不回调 Rust，也不把内部引用交给 Rust。
- **只写 lifetime 不够，还要 `Drop`。** 没有 `Drop` 的结构体，dropck 允许它在借用结束后才被隐式析构，
  `drop(seat)` 放在 `Cursor` 之前照样能编译——而 `Cursor` 的析构恰好要碰那个 `Seat`。所以每个带 lifetime 的包装都
  带一个有 `Drop` 的标记字段。`Cursor` 的文档里有一个 `compile_fail` doctest 守着这一点。
- **`SceneRenderer` 不借任何东西**，每块输出的渲染状态也由它自己按输出保存（输出销毁时丢掉），所以用户代码里
  没有借它的每输出对象；只要在借它的 `Cursor`、`LinuxDmabuf` 之前声明即可。`set_presentation(Some(&p))` 没有借用：renderer 一般比 `Presentation` 活得久，这条链接由 shim 从两头维护，
  哪边先死哪边负责断开，之后反馈只是不再发送。
- **`Output` 不进生命周期。** 真正的 `lm::Output` 归后端所有，显示器随时可能被拔掉，这不是 Rust 的作用域能表达的。
  所以和输出有关的都在运行时兜底：`Output` 自己变成空操作（`is_alive()` 为 false）；`OutputLayout` 与渲染器的
  每输出状态自动解除绑定，跟随它的 `OutputGlobal` 停止跟随；`DirectScanout` 不再调用它；`Frame` 随输出一起销毁，之后 `reset` / `submit` 返回错误、其余方法为空操作。
- **结构体不能同时装被借者和借用者**（自引用）。把被借的放在外层——`main` 的局部变量，或一个只装它们的结构体——
  再让借用者借用它们。`examples/minimal.rs` 一个带 lifetime 的结构体都没写：每块输出和跟随它的 wl_output 放在
  `Vec<(Output, Option<OutputGlobal>)>` 里，类型推断替它写了 lifetime。
  会话、显卡、libinput 这一组本来就是这种关系（后两者借前者），所以由 `AutoBackend` 在 C++ 一侧一起持有，
  Rust 只借 `Display`。

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

// 5. 一轮：把上一轮产出的发给客户端，等事件循环（None 为阻塞等待，
//    Some(ZERO) 为立即非阻塞轮询），派发，并返回这一轮发生的一切
for event in display.dispatch(Some(Duration::ZERO))? {
    // luminaria::Event
}

// 6. 现在就把待发送的数据刷给客户端，而不是等下一轮
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
// 当可读时调用非阻塞派发，并处理返回的事件：
for event in display.dispatch(Some(std::time::Duration::ZERO))? { /* ... */ }
display.flush_clients();
```
自己派发事件循环（不经 `dispatch()`）的调用者用 `display.events()` 取走排队的事件，不等待也不派发。

### `KeyRouter`
在混成器快捷键与焦点客户端之间分配键盘，每个键保持完整：
```rust
keys.route(&seat, &input, |e| {
    let &InputEvent::Key { keysym, modifiers, .. } = e else { return false };
    modifiers.held() == Modifiers::LOGO && keysym.to_lower() == Keysym::q // true：这个键归混成器
});
```
按下先交给闭包；答 `true` 的键连同它的松开一起留下，其余交给 `seat`。松开丢失（切 VT）后的再次按下按新按下处理。
不是按键的事件直接忽略。

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
- `output.scanout_modifiers(DrmFourcc::Xrgb8888)`：查询硬件扫描线支持的 DRM 格式修饰符列表。
- 输出上发生的事以 `Event::Output(id, OutputEvent)` 到来：`Frame`（`schedule_frame()` 要的那一帧到了，现在画）、
  `Present`（一帧上了屏）、`ModeChanged`（`set_mode()` 落地；渲染器、布局与跟随的 `OutputGlobal` 已经跟上）。
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
- **`DrmBackend`** / **`LibinputBackend`** / **`Session`**：裸机驱动栈（须在独立空闲 VT 环境下运行）。显示器以 `Event::OutputAdded` / `Event::OutputRemoved` 到来与离开，输入以 `Event::Input`，VT 切换以 `Event::SessionActive`；`Session::switch_vt_for(keysym)` 是 Ctrl+Alt+Fn 那个绑定。见[第 9 节](#9-逃生口和别的-rust-crate-共用底层对象)。
- **`AutoBackend`**（特性 `gpu`，C++ `AutoBackend`）：替调用者挑后端。auto 依次试嵌套、能拿到 libseat 会话时的
  DRM（ssh、CI 因此不会误上 VT）、无头；按名字要某一种时失败就是错误而不回退，DRM 这时没有会话也照跑。
  会话、显卡、libinput 都归它，Rust 只借 `Display`。读环境变量仍是调用者的事，`"auto".parse::<BackendKind>()`
  负责解析那四个名字。
  ```rust
  let options = BackendOptions { kind: std::env::var("LUMINARIA_BACKEND")?.parse()?, ..Default::default() };
  let mut backend = AutoBackend::new(&display, &options)?;      // 宽高、窗口标题、显卡路径都在 options 里
  let renderer = SceneRenderer::new(&backend.vulkan_options())?; // DRM 上是屏幕背后那块 GPU
  if let Some(source) = backend.input() { seat.follow(source, false); } // 嵌套或 libinput；无头为 None
  backend.start()?;                                             // 先 libinput，后后端
  for event in display.dispatch(None)? {
      match event {
          Event::OutputAdded(output) => { /* ... */ }
          Event::OutputRemoved(id) => { /* 布局和渲染器已经放手 */ }
          Event::HostCloseRequest => { /* 只有嵌套会有 */ }
          _ => {}
      }
  }
  if let Some(session) = backend.session() { session.switch_to(2)?; } // 只在 DRM 上
  ```
  嵌套与无头那一块输出也以 `Event::OutputAdded` 到来（`start()` 时），所以三种后端只有一条输出路径。

### 输入：`InputSource`
嵌套的 `WaylandBackend`、裸机的 `LibinputBackend` 与 `AutoBackend` 都实现（封闭的）`InputSource` trait，输入以
`Event::Input(InputEvent)` 到来。两者的差别在于到来的是哪些分支（相对移动 `Motion` 与触摸来自 libinput，绝对移动
`MotionAbsolute` 与 `PointerLeave` 来自嵌套窗口），而不在于怎么到来，所以混成器只写一个 match：

```rust
if let Some(source) = backend.input() {
    seat.follow(source, false); // keymap 与 capabilities 归 seat 跟随；不路由触摸就不公布 touch
}
for event in display.dispatch(None)? {
    let Event::Input(input) = event else { continue };
    match input {
        InputEvent::Key { .. } => keys.route(&seat, &input, |e| /* 快捷键？ */ false),
        InputEvent::Modifiers(m) => seat.notify_modifiers(m),
        InputEvent::Axis(axis) => seat.pointer_axis_event(axis),
        // Motion / MotionAbsolute / PointerLeave / Button / Capabilities / Touch*
        _ => {}
    }
}
```

- `InputEvent::Key` 的 `keysym`（`util::Keysym`，每个 `XKB_KEY_*` 一个常量：`Keysym::Return`、`Keysym::q`）与
  `modifiers`（`Modifiers::held()` 是快捷键要比的那一部分，`Modifiers::LOGO` 等常量）是后端按 `keymap()` 读出、取**这个键之前**的状态：先按 Logo
  再按 Return，到来的是带着 Logo 的 Return。快捷键直接对着它匹配，不需要自己的 `KeymapState`。读数在 C++ 信号里
  就拍下，所以同一批后面的按键改掉 xkb 状态也不影响它。
- `MotionAbsolute { output }` 是 `OutputId`，指明坐标属于哪块输出。
- `Seat::pointer_axis_event(PointerAxis)`：滚轮同时报刻度和平滑量，刻度优先；分别调 `pointer_axis` 与
  `pointer_axis_discrete` 会让每格滚两次。
- `keymap()` / `capabilities()` 给出启动时已有的状态，之后的变化以 `KeymapChange` / `Capabilities` 到来；
  `Seat::follow()` 替你跟随这两样。
- 父级要求关窗不是输入：`Event::HostCloseRequest`。

---

## 4. `protocol` 模块

负责向客户端通告和处理 Wayland 协议规范。

### 事件：一条流

混成器是被 C++ 的 `Signal` 驱动的。绑定不让 C++ 回调 Rust：每个 shim 把信号按触发的顺序推进**同一条**
（每线程一条）队列，`Display::dispatch()` 返回时把它整条交回，成为 `luminaria::Event`：

```rust
for event in display.dispatch(Some(Duration::from_millis(16)))? {
    match event {
        Event::Toplevel(surface, e) => { /* ... */ }
        Event::Layer(surface, e) => { /* LayerSurfaceEvent */ }
        Event::Seat(e) => { /* SeatEvent */ }
        Event::Output(id, OutputEvent::Frame) => { /* 画这一块 */ }
        _ => {}
    }
}
```

- **没有要记得取空的队列。** 以前每个 global 一条队列，用了它却忘了每轮取空就一直长；现在只有一条，
  `dispatch()` 每次都整条交回，不 match 的事件就丢掉。
- **顺序就是信号的顺序。** 不同 global 的事件按发生先后交错，而不是按 global 分组。
- **每个载荷都能 `TryFrom<Event>` 取回**（不是它时原样还回事件），例如
  `events.filter_map(|e| XdgEvent::try_from(e).ok())`；`(OutputId, OutputEvent)` 与 `(SurfaceId, LayerSurfaceEvent)` 也一样。
  输出和 xdg 窗口的事件是平的（`Event::OutputAdded` / `OutputRemoved` / `Toplevel` / `Popup`），一个 match 分支就把整件事说完；
  `BackendEvent` 与 `XdgEvent` 是把各自这一族收在一起的类型，给 `TryFrom` 用。
- **混成器自己的调用触发的信号**（`seat.set_keyboard_focus()` 发出的焦点事件之类）同样进队列，在下一次
  `dispatch()` 或 `display.events()` 时出现，而不是当场返回。
- **一个线程一个 `Display`。** 队列按线程分，同一线程上的第二个 `Display` 会收到第一个的事件。
- **事件晚于信号到达。** 信号在 `dispatch` 里发出，Rust 看到时对象可能已经又变了、甚至没了。所以客户端创建的对象
  不以引用交给 Rust，而是以名字：有自己 surface 的（toplevel、popup、layer surface、锁屏 surface、指针约束、
  输入法弹窗）用那个 surface 的 `SurfaceId`，其余（锁、idle 通知与抑制器、text input、输入法）用绑定分配、
  永不复用的 id。`shell.toplevel(id)` 之类返回的句柄每次调用都重新查找；对象销毁后 getter 返回默认值、setter 不做事。
- **替 Rust 立刻做的事，由 C++ 自己做。** 对象销毁时必须马上放开它的地方——`PopupTree`、`LayerManager`、
  `OutputLayout`、`SceneRenderer` 的每输出状态——都在 C++ 里监听销毁信号，不等 Rust 取到事件。
- **要同步回答的信号无法转发。** `XdgShell` 的 popup 约束查询改成事先告知（`set_popup_constraint_areas`）；
  xdg-activation 的发 token 请求总是批准。
- **maximize / fullscreen / minimize 默认由库回答。** luminaria 对这三个请求有一个"没人监听就自己批准"的兜底，
  和不监听它们的 C++ 混成器一样生效；要自己回答，`XdgShell::new(&display)?.forward_window_requests()`，
  之后它们以 `ToplevelEvent::RequestMaximize` 等到来。

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

`Event::Seat` 报告路由的变化，无论谁引起：`SeatEvent::KeyboardFocus(Option<SurfaceId>)`、
`PointerFocus(Option<SurfaceId>)`、`CursorChange { surface, hotspot, hidden }`（指针焦点所在的客户端设置了光标；
指针离开时光标回到混成器自己的，报为 `surface: None, hidden: false`）。

- `seat.follow(&source, touch)`：跟随输入源（`backend.input()`）的 keymap 与 capabilities，含启动前已有的；
  touch 只在 `touch` 为 true 时公布。
- `seat.release_keys()`：松开客户端以为按着的每个键并清修饰键（切 VT 时）。

### `XdgShell`
发布 `xdg_wm_base`（toplevel 与 popup）。事件是 `Event::Toplevel(SurfaceId, ToplevelEvent)` 或
`Event::Popup(SurfaceId, PopupEvent)`：

- `ToplevelEvent`：`Created`、`Map`、`Unmap`、`Destroy`；客户端的请求 `RequestMaximize(bool)`、`RequestFullscreen(bool)`、
  `RequestMinimize`（这三个只在 `forward_window_requests()` 之后到来，否则库自己批准）、`RequestMove { serial }`、`RequestResize { serial, edges }`（`util::edge` 位掩码）、
  `RequestWindowMenu { serial, x, y }`；`IdentityChange`（标题 / app id）、`StateChange`（混成器的 setter 真的改了状态）、
  `ParentChange(Option<SurfaceId>)`。
- `PopupEvent`：`Created { parent }`（layer surface 的 popup 为 `None`）、`Map`、`Unmap`、`Destroy`、`Reposition`、`Grab { serial }`。

```rust
let shell = XdgShell::new(&display)?.forward_window_requests(); // 全屏请求归我们答
shell.set_bounds((1920, 1080));
let popups = PopupTree::new(&shell)?; // 从此自己跟随每个 popup
for event in display.dispatch(None)? {
    match event {
        Event::Toplevel(s, ToplevelEvent::Created) => {
            let t = shell.toplevel(s).unwrap();
            t.set_maximized(true);
            t.configure((1920, 1080));
        }
        Event::Toplevel(s, ToplevelEvent::RequestFullscreen(on)) => {
            if let Some(t) = shell.toplevel(s) { t.set_fullscreen(on) }
        }
        Event::Popup(..) => { /* 要一帧；树已经知道了 */ }
        _ => {}
    }
}
```

- `Toplevel`：`title()`、`app_id()`、`parent()`、`is_mapped()`、`min_size()`、`max_size()`、`geometry()`（窗口几何，
  不含客户端阴影）、`pending_size()`、`states()`；`configure(size) -> serial`（`size` 是 `(w, h)` 或 `rect.size()`）、`set_maximized` / `set_fullscreen` /
  `set_activated` / `set_resizing` / `set_tiled` / `set_minimized`、`close()`。`shell.toplevels()` 列出全部。
- `Popup`：`parent()`、`is_mapped()`、`has_grab()`、`geometry()`（相对父窗口几何）、`dismiss()`。
- `set_popup_constraint_areas(&[PopupArea { parent, parent_box, usable }])`：每个可能当 popup 父亲的 surface 画在哪、
  在哪块屏上。shell 摆 popup 时在 `dispatch` 里同步查询，只能用事先告诉它的；场景一变就更新。不在表里的父亲不受约束。

### `LayerShell`
发布 `zwlr_layer_shell_v1`（面板、壁纸、通知、锁屏界面）。事件是 `Event::Layer(SurfaceId, LayerSurfaceEvent)`：
`Created`、`Map`、`Unmap`、`Destroy`、`StateChange`（提交了新的层 / 锚点 / 尺寸 / 边距 / 独占区，重新排版）、
`NewPopup(SurfaceId)`（客户端把这个没有 xdg 父亲的 popup 挂到本 layer surface 上，这是唯一知道它归属的地方）。

`layers.layer_surface(id)` 的句柄：`scope()`、`layer()`、`anchor()`（`util::edge` 位掩码）、`desired_size()`、
`exclusive_zone()`、`exclusive_edge()`、`margins()`、`keyboard_interactivity()`、`is_mapped()`、`pending_size()`、
`has_output()` / `is_on(&output_global)`（客户端点名的输出）、`configure(w, h)`、`close()`、
`arrange(full, &mut usable) -> Rect`。整块输出一起排版用 `shell::LayerManager`（§7）。

### 其余协议的事件

| global | 事件（`Event::…`） | 查询与回答 |
|---|---|---|
| `IdleInhibitManager` | `IdleInhibit(IdleInhibitEvent)`：`IdleInhibitEvent::{NewInhibitor { inhibitor, surface }, InhibitorDestroy, Changed(bool)}` | `is_inhibited()`、`inhibitor_count()`、`set_visible(id, bool)`（只有可见的抑制器生效） |
| `IdleNotifier` | `IdleNotifier(IdleNotifierEvent)`：`IdleNotifierEvent::{NewNotification, NotificationDestroy, StateChange { notification, idled }}` | `notify_activity()`、`set_inhibited()`、`notification(id)` |
| `XdgActivation` | `Activation(ActivationEvent)`：`ActivationRequest { surface, token_valid, app_id, requesting_surface, serial }` | 给不给焦点由混成器决定 |
| `PointerConstraints` | `PointerConstraint(…)`：`PointerConstraintEvent::{New, Destroy, RegionChange}(SurfaceId)` | `constraint(surface)`：`kind()`、`region()`、`cursor_hint()`、`activate()`（只在指针焦点在该 surface 时生效）、`deactivate()`；`active_constraint()` |
| `TextInputManager` | `TextInput(…)`：`TextInputEvent::{New, Enable, Disable, Commit, Destroy}(TextInputId)` | `focused()`、`text_input(id)`：`state()`、`surrounding_text()`、`send_preedit_string` / `send_commit_string` / `send_delete_surrounding_text` / `send_done` |
| `DataDeviceManager`、`PrimarySelectionManager` | `Selection(SelectionChange { primary, mime_types })`（新所有者提供的 MIME 类型，空表示清空） | `selection_mime_types()`、`selection_receive(mime, OwnedFd)`（写进管道写端，dispatch 后读另一端） |

### `CoreGlobals`
每个可用混成器都要的那一组，一次建好（C++ `CoreGlobals` 的对应物）：wl_shm、wl_compositor、wl_subcompositor、
xdg-shell、wl_seat、剪贴板与 primary selection、single-pixel-buffer、presentation-time、viewporter、fractional-scale、
cursor-shape、xdg-decoration。两个选区管理器借的 seat 归它自己，所以它是一个可以移动、可以放进结构体的值：
```rust
let globals = CoreGlobals::new(&display)?;          // 或 with_seat_name(&display, "seat1")
let (shell, seat) = (globals.shell(), globals.seat());
globals.fractional_scale().set_scale(surface, 1.5); // 精确值 + 向上取整的整数提示
```
`forward_window_requests()` 同 `XdgShell` 的。linux-dmabuf（要渲染器）与其余协议照下面单独建。

### `OutputGlobal`
`OutputGlobal::for_output(&display, &output, &layout, name)` 建一个跟随输出与布局的 wl_output：mode 变化重发 mode、
scale、transform，布局变化重发位置，输出销毁后停止跟随；空名字取 `luminaria-<n>`。`OutputGlobal::new(&display, w, h, name)`
加 setter 留给不来自后端的输出。查询：`logical_position()`、`logical_size()`、`size()`、`scale()`、`transform()`、
`refresh_mhz()`、`name()`。

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
let _gestures        = PointerGestures::new(&display)?;

// 依赖 Seat 的席位协议
let _data_device     = DataDeviceManager::new(&display, &seat)?;
let _primary_sel     = PrimarySelectionManager::new(&display, &seat)?;
let _relative_ptr    = RelativePointerManager::new(&display, &seat)?;
let _ptr_constraints = PointerConstraints::new(&display, &seat)?;
let _text_input      = TextInputManager::new(&display, &seat)?;
```

- **`PointerGestures`**：`zwp_pointer_gestures_v1` (v3) 触摸板手势支持。向客户端分发手势：
  - `send_swipe_begin(surface, serial, time_ms, fingers)` / `send_swipe_update(time_ms, dx, dy)` / `send_swipe_end(serial, time_ms, cancelled)`
  - `send_pinch_begin(surface, serial, time_ms, fingers)` / `send_pinch_update(time_ms, dx, dy, scale, rotation_deg)` / `send_pinch_end(serial, time_ms, cancelled)`
  - `send_hold_begin(surface, serial, time_ms, fingers)` / `send_hold_end(serial, time_ms, cancelled)`

---

## 5. `gpu` 模块

对应 `luminaria.gpu` 扩展，包含基于 Vulkan 1.3 的高性能图形管线与裸机加速支持。

### `VulkanRenderer`
- `VulkanRenderer::new(&VulkanOptions::default())`：初始化 Vulkan 1.3 上下文（要求 Dynamic Rendering、Timeline Semaphore 与 dmabuf 扩展支持）。裸机上传 `VulkanOptions { drm_device: DrmDeviceId::from_fd(&drm) }`，让渲染器就是显示所在的那块 GPU。
- `vulkan.scanout_modifiers(DrmFourcc::Xrgb8888)`：读取显卡支持的内存布局 modifier（`util::DrmFourcc`，来自 `drm-fourcc` crate）。

### `SceneRenderer`
全自动具备 GPU 混合及 CPU 降级能力的场景呈现引擎。每块输出的渲染状态由它自己按输出保存，输出销毁时丢掉：
```rust
use luminaria::gpu::{SceneRenderer, VulkanOptions, SceneOutcome};
use luminaria::util::Color;

let renderer = SceneRenderer::new(&VulkanOptions::default())?;

// 每块输出一次（默认 XRGB8888；reset_with_format(&mut output, DrmFourcc::Argb8888) 换格式），
// 之后的 mode 变化它自己重新分配
renderer.reset(&mut output)?;

// 在 OutputEvent::Frame 上提交场景：present 用输出坐标，present_in 用布局坐标
let outcome = renderer.present_in(&layout, &mut output, &scene_items, Color::rgb(0.0, 0.0, 0.0))?;
match outcome {
    SceneOutcome::Composited => { /* GPU 成功合成 */ }
    SceneOutcome::Scanout => { /* 客户端全屏零拷贝直出 */ }
    SceneOutcome::Unchanged => { /* 画面与上一帧完全相同，跳过重绘 */ }
    SceneOutcome::Cpu => { /* 降级至 CPU 软件混合 */ }
}
```
`gpu_output(&output)` 说这块输出现在走不走 GPU，`read_back(&output)` 读回上一帧。

### `Cursor`
指针及其图像：主题、cursor-shape、客户端精灵、KMS 光标平面。`Cursor::new(&renderer, &layout, &seat)?`
借用这三者直到它 drop（见上文"借用关系"）。`move_to(x, y)` / `move_by(dx, dy)`（夹在整个布局里；第一块输出出现时指针自己站在它中间）、`move_to_within` / `move_by_within`（要自己给边界，指针约束那种）、`leave` / `position` / `refresh`。

### `Frame`
逐输出的即时模式 GPU 帧账本。如果需要手动绕过 `SceneRenderer` 直接驱动 Vulkan 帧绘制，可使用 `Frame`。
它借用 `VulkanRenderer`；输出被销毁时它随之失效（`reset` / `submit` 返回错误）：
- `Frame::new(&mut output, &vulkan)?`
- `frame.begin(viewport)`
- `frame.place_rect(rect, color)`
- `frame.reset(DrmFourcc::Xrgb8888)`
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
- **`SessionLockManager`**：`ext-session-lock` 锁屏协议。事件 `Event::SessionLock(SessionLockEvent)`：`NewLock(LockId)`、
  `NewSurface { lock, surface }`、`SurfaceMap`、`SurfaceDestroy`、`LockDestroy { lock, unlocked }`。盖住所有输出后
  `locks.lock(id).send_locked()`，拒绝用 `send_finished()`；`lock_surface(surface)` 的句柄 `is_on(&output_global)`、
  `configure(w, h)`。它**失败即锁定**：锁屏客户端没解锁就死了，`unlocked` 为 false，`is_locked()` 仍为 true，
  绘制与输入应跟随 `is_locked()` 而不是锁屏客户端是否还在。
- **`OutputManager`**：为 `kanshi`、`wlr-randr` 等外部多屏配置工具提供查询与动态布局修改接口。
- **`DataControlManager`**：为 `wl-clipboard` 等无窗口剪贴板工具提供特权读取与写入接口。
- **`InputMethodManager`**：为 Fcitx / IBus 等输入法引擎提供候选词窗口绑定与键盘抢占支持；与 `TextInputManager`
  之间的桥接它自己完成。事件 `Event::InputMethod(InputMethodEvent)`：`New`、`GrabChange { input_method, grabbed }`、`Destroy`、
  `NewPopupSurface { input_method, surface }`、`PopupSurfaceDestroy`。抢占键盘期间按键经 `input_method(id).grab_send_key(...)`
  送给输入法，候选窗口放在 `popup_text_input_rectangle(surface)` 旁边。
- **`GammaControlManager`**：`zwlr_gamma_control_manager_v1`。输出伽马曲线查找表管理（夜间模式等）。`gamma_size()` 返回查找表条目数，`ramp(output_res)` 查询当前设定的 16 位 RGB 曲线。事件 `Event::Gamma(GammaEvent { output_resource, has_ramp })`。
- **`CtmControlManager`**：`hyprland_ctm_control_manager_v1` (v2)。输出色彩变换矩阵（CTM）控制。`matrix(output_res)` 查询当前提交的 3×3 行优先变换矩阵。事件 `Event::Ctm(CtmEvent { output_resource })`。
- **`VirtualKeyboardManager`**：`zwp_virtual_keyboard_manager_v1`。接收客户端虚拟键盘按键与修饰键注入。事件 `Event::VirtualKey(VirtualKeyEvent)`、`Event::VirtualModifiers(VirtualModifiersEvent)`。
- **`VirtualPointerManager`**：`zwlr_virtual_pointer_manager_v1` (v2)。接收客户端虚拟指针移动、按键与轴滚轮注入。事件 `Event::VirtualPointer(VirtualPointerEvent)`（`Motion`、`MotionAbsolute`、`Button`、`Axis`、`Frame`）。

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
`PopupTree::new(&shell)` 从此自己跟随 shell 的每个 popup（创建、映射、取消映射、重定位、销毁），混成器只画它；
layer surface 的 popup 用 `set_parent(&shell, popup, layer_surface)` 指定父亲。`append_to(&mut scene)`（每个 popup 接在父 surface 之上、按偏移摆放、带父窗口的 tag，子菜单嵌套）、
`contains`、`active_grab`、`dismiss_grabs_outside(seat.pointer_focus())`（点在所有 popup 外面就关掉抢占的那些，返回 true
时这次点击不该交给客户端）。不借用 `XdgShell`：shell 先死，它的连接自动失效。

### `scene_route_pointer` 与 `scene_surface_origin`
`scene_route_pointer(&seat, &items, x, y)` 是指针路由：按住按钮（隐式 grab）期间每次运动都按 grab 表面自己的坐标
发过去，否则把焦点给命中的表面、或在空白处清掉焦点；返回命中结果。`scene_surface_origin(&items, surface)` 是它用的
那一半：某个表面（子表面也算）的原点画在哪。`SceneItem::kind` 是 `SceneKind::{Surface, Rect, Border, Image}`。

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
use luminaria::protocol::{CoreGlobals, OutputGlobal};
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

    // 3. 注册每个混成器都要的协议接口
    let globals = CoreGlobals::new(&display)?;
    globals.shell().set_bounds((1024, 768));
    globals.seat().set_capabilities(true, true, false);

    // 4. 多屏坐标排版
    let layout = OutputLayout::new()?;
    layout.add(&mut output, 0, 0);

    // 5. 向客户端发布跟随这块输出的 wl_output
    let _out_global = OutputGlobal::for_output(&display, &output, &layout, "HEADLESS-1")?;

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

    // 7. 单步驱动事件循环，处理这一轮的事件
    for event in display.dispatch(Some(Duration::ZERO))? {
        println!("{event:?}");
    }

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
补法与嵌套后端一样：shim 把信号推进事件队列，`dispatch()` 返回时交给 Rust（§4「事件」）。

- **`LibinputBackend` 是一个 `InputSource`**（见 §3「输入」）：按键、修饰键、相对移动、按钮、滚轮
  （平滑量 + 刻度 + 停止标记）、触摸（down/motion/up/frame/cancel，坐标归一化到触摸屏）、设备能力变化。
  另有 `set_keymap()`，以及对应 `create_path()` 的 `LibinputBackend::new_path()`。
- **`Event::OutputAdded(Output)` 与 `Event::OutputRemoved(OutputId)`**，DRM 与 `AutoBackend` 都是它们（`BackendEvent` 把两者收成一族，给 `TryFrom` 用）。
  `start()` 时已有的显示器也以 `OutputAdded` 到来。显示器被拔掉后，手里的 `Output` 仍可安全调用，只是一切都成了
  空操作（`Output::is_alive()` 为 false），收到 `OutputRemoved` 时丢掉即可。`Output::id()` 是比较身份用的。
- **`OutputLayout::add_auto()` / `bounds()`**：从左到右排；所有输出的外框，相对指针就夹在里面。
  **`Cursor::position()`** 读回 `move_by()` 夹过之后指针在哪，不必在 Rust 里再算一遍。
- **`OutputLayout` 在显示器销毁时自动把它移除**，换 mode 时保留位置、换上新尺寸——现在 C++ 的 `OutputLayout`
  自己就这么做，渲染器的每输出状态同样自行解绑。
- **`OutputEvent::ModeChanged`**：`set_mode()` 生效。渲染器、布局与跟随的 `OutputGlobal` 已经跟上，剩下的只有
  混成器自己按旧尺寸摆的东西（窗口大小）。
- **`Event::SessionActive(bool)`**：VT 切走 / 切回。切走时 `seat.release_keys()`；`session.switch_vt_for(keysym)` 是
  Ctrl+Alt+Fn 那个绑定。

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

`minimal.rs` 与 `minimal.cpp` 的后端选择相同：`LUMINARIA_BACKEND=auto|nested|drm|headless`。
auto 依次尝试嵌套、能拿到 libseat 会话时的 DRM、无头；显卡取第一个参数或 `LUMINARIA_DRM_DEVICE`；
Ctrl+Alt+Fn 切 VT；多显示器按插入顺序从左到右排，窗口铺满第一块；显示器热插拔、模式变化、VT 切走时放开
按键、都与 C++ 版一致；DRM 上每秒一行的帧统计和其它诊断行只在 C++ 版里（`LUMINARIA_TRACE=1`，`scripts/tty-check.sh` 读的就是它），Rust 版不带。有了 §4 的事件，弹出菜单也一致了：`PopupTree` 把 popup 放进场景，
点在菜单外面关掉它，`set_popup_constraint_areas` 让贴边的菜单翻转；窗口按窗口几何摆放；`OutputGlobal`
告诉客户端位置、模式、缩放与朝向。C++ 版没有 layer shell 与锁屏，所以 Rust 版也没有（两者保持同步）；
它们的用法见 `tests/test_layer_shell.rs`、`tests/test_session_lock.rs`。

与 C++ 版曾有的差别现已全部补齐，两个示例在行为上已经一致，并且建在同一组库机制上：`CoreGlobals`、
跟随输出与布局的 `OutputGlobal`、跟随 shell 的 `PopupTree`、`scene_route_pointer`、`KeyRouter::route` +
`Session::switch_vt_for` + `Seat::release_keys`、`Seat::follow`、用布局坐标的 `present`。两者剩下的差别只在
语言本身：C++ 在信号回调里处理，Rust 在 `dispatch()` 返回的事件上 match；popup 约束 C++ 同步回答、Rust 事先告知；
Rust 版一个带 lifetime 的结构体都没写。行数：C++ 639 → 500 → 365，Rust 677 → 420 → 322。

- **窗口状态请求**：两边都不回答 maximize / fullscreen / minimize，由库按兜底批准（Rust 不调
  `forward_window_requests()`）。

- **后端由库来挑**：两边都只读环境变量、填一个 `BackendOptions`，交给 `AutoBackend`。之前两边各自写了约 50 行
  挑后端的代码，每个基于本库的混成器都会照抄一遍；Rust 版还为此带着一个本地 `enum Backend` 和一个 `'pick:`
  标签块——DRM 失败时要把会话还回来，而只有写在 `main` 里借用检查器才看得出那次借用已经结束。现在会话归
  `AutoBackend`，这些都不需要了；`backend.session()` 在用到时临时借出会话。两边启动时都打印
  `minimal: backend = …`。

- **输入只有一条路径**：两个示例都把嵌套和 libinput 当作同一个 `InputSource`，一个处理函数。之前各写两遍，
  Rust 的嵌套路径把滚轮的刻度和平滑量都发给客户端，每格滚两次；C++ 版与 DRM 路径只发刻度。现在这条规则在
  `Seat::pointer_axis_event` 里，两边都调它。按键的 keysym 也由后端给出，嵌套路径不再自己维护一份 `KeymapState`。

- **首选缓冲缩放与分数缩放**：窗口 map 时把输出的整数缩放与分数缩放发给客户端——`SurfaceId::set_preferred_buffer_scale(scale)`、
  `SurfaceId::set_preferred_buffer_transform(transform)` 对应 `wl_surface.preferred_buffer_scale` /
  `preferred_buffer_transform`（`wl_surface` v6 起），`FractionalScaleManager::set_scale(surface, 1.5)` 同时发
  `wp_fractional_scale_v1.preferred_scale` 与向上取整的整数提示，两个示例都只调它。输出缩放变化后要重新发。
- **子表面树**：`SurfaceId::surface_tree() -> Vec<SurfaceAt>` 按后到前的顺序给出这棵树（含子表面），
  每一项的 `x`/`y` 是它相对根表面的偏移。按住按钮、抓取落在子表面上时，`scene_route_pointer` 用它算出该子表面
  在屏幕上的原点（场景项的位置 + 偏移），指针坐标才不会按整个窗口的原点算。

另有两条与「对象活得比输出久」相关的约定，热插拔时靠它们兜底：`Frame`（借用 `VulkanRenderer` 的那个）
在输出销毁时被 shim 直接丢弃，之后 `reset` / `submit` 返回错误、其余方法是空操作；`DirectScanout`
在 C++ 侧自己把 `Output*` 置空，所以输出拔掉之后 `available()` 为 false、`id_for()` 为 `None`。这两条由
`tests/test_escape_hatches.rs` 用一个被丢掉的 headless 后端守着。

现已补全的跨 crate 互操作与功能支持：

- **`LinuxDmabuf` 与 GPU 共享**：可通过 `LinuxDmabuf::from_scene_renderer(&display, &renderer)`（或 `renderer.create_linux_dmabuf(&display)`）直接从 `SceneRenderer` 创建；`examples/minimal.rs` 已接驳该协议，GPU 客户端可直接走零拷贝 dmabuf。
- **光标形状协议 (`cursor-shape`)**：通过 `Cursor::new_with_shapes(&renderer, &layout, &seat, &shapes)` 将 `CursorShapeManager` 接入光标引擎，自动呈现客户端请求的命名光标形状，已在 `examples/minimal.rs` 中完整启用。
- **图像数据输入 (`SceneItem::image`)**：`SceneRenderer::set_image(serial, width, height, rgba: &[u8])` 接受紧凑排布的原始 RGBA `&[u8]` 字节切片（`image` crate 的图像缓冲区可直接传入），与场景中的 `SceneItem::image(rect, serial)` 对应；GPU 路径按 serial 自动去重缓存与动态上传，CPU 路径自动回退合成。
- **显示器配置 (`OutputGlobal`)**：`for_output()` 跟随输出与布局；`logical_position()`、`size()`、`logical_size()`、`scale()`、`transform()`、`refresh_mhz()`、`name()` 读的都是 C++ global 当前描述的值。
- **分类错误类型 (`core::ErrorKind`)**：`core::Error` 现包含可模式匹配的 `ErrorKind`（`err.kind()`），涵盖 `Init`、`EventLoop`、`Backend`、`Gpu`、`Protocol`、`Shell`、`InvalidArgument`、`Other`；保留 `Display` 与 `std::error::Error` 实现，可无缝配合 `?` 操作符、`anyhow` 与 `thiserror`。
- **几何类型转换 (`mint` / `glam` 特性)**：提供可选 cargo 特性 `mint` 与 `glam`，为 `Rect`、`Color`、`Point` 提供 `From`/`Into` 实现，避免调用者受孤儿规则限制而编写 newtype 包装。
- **事件循环集成 (`calloop` 与 `tokio`)**：
  - **`calloop`**：`Display` 实现了 `AsFd`，可将其文件描述符通过 `calloop::generic::Generic` 注册至 calloop 事件循环（详见 `examples/calloop.rs`）。就绪时调用 `display.dispatch(Some(Duration::ZERO))`，处理它返回的事件；自己派发事件循环的调用者用 `display.events()`。
  - **`tokio`**：注意 luminaria 所有类型均为 `!Send`（所有 Wayland 对象、后端句柄与 C++ 状态均绑定至创建 `wl_display` 的单线程主循环）。因此在 tokio 中必须使用单线程运行时（`tokio::runtime::Builder::new_current_thread()`）或在 `tokio::task::LocalSet` 内运行；使用 `tokio::io::unix::AsyncFd::new(display.as_fd())` 监听可读就绪事件，随后执行非阻塞 `display.dispatch(Some(Duration::ZERO))` 并清空就绪标记（`guard.clear_ready()`）。

整个绑定现已完整转发全部信号（C++ 产生信号时按触发顺序推进同一条队列，`dispatch()` 返回时成为 `Event`）：

- **`Compositor`**：`CompositorEvent::NewSurface`，以及每个 surface 的 `Commit`、`Destroy`、`Rendered` 与全局的 `Invalidated`。
- **`Output`**：`Event::Output(id, OutputEvent::{Frame, Present(PresentEvent), ModeChanged})`（`PresentEvent` 含硬件时钟与单调时间戳、刷新周期与垂直同步标志）。
- **`WaylandBackend`**：作为 `InputSource` 转发 `Key`（带 keysym）、`MotionAbsolute`（带输出）、`PointerLeave`、`Button`、`Modifiers`、`Axis`（平滑滚动量、离散步进刻度与停止标记）、`KeymapChange` 与 `Capabilities`；关窗请求是 `Event::HostCloseRequest`。
- **需同步回答的信号**（遵照 C++ 绝不调用 Rust 的规矩，策略预先配在 shim 上）：
  - xdg-activation 的 `ActivationTokenRequest`：默认批准（可通过 `set_grant_tokens(bool)` 预设策略），同时作为 `Event::Activation` 到来。
  - cursor-shape 的 `CursorShapeRequest`：通过 `Cursor::new_with_shapes(&renderer, &layout, &seat, &shapes)` 将 `CursorShapeManager` 接进 `Cursor`，在 C++ 信号发射期间直接由光标引擎同步呈现所请求的光标；该请求同时作为 `Event::CursorShape` 到来。
- **桌面协议请求**：
  - `XdgDecorationManager`：`Event::Decoration(DecorationRequest)`（携带表面、客户端偏好与最终决议），可由 `set_default_mode` 预先配置默认模式。
  - `ForeignToplevelManager`：`Event::ForeignToplevel(ForeignToplevelRequest)`（最大化、最小化、全屏、激活、关闭等请求）。
  - `WorkspaceManager`：`Event::Workspace(WorkspaceRequest)`（激活、反激活、创建、移除等工作区请求）。
  - `OutputManager`：`Event::OutputConfig(OutputConfigRequest)`（测试与应用请求，可通过 `set_auto_verdict` 设置默认裁决）。
  - `XdgToplevelIconManager`：`Event::ToplevelIcon(ToplevelIconChange)`，并提供 `icon_name(surface)` 查询客户端设定的图标名。

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
