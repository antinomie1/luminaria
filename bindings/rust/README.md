# luminaria（Rust 绑定）

用 [cxx](https://cxx.rs) 从 Rust 调用 luminaria。C++ 侧做所有混成器都一样做的事
（全局对象、渲染、帧节拍、帧回调、presentation 反馈、popup、光标、指针焦点），
Rust 侧只剩窗口管理：窗口放哪、叠放顺序、尺寸、焦点、快捷键。

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

完整示例见 `examples/floating.rs`（层叠放置、点击聚焦、拖标题栏移动、最大化）：

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

没有 libseat session（既没有 logind seat 也没有 seatd）时 `Drm` 仍能从已登录的 VT 启动，
但 VT 切换会被禁用——这时请绑一个退出键（示例里是 Logo+Escape），第一次试的时候再加上
`LUMINARIA_EXIT_MS=10000` 兜底。

## 构建

需要 xmake、clang ≥ 22（或 gcc ≥ 16）、glslangValidator，以及 luminaria 本身的系统依赖。
`cargo build` 一条命令即可，`build.rs` 分三步：

1. `cxx-build` 展开 `src/lib.rs` 里的 `#[cxx::bridge]`，生成 `lib.rs.h` / `lib.rs.cc`，
   用 `cc` 编译胶水。`cc` 不认识 C++ 模块——胶水只 include `luminaria_rs.h`，用不着。
2. xmake 编译 luminaria 和 `luminaria_rs.cpp`（唯一 `import luminaria.gpu;` 的文件），
   include 路径里带上第一步生成的头文件。xmake 的状态和产物都放在 `OUT_DIR` 下，不污染源码树。
3. 链接三个静态库，系统库由 pkg-config 给出。

两边必须用同一套 C++ 标准库，否则跨桥的 `std::unique_ptr` 布局对不上。默认两边都用
clang（Linux 上即都是 libstdc++）；`LUMINARIA_TOOLCHAIN=gcc` 会把两边一起换成 gcc。
`LUMINARIA_XMAKE_MODE=debug|release` 可以单独指定 C++ 侧的构建模式（默认跟随 cargo profile）。

## 文件

```
luminaria_rs.h     普通头文件，不含模块，也不出现任何 luminaria 类型；给 cxx 读
luminaria_rs.cpp   import luminaria.gpu; 所有封装都在这里，由 xmake 编译
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
  满足"renderer 必须比 Display 活得久"；每窗口、每输出的状态最后声明（最先析构），
  它们的信号连接先断开。销毁回调里只做标记，真正释放推迟到下一次 `dispatch()`。
- **场景是每次整体替换的列表**（ADR 0001 的即时模式）：布局变了就 `set_scene(&items)`，
  画什么和点中什么来自同一张列表，重绘范围由库里的列表 diff 算出。`Item::Window` 的坐标
  是窗口几何（不含客户端阴影）的左上角，popup 跟着父窗口自动摆放。

## 还没有的

layer-shell、截屏、桌面特权协议、触摸、交互式缩放的辅助、裸机上的输出模式/缩放/旋转设置
（目前每块屏保持连接器的首选模式，scale 1）。
