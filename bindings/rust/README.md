# luminaria（Rust 绑定）

用 [cxx](https://cxx.rs) 从 Rust 调用 luminaria。

Crate 逐类型公开 `luminaria`、`luminaria.gpu`、`luminaria.desktop`、`luminaria.text` 四个 C++23 模块的类型与能力，由使用者自行组合成混成器；窗口放哪、谁拿键盘、按键做什么，这些策略始终留在 Rust 一侧。

- 详细 API 说明：参见 [API 文档](docs/api.md)

## 特性开关（cargo features）

绑定跟着 C++ 的模块切（ADR 0003）：**每个模块一个 `#[cxx::bridge]`、一个 shim 翻译单元**，
cargo 特性决定编译哪些、链接哪些。关掉的模块不进编译、不进链接，也不探测它的系统库。

| 特性 | 默认 | 对应的 C++ 模块 | 打开后多出来的东西 | 额外链接的系统库 |
|---|---|---|---|---|
| *（始终有）* | — | `luminaria` | 协议、`Display`、headless / 嵌套后端、`CpuCompositor`、`SceneItem` | wayland-server、wayland-client、xkbcommon |
| `gpu` | ✅ | `luminaria.gpu` | `gpu` 模块（`VulkanRenderer`/`SceneRenderer`/`Frame`/`Cursor`/`DirectScanout`/`LinuxDmabuf`/`Screencopy`），`backend::DrmBackend`/`LibinputBackend`/`Session` | vulkan、libdrm、libinput、libudev、gbm、libseat |
| `desktop` | ✅ | `luminaria.desktop` | `desktop` 模块（任务栏、工作区、锁屏、输出管理、剪贴板、输入法） | — |
| `text` | ❌ | `luminaria.text` | `text` 模块（`Font`：字形栅格化与度量） | fcft、fontconfig、pixman |
| `raw-vulkan` | ❌ | —（隐含 `gpu`） | `VulkanRenderer::raw_handles()` / `SceneRenderer::raw_vulkan_handles()` | — |
| `mint` / `glam` | ❌ | — | 几何类型的 `From`/`Into` 转换 | — |

两个扩展互不依赖：只开 `desktop` 就是一个不链接 Vulkan 的桌面外壳。只写无头合成器（不需要
Vulkan、不需要 DRM/libinput/libseat）时：

```sh
cargo build --no-default-features            # 只有核心：link line 里没有 vulkan/gbm/libseat
cargo build --no-default-features --features gpu
cargo build                                  # 默认：gpu + desktop，与之前完全一致
```

只用 `--no-default-features` 构建出的二进制，`ldd` 只剩 wayland 与 xkbcommon；这是本绑定
存在的意义之一，所以它由 `tests/` 里的默认特性组合守着。

## 模块结构

Crate 按库的职责分层组织为以下模块：

- **`core`**：`Display`（拥有 `wl_display` 和事件循环；`dispatch()` 返回这一轮的全部 `Event`）、`EventLoop`、`KeyRouter`（快捷键与客户端之间分键盘）、`Error`（可模式匹配的 `ErrorKind` 分类）。
- **`event`**：`Event`——库报告的一切都走这一条流，按信号触发的顺序；各载荷类型都能用 `TryFrom<Event>` 取回。
- **`util`**：基础几何与格式工具：`Rect`、`Color`、`Point`、`Transform`、`Region`（不相交矩形集）、`KeymapState`（xkb 状态封装）、`Pixel`（RGBA8，与 `read_back` / `commit_frame` 的字节顺序一致）、`Keysym`（每个 `XKB_KEY_*` 一个关联常量，由系统的 xkbcommon 生成）、`DrmFourcc`（来自 `drm-fourcc` crate）、`edge`（xdg_toplevel 缩放边缘位）、`layout_fits`（可选 `mint` 与 `glam` 特性支持零样板几何转换）。
- **`backend`**：输出与硬件/虚拟后端：`Output`、`OutputMode`、`HeadlessBackend`（无头软件帧泵）、`WaylandBackend`（宿主嵌套窗口）；`DrmBackend`（KMS bare-metal）、`LibinputBackend`（输入事件）、`Session`（libseat 会话管理）只在特性 `gpu` 下存在。
- **`protocol`**：Wayland 核心协议全局对象：`CoreGlobals`（一次创建每个混成器都要的那一组）、`Compositor`、`Subcompositor`、`SurfaceId`、`XdgShell`、`Seat`、`LayerShell`、`OutputGlobal`、`CursorTheme`，以及各类协议管理器（`Viewporter`、`FractionalScaleManager`、`XdgActivation`、`XdgDecorationManager`、`CursorShapeManager`、`DataDeviceManager`、`PrimarySelectionManager`、`IdleInhibitManager`、`IdleNotifier`、`RelativePointerManager`、`PointerConstraints`、`TextInputManager`、`SinglePixelBufferManager`、`TearingControlManager`、`ContentTypeManager`、`FifoManager`、`CommitTimingManager`、`Presentation`、`BackgroundEffectManager`、`XdgToplevelIconManager`）。
- **`gpu`**（对应 `luminaria.gpu`，特性 `gpu`）：`VulkanRenderer`（Vulkan 1.3 动态渲染与 dmabuf 导入）、`SceneRenderer`（GPU 混合与 CPU 自动回退，自己按输出保存渲染状态）、`Frame`（即时模式逐输出帧账本）、`Cursor`（硬件光标平面与主题精灵跟随）、`DirectScanout`（全屏零拷贝直出评估）、`LinuxDmabuf`、`DrmSyncobjManager`、`ScreencopyManager`。
- **`desktop`**（对应 `luminaria.desktop`，特性 `desktop`）：桌面特权协议管理器：`ForeignToplevelManager`（任务栏窗口列表）、`WorkspaceManager`（ext-workspace 工作区）、`SessionLockManager`（ext-session-lock 锁屏）、`OutputManager`（wlr-output-management 显示器配置）、`DataControlManager`（剪贴板访问）、`InputMethodManager`（输入法协议）。
- **`text`**（对应 `luminaria.text`，特性 `text`）：`Font`（fcft 字形栅格化与度量）、`decode_utf8`。不画文字的混成器不必链接 fontconfig。
- **`shell`**：即时模式场景与排版：`OutputLayout`（全局逻辑多屏布局）、`SceneItem`（场景绘制项）、`SceneHit` 与 `scene_hit_test`（命中测试）、`scene_route_pointer`（指针路由，含隐式 grab）、`CpuCompositor`（纯 CPU 软件合成器）、`PopupTree`（自己跟随 `XdgShell` 的 popup 树与 grab 关闭）、`Pointer`。

---

## 快速上手

组合 Display、后端、协议与渲染流程：

```rust
use std::time::Duration;
use luminaria::core::Display;
use luminaria::backend::HeadlessBackend;
use luminaria::protocol::{Compositor, XdgShell, Seat, OutputGlobal};
use luminaria::shell::{CpuCompositor, OutputLayout, SceneItem};
use luminaria::util::{Color, Rect};

// 1. 初始化 Display 与事件循环
let display = Display::new()?;
let socket = display.add_socket_auto()?;
display.init_shm()?;

// 2. 创建后端与输出
let mut backend = HeadlessBackend::new(&display, 16)?;
let mut output = backend.add_output(1280, 720)?;
backend.start()?;

// 3. 注册协议全局对象
let _compositor = Compositor::new(&display)?;
let xdg_shell = XdgShell::new(&display)?;
xdg_shell.set_bounds((1280, 720));
let seat = Seat::new(&display, "seat0")?;
seat.set_capabilities(true, true, false);

// 4. 配置屏幕布局
let layout = OutputLayout::new()?;
layout.add(&mut output, 0, 0);
let _out_global = OutputGlobal::new(&display, 1280, 720, "HEADLESS-1")?;

// 5. 软件合成或送入 GPU 渲染
let mut comp = CpuCompositor::new()?;
comp.composite_rect(Rect::new(50, 50, 400, 300), Color::rgb(0.2, 0.4, 0.8));
let pixels = comp.finish(1280, 720, Color::rgb(0.1, 0.1, 0.1));
output.commit_frame(&pixels, 1280, 720)?;

// 6. 驱动事件循环：这一轮发生的一切都在返回的事件里
for event in display.dispatch(Some(Duration::ZERO))? {
    if let luminaria::Event::Toplevel(surface, e) = event {
        println!("{surface:?} {e:?}");
    }
}
```

完整可用的混成器见 `examples/minimal.rs`：它与根目录的 `examples/minimal.cpp` 是一对，窗口策略与快捷键相同（每个窗口最大化、新的在最上面、点击置顶；Logo+Return 开终端、Logo+Tab 轮换、Logo+q 关窗、Logo+Escape 退出），后端选择也相同（嵌套 / DRM / headless）。

要和别的 Rust crate 共用底层对象——在 luminaria 的 `wl_display` 上注册自定义 global、把 DRM fd 交给 `drm`/`gbm`、用 ash 在同一块 GPU 上提交工作——见 [`docs/api.md`](docs/api.md) 第 9 节"逃生口"。

```sh
cargo run --example minimal                                        # 嵌套在当前桌面
LUMINARIA_BACKEND=headless LUMINARIA_EXIT_MS=500 cargo run --example minimal  # 冒烟测试
cargo run --example calloop                                        # calloop 事件循环集成示例
```

更多组装细节可参考 `tests/test_custom_compositor.rs`。

---

## 线程、生命周期与安全约束

1. **单线程约束（Single-Threaded）**：
   底层 `libwayland-server` 是单线程事件循环架构，所有涉及 Wayland 对象、后端输出与渲染器的操作必须在同一个线程中运行。所有 FFI 包装类型均未实现 `Send` 与 `Sync`。事件队列是每线程一条，所以**一个线程只放一个 `Display`**；两个混成器（比如测试里的宿主与嵌套端）各占一个线程。
2. **析构顺序由借用检查器保证**：
   - C++ 对象之间的引用都写进了 Rust 类型的生命周期参数：协议 global、后端和 `Session` 借用 `Display`（`Seat<'d>`、`HeadlessBackend<'d>`……），`Cursor<'a>` 借用 renderer、layout 和 seat，等等。被借的一方先 drop 就**编译不过**，不再是运行时的 use-after-free。完整清单见 [API 文档](docs/api.md) 的"借用关系"一节。
   - 会被别的对象借住的类型（`Seat`、`XdgShell`、`OutputLayout`、`SceneRenderer`、`Session`）的方法都取 `&self`，所以借出之后照样能用。
   - 局部变量按声明的逆序析构，被借的先声明即可。`SceneRenderer` 不借任何东西，每块输出的渲染状态由它自己保存，所以放在哪里都行，只要在借它的 `Cursor` / `LinuxDmabuf` 之前：
     ```rust
     let display = Display::new()?;
     let globals = CoreGlobals::new(&display)?;          // 借 display
     let layout = OutputLayout::new()?;
     let renderer = SceneRenderer::new(&opts)?;
     let cursor = Cursor::new(&renderer, &layout, globals.seat())?; // 借三者，最先析构
     ```
   - 结构体不能同时持有被借者和借用者（那是自引用）。把被借的对象放在外层（`main` 的局部变量，或一个只装它们的结构体），再让持有 `Cursor<'a>` 之类的结构体借用它们。
3. **世代标识符（Generational SurfaceId）**：
   跨事件调度或存储时不直接持有裸指针，而是使用 `SurfaceId`。`SurfaceId` 包含槽位索引与单调自增的世代号；当表面销毁时，该 ID 自动失效，不会发生悬垂指针或复用误判。
4. **异常与错误安全**：
   C++ 侧的 `Result<T>` 与 `Status` 统一在 FFI 边界转化为 Rust 的 `Result<T, Error>`，不让 C++ 异常跨越 FFI 边界。

---

## 外部事件循环集成

`Display` 和 `EventLoop` 均实现了 `AsFd` 与 `AsRawFd`，可以直接嵌入外部事件循环（如 `calloop`、`epoll`、`tokio`）。

### Calloop

参见完整工作示例 `examples/calloop.rs`：将 `Display` 的底层事件循环文件描述符包装为 `calloop::generic::Generic` 注册至事件循环，在可读回调中执行非阻塞 `display.dispatch(Some(Duration::ZERO))` 并排干协议事件队列：

```rust
use std::os::fd::AsRawFd;
use std::time::Duration;
use calloop::{generic::{FdWrapper, Generic}, EventLoop, Interest, Mode, PostAction};
use luminaria::core::Display;

let mut event_loop = EventLoop::<()>::try_new()?;
let handle = event_loop.handle();
let display = Display::new()?;

// Safe: display 活过事件循环
unsafe {
    handle.insert_source(
        Generic::new(FdWrapper::new(display.as_raw_fd()), Interest::READ, Mode::Level),
        |_readiness, _fd, _shared| {
            display.dispatch(Some(Duration::ZERO))?;
            // 排干事件队列并按需渲染...
            Ok(PostAction::Continue)
        },
    )?;
}
```

### Tokio

注意 luminaria 的所有对象均**未实现 `Send`**（单线程事件循环架构）。使用 Tokio 时：
1. 必须使用单线程运行时（`tokio::runtime::Builder::new_current_thread()`）或在 `tokio::task::LocalSet` 内运行。
2. 将 `display.as_fd()` 包装进 `tokio::io::unix::AsyncFd`：

```rust
use std::os::fd::AsFd;
use std::time::Duration;
use tokio::io::unix::AsyncFd;
use luminaria::core::Display;

async fn run(display: Display) -> Result<(), Box<dyn std::error::Error>> {
    let async_fd = AsyncFd::new(display.as_fd())?;
    loop {
        let mut guard = async_fd.readable().await?;
        display.dispatch(Some(Duration::ZERO))?;
        // 在此处排干协议事件队列...
        guard.clear_ready();
    }
}
```

---

## 硬件与环境前置条件

1. **无头与嵌套（Headless / Nested）**：
   可在普通终端、CI 或宿主 Wayland 会话中直接运行，无需特殊权限。
2. **裸机运行（DRM / KMS + Libinput）**：
   - 需要在独立的空闲 Linux VT（虚拟控制台）上运行，通过 `libseat`（配合 `seatd` 或 `systemd-logind`）接管显卡 DRM Master 和输入设备。
   - 在桌面终端模拟器内直接启动 DRM 后端会因非 DRM Master 或 seat 会话被拒而返回错误。
3. **GPU 渲染（Vulkan 1.3）**：
   - GPU 渲染路径基于 Vulkan 1.3、`VK_EXT_external_memory_dma_buf` 与 `VK_EXT_image_drm_format_modifier`。
   - `SceneRenderer` 具有全自动运行时降级能力：当机器没有可用 Vulkan 物理设备或目标分配失败时，自动降级至 CPU 软件合成路径，保证启动可靠性。
4. **自动化测试约定**：
   - 依赖裸机硬件环境（真实 DRM Master、切换 VT、真实输入设备节点）的测试默认跳过；仅在明确设置环境变量 `LUMINARIA_TEST_HARDWARE=1` 时执行。
   - 测试运行期间会自动将 `XDG_RUNTIME_DIR` 隔离至临时目录，不会对宿主运行中的桌面会话造成任何侵入或冲突。

---

## 构建

构建流程由 `build.rs` 自动编排，且只做启用的特性要求的事：
1. `cxx-build` 处理**已启用**的桥接定义（`src/ffi.rs`，以及 `src/ffi_gpu.rs` / `src/ffi_desktop.rs` / `src/ffi_text.rs`），生成 FFI 头文件与胶水代码。
2. `xmake --modules=gpu,desktop,…` 编译对应的 luminaria C++23 模块与 shim 翻译单元
   （`luminaria_rs.cpp` / `luminaria_rs_gpu.cpp` / `luminaria_rs_desktop.cpp` / `luminaria_rs_text.cpp`），
   并行度继承 cargo 的 `-j`，防止模块编译吃光内存。
3. Cargo 只链接上表里那一行对应的系统库——核心构建不探测、不链接 Vulkan/DRM/libinput/libseat/gbm。

```sh
cargo build                 # 构建库与依赖
cargo test                  # 运行单元测试与集成测试
cargo run --example minimal # 运行示例
```
