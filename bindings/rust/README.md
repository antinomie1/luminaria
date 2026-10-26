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
```

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

DRM/libinput 裸机后端（目前只有嵌套和 headless）、layer-shell、截屏、桌面特权协议、
触摸、交互式缩放的辅助。
