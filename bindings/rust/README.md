# luminaria（Rust 绑定）

用 [cxx](https://cxx.rs) 从 Rust 调用 luminaria。C++ 侧做所有混成器都一样做的事
（全局对象、渲染、帧节拍、帧回调、presentation 反馈、popup、layer-shell、光标、指针焦点、
截屏、锁屏、输入法），Rust 侧只剩窗口管理：窗口放哪、叠放顺序、尺寸、焦点、快捷键、工作区。

```rust
use luminaria::{Config, Event, Item, Server};

let mut server = Server::new(Config::default())?;
println!("WAYLAND_DISPLAY={}", server.socket_name());
let mut windows = Vec::new();
server.run(|server, event| {
    if let Event::WindowMap(w) = event {
        windows.push(w);
        server.focus(Some(w));
        let scene: Vec<Item> = windows.iter().map(|&id| Item::Window { id, x: 40, y: 40 }).collect();
        server.set_scene(&scene);
    }
    std::ops::ControlFlow::Continue(())
})?;
```

完整示例见 `examples/floating.rs`（层叠放置、点击聚焦、拖标题栏移动、拖边缘缩放、
避开 panel 的最大化、最小化、两个工作区）：

```sh
cargo run --example floating                                    # 嵌套在当前会话里
LUMINARIA_BACKEND=headless LUMINARIA_EXIT_MS=500 cargo run --example floating
LUMINARIA_BACKEND=drm cargo run --example floating              # 裸机，在空闲 VT 上跑
```

## 后端

`Config::backend` 选输出到哪里：

| 值 | 含义 |
|---|---|
| `Auto`（默认） | 依次尝试：嵌套 → DRM（只在拿得到 libseat session 时，即确实坐在一个 VT 上，ssh/CI 不会误入）→ headless |
| `Nested` | 宿主混成器里的一个窗口，失败就报错 |
| `Headless` | 不显示，给测试和 CI 用 |
| `Drm` | 裸机：KMS 出图、libinput 输入、libseat 管设备和 VT 切换。`Config::drm_device` 指定显卡，`None` 选第一块接了显示器的 |

裸机上 shim 额外做的事（都是机制，不是策略）：libinput 相对位移 → 指针位置（从第一块屏中央
起步）、硬件光标平面（光标在 plane 上时移动鼠标不重绘）、按插着的设备通告 seat 能力、
热插拔显示器、**Ctrl+Alt+F1..F12 切 VT**——按 keymap 给出的 `XF86Switch_VT_n` 判断，切走前把
客户端那边按住的键全部松开，窗口管理器绑不到也拦不住它。切走/切回会收到
`Event::SessionActive { active }`。

**触摸屏**也在这里接好：libinput 给的是归一化到触摸面板的坐标，shim 把它映射到**第一块输出**
的布局框（假定面板和这块屏的原生朝向一致，所以输出旋转时触摸跟着转），用和指针同一份场景
做命中测试，按表面局部坐标发给客户端。每个触点按 slot 记住按下时落在哪个表面上，之后的
移动和抬起都发给它，手指滑出窗口也一样（和按住鼠标键时的隐式 grab 相同）。seat 的触摸能力
按插着的设备如实通告。Rust 另外收到一份 `Event::TouchDown` / `TouchMotion` / `TouchUp`
（全局坐标、触点 id、按下时的窗口），用来做窗口管理器自己的手势，比如示例里的点按聚焦。
设备取消触点或 VT 被切走时，客户端收到 `wl_touch.cancel`，Rust 对每个还按着的触点收到一个
`TouchUp`。多块触摸屏、触摸屏不在第一块输出上的情况暂不处理。

没有 libseat session（既没有 logind seat 也没有 seatd）时 `Drm` 仍能从已登录的 VT 启动，
但 VT 切换会被禁用——这时请绑一个退出键（示例里是 Logo+Escape），第一次试的时候再加上
`LUMINARIA_EXIT_MS=10000` 兜底。

## 协议：哪些自动处理，哪些交给 Rust

| 协议 | shim 自动做的 | 留给 Rust 的 |
|---|---|---|
| xdg-shell | popup 摆位与约束、grab 外点击关闭菜单 | 窗口位置、尺寸、焦点：`set_scene`、`configure`、`focus`；`Request*` 事件 |
| wlr-layer-shell | 按输出排版与独占区；background/bottom 画在窗口下，top/overlay 在上；panel 的 popup；`exclusive` 抢键盘、`on_demand` 点击获得键盘 | `output_usable()` 与 `Event::OutputChange`：窗口该放进的区域 |
| ext-session-lock | 锁定即只画锁屏、只给锁屏输入、快捷键全部失效；所有输出都画出锁屏后才回 `locked`；锁屏程序崩溃时保持锁定（fails closed） | `Event::SessionLock`、`is_locked()`，仅供知情 |
| wlr-screencopy / ext-image-copy-capture | 从已上屏的画面读回（含 dmabuf 目标），光标作为单独的捕获源 | — |
| foreign-toplevel、xdg-activation | 任务栏列表自动同步；关闭请求直接执行；只有有效 token 才能转移焦点 | 转成 `RequestActivate` / `RequestMaximize` / `RequestMinimize` 等同一套事件 |
| wlr-output-management | 每个输出一个 head（名字与 wl_output 相同），热插拔与任何 `set_output_*` 之后自动重发；`wlr-randr` / kanshi 的 apply 直接执行（见下） | 照常收到 `Event::OutputChange` |
| ext-workspace | 一个 group，覆盖所有输出 | `add_workspace` / `activate_workspace` / `remove_workspace`；`Event::WorkspaceActivate` |
| text-input-v3 + input-method-v2 | 输入法抢键盘时按键转给它；候选框贴在文本光标下 | — |
| pointer-constraints、relative-pointer | 指针锁定/限制随指针焦点生效；裸机上发相对位移 | — |
| idle-notify、idle-inhibit | 任何输入都算活动；有 inhibitor 时不进入 idle | — |
| data-device、primary-selection、wlr-data-control | 剪贴板、拖放、中键粘贴、剪贴板管理器 | — |
| 其余（viewporter、fractional-scale、presentation-time、fifo、commit-timing、tearing-control、content-type、cursor-shape、single-pixel-buffer、linux-dmabuf、drm-syncobj、xdg-decoration、background-effect、xdg-toplevel-icon） | 全部自动；客户端声明的模糊区域在 GPU 上照做 | `icon_name()`；`Config::server_side_decorations` |

交互式移动/缩放：收到 `RequestMove` / `RequestResize` 后 `grab_pointer(true)`（客户端看不到指针移动），
缩放时配合 `set_resizing` 和 `Rect::resized(edges, dx, dy, min_size)`，松开按键后 `grab_pointer(false)`。

输出（裸机上才有意义）：`output_modes`、`set_output_mode`、`set_output_scale`、
`set_output_transform`、`set_output_position`，之后都会收到 `Event::OutputChange`。键盘布局用
`set_keyboard_layout("de")`；嵌套时按键来自宿主，跟随宿主的布局。

用户用 `wlr-randr` / kanshi 改输出时，shim 把它当作机制直接执行，不经过 Rust 审批，也不新增任何
Rust API：配置走的就是上面那几个 `set_output_*` 的同一条路径，Rust 照常收到
`Event::OutputChange`，和自己调用时分不出区别。先整体校验再动手，做不到的整份拒绝（`failed`），
什么都不改：关掉某个输出（输出只会因拔掉而离开 layout）、开启自适应同步、输出不支持的 mode。
小数 scale 四舍五入成整数，协议允许这样做。mode 切换是唯一可能被后端当场拒绝的一步，所以最先做，
失败时把已经切过的输出切回去。

## 构建

需要 xmake、clang ≥ 22（或 gcc ≥ 16）、glslangValidator，以及 luminaria 本身的系统依赖。
`cargo build` 一条命令即可，`build.rs` 分三步：

1. `cxx-build` 展开 `src/lib.rs` 里的 `#[cxx::bridge]`，生成 `lib.rs.h` / `lib.rs.cc`，
   用 `cc` 编译胶水。`cc` 不认识 C++ 模块——胶水只 include `luminaria_rs.h`，用不着。
2. xmake 编译 luminaria 和 `luminaria_rs.cpp`（唯一 import 模块的文件），
   include 路径里带上第一步生成的头文件。xmake 的状态和产物都放在 `OUT_DIR` 下，不污染源码树。
3. 链接四个静态库，系统库由 pkg-config 给出。

两边必须用同一套 C++ 标准库，否则跨桥的 `std::unique_ptr` 布局对不上。默认两边都用
clang（Linux 上即都是 libstdc++）；`LUMINARIA_TOOLCHAIN=gcc` 会把两边一起换成 gcc。
`LUMINARIA_XMAKE_MODE=debug|release` 可以单独指定 C++ 侧的构建模式（默认跟随 cargo profile）。

`LUMINARIA_SANITIZE=address`（或 `undefined`）把两边都插桩。链接也必须交给同一家编译器，
建议用单独的 target 目录，免得整个重编：

```sh
CARGO_TARGET_DIR=target-asan LUMINARIA_SANITIZE=address RUSTFLAGS="-C linker=clang" cargo build --example floating
```

## 文件

```
luminaria_rs.h     普通头文件，不含模块，也不出现任何 luminaria 类型；给 cxx 读
luminaria_rs.cpp   import luminaria.gpu / luminaria.desktop; 所有封装都在这里，由 xmake 编译
src/lib.rs         #[cxx::bridge]（私有）加一层安全封装（公开 API）
build.rs           上面三步
xmake.lua          shim 的 xmake 子工程，includes("../..") 引入 luminaria
```

## 几条设计上的取舍

- **C++ 从不回调 Rust。** libwayland 的回调只往队列里塞 `Event`；`dispatch()` 返回后
  Rust 用 `next_event()` 取。任何 Rust 调用发生时栈上都没有 luminaria 的信号槽，所以
  不存在重入：事件处理函数可以直接拿 `&mut Server`，不需要把状态塞进回调对象。
  桥里因此没有 `extern "Rust"` 块。要嵌进别的事件循环，用 `AsRawFd` 拿到 fd，可读时
  `dispatch(Some(Duration::ZERO))`。
- **不用 cxx 的 `Result` 映射。** 它靠 C++ 异常把 `Err` 带过来，而本项目约定异常不跨
  C 边界。`server_new` 失败返回空指针，原因写进 `&mut String`；客户端给的字符串一律
  `rust::String::lossy`，因为带校验的构造函数遇到非法 UTF-8 会抛异常。
- **只把 id 交给 Rust。** `WindowId` / `OutputId` 是单调递增、永不复用的 u64；对象没了，
  拿旧 id 调用就是空操作。`Surface*`、`Toplevel*` 永远不过桥。
- **生命周期靠成员声明顺序。** `Server::Impl` 里 `SceneRenderer` 最先声明（最后析构），
  满足"renderer 必须比 Display 活得久"；持有 `Seat&` 的全局对象排在 seat 后面；每窗口、
  每输出的状态最后声明（最先析构），它们的信号连接先断开。销毁回调里只做标记，真正释放
  推迟到下一次 `dispatch()`。析构时**先断开所有客户端**，再拆全局对象：客户端资源在死去时
  会回调它的全局对象（退出时还连着的 wl-copy 曾让 data-control 读到已释放的
  `DataDeviceManager`）。
- **场景是每次整体替换的列表**（ADR 0001 的即时模式）：布局变了就 `set_scene(&items)`，
  画什么和点中什么来自同一张列表，重绘范围由库里的列表 diff 算出。`Item::Window` 的坐标
  是窗口几何（不含客户端阴影）的左上角，popup 跟着父窗口自动摆放。

## 还没有的

- **嵌套后端的触摸。** `WaylandBackend` 不转发宿主的 `wl_touch`，所以触摸只在 `Drm` 后端有。
- 截屏注册的输出尺寸是客户端绑定 wl_output 时的尺寸；改模式后新绑定的客户端才拿到新尺寸。
