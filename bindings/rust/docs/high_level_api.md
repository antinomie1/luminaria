# Luminaria 高层 API 文档（`Server` 封装）

`Server` 是 Luminaria 为窗口管理器开发者提供的高层、开箱即用的混成器封装。

在设计上，底层 C++ 库完成所有混成器通用且固定的机制工作（Display 全局对象维护、帧节拍与垂直同步、popups 摆位与越界修正、wlr-layer-shell 排版与独占区扣除、KMS 硬件光标平面加速、截屏、锁屏与输入法数据路由等），而将**窗口管理的核心策略**（窗口摆放、叠放次序、尺寸仲裁、键盘/指针焦点、快捷键逻辑与工作区策略）交由 Rust 编写。

---

## 目录

1. [生命周期与并发模型](#1-生命周期与并发模型)
2. [初始化与配置 (`Config` / `Backend`)](#2-初始化与配置-config--backend)
3. [事件驱动模型 (`Event`)](#3-事件驱动模型-event)
4. [即时模式场景 (`Item`)](#4-即时模式场景-item)
5. [窗口控制与属性查询](#5-窗口控制与属性查询)
6. [快捷键与输入设备管理](#6-快捷键与输入设备管理)
7. [屏幕与多输出管理](#7-屏幕与多输出管理)
8. [多工作区与锁屏](#8-多工作区与锁屏)
9. [外部事件循环集成 (`AsRawFd`)](#9-外部事件循环集成-asrawfd)
10. [完整开发示例](#10-完整开发示例)

---

## 1. 生命周期与并发模型

- **单线程模型**：Wayland 协议及底层 `libwayland-server` 严格单线程运行。`Server` 未实现 `Send` 与 `Sync`，所有操作应在同一线程中执行。
- **无重入保障（No Re-entrancy）**：C++ 侧处理协议请求时不直接回调 Rust 闭包，而是将事件写入内部队列。Rust 仅在 `dispatch()` 执行完成退出 C++ 栈帧后通过 `next_event()` 消费队列。因此，在事件循环和处理回调中可以直接安全地借用 `&mut Server`，不存在多层重入或别名悬垂问题。
- **对象代际标识符**：`WindowId`、`OutputId` 为单调自增、永不复用的 64 位整数。当窗口或显示器销毁后，旧 ID 自动失效，以此 ID 调用任何 `Server` 方法均为安全空操作（No-op），不会引发非法内存访问。

---

## 2. 初始化与配置 (`Config` / `Backend`)

### `Config`
定义混成器启动时的参数：

```rust
pub struct Config {
    /// 目标后端，详见下方说明
    pub backend: Backend,
    /// 窗口或无头输出的默认像素宽度（DRM 模式下忽略，以显示器原生模式为准）
    pub width: i32,
    /// 窗口或无头输出的默认像素高度
    pub height: i32,
    /// 是否强制提示客户端使用服务端装饰（SSD），此时客户端不绘制自身标题栏与阴影
    pub server_side_decorations: bool,
    /// DRM 模式下绑定的显卡设备节点路径（例如 Some("/dev/dri/card1".into())），None 则自动选择
    pub drm_device: Option<String>,
}
```

### `Backend`
指定像素渲染的目标平台：
- `Backend::Auto`（默认）：依次探测 `Nested`（若存在 `WAYLAND_DISPLAY`）→ `Drm`（若处于独立 VT 且能获取 libseat 会话）→ `Headless`。
- `Backend::Nested`：作为客户端嵌入宿主 Wayland 混成器中，用于日常开发与调试。
- `Backend::Headless`：纯无头软件渲染，不开启任何物理或窗口输出，适用于 CI 自动化测试。
- `Backend::Drm`：裸机 KMS/DRM 输出与 libinput 输入，接管物理显示器与外设，支持硬件光标平面与 Ctrl+Alt+F1..F12 切 VT。

### 构造 `Server`
```rust
use luminaria::{Config, Server};

let server = Server::new(Config::default())?;
println!("WAYLAND_DISPLAY={}", server.socket_name());
```

---

## 3. 事件驱动模型 (`Event`)

混成器通过拉取事件感知客户端请求与外部硬件状态：

```rust
// 阻塞等待并派发事件循环（timeout 为 None 表示一直阻塞）
server.dispatch(Some(Duration::from_millis(16)))?;

// 排空消费事件队列
while let Some(event) = server.next_event() {
    match event {
        Event::WindowMap(w) => { /* 窗口已有内容，准备绘制 */ }
        Event::CloseRequested => break,
        _ => {}
    }
}
```

或使用开箱即用的 `server.run(...)` 辅助循环：
```rust
server.run(|server, event| {
    if event == Event::CloseRequested {
        std::ops::ControlFlow::Break(())
    } else {
        std::ops::ControlFlow::Continue(())
    }
})?;
```

### 事件清单

| 事件枚举项 | 说明与处理建议 |
|---|---|
| `WindowNew(WindowId)` | 客户端新建了 toplevel，此时尚无内容，通常先调用 `configure` 预置尺寸 |
| `WindowMap(WindowId)` | 窗口已附着 buffer 并提交，可以加入渲染场景 |
| `WindowUnmap(WindowId)` | 窗口隐藏，应从场景中移出，若持有键盘焦点已被系统自动收回 |
| `WindowDestroy(WindowId)` | 窗口彻底销毁，句柄已失效，应清理业务状态 |
| `WindowTitle(WindowId)` | 窗口标题或 AppId 变更，可通过 `server.title(w)` / `server.app_id(w)` 读取 |
| `RequestMaximize { window, on }` | 客户端请求（取消）最大化，可裁决是否调用 `server.set_maximized(window, on)` |
| `RequestFullscreen { window, on }` | 客户端请求（取消）全屏，可裁决是否调用 `server.set_fullscreen(window, on)` |
| `RequestMinimize { window, on }` | 客户端请求最小化，可将其移出场景并调用 `server.set_minimized(window, on)` |
| `RequestActivate(WindowId)` | 任务栏点击或跨应用 xdg-activation 令牌要求激活窗口，通常将其聚焦并置顶 |
| `RequestMove(WindowId)` | 客户端拖拽标题栏请求交互式移动，可开启 `server.grab_pointer(true)` |
| `RequestResize { window, edges }` | 客户端拖拽边缘请求交互式调整大小，`edges` 包含边缘掩码 |
| `PointerMotion { window, x, y }` | 指针绝对移动事件，附带全局逻辑坐标与命中测试窗口 |
| `PointerButton { window, button, pressed, x, y }` | 按键点击事件，已按协议自动路由，副本供做点击聚焦或拖拽判定 |
| `TouchDown / TouchMotion / TouchUp` | 触摸屏触点事件，包含触点 slot ID、全局坐标与按压表面 |
| `Binding(BindingId)` | 预绑定的全局快捷键触发，该按键事件不会泄露给获得焦点的客户端 |
| `OutputNew { output, x, y, width, height }` | 检测到新接入的显示屏（热插拔或启动时发现） |
| `OutputDestroy(OutputId)` | 显示器拔出或销毁 |
| `OutputChange(OutputId)` | 输出模式、缩放比例、朝向或 layer-shell 独占区变更，应重读 `output_usable()` |
| `CloseRequested` | 嵌套窗口的关闭按钮被点击 |
| `SessionActive { active }` | 裸机 DRM 下 VT 被切走 (`false`) 或切回 (`true`) |
| `SessionLock { locked }` | 屏幕被锁定 (`true`) 或解锁 (`false`) |
| `WorkspaceActivate(WorkspaceId)` | 外部 pager/dock 请求切换至该工作区 |

---

## 4. 即时模式场景 (`Item`)

混成器遵循即时模式（Immediate-Mode，ADR 0001）：**窗口管理器不需要维护长期的场景保留树**。

每次窗口移动、缩放或叠放顺序调整后，只需构建并提交一份完整的场景项列表，底层渲染引擎会自动对前后两次列表进行差分，仅重绘变动矩形与损伤区域：

```rust
use luminaria::{Color, Item, WindowId};

let mut scene = Vec::new();

// 1. 放置底图或装饰矩形
scene.push(Item::Rect {
    x: 0,
    y: 0,
    width: 1920,
    height: 1080,
    color: Color::rgb(0.1, 0.1, 0.1),
});

// 2. 按从下到上的 Z 序添加窗口
for (idx, &w) in windows.iter().enumerate() {
    scene.push(Item::Window {
        id: w,
        x: 40 + (idx as i32) * 30,
        y: 40 + (idx as i32) * 30,
    });
}

// 3. 提交场景
server.set_scene(&scene);
```

### `Item` 类型说明
- `Item::Window { id, x, y }`：客户端窗口，`(x, y)` 为其有效内容几何的左上角（不计客户端自带外阴影），子菜单 popup 会自动贴合跟随。同时承担指针命中测试的几何体。
- `Item::Rect { x, y, width, height, color }`：纯色填充矩形，不参与窗口命中测试。
- `Item::Border { x, y, width, height, thickness, color }`：在矩形边框内侧绘制厚度为 `thickness` 的边框线，常用于当前聚焦窗口的高亮轮廓。

---

## 5. 窗口控制与属性查询

### 焦点与尺寸
- `server.focus(Some(w))` / `server.focus(None)`：设置当前拥有键盘输入焦点的窗口。
- `server.configure(w, width, height)`：向客户端发送建议尺寸。传 `(0, 0)` 则由客户端自决尺寸。

### 状态标注
- `server.set_maximized(w, true)` / `server.set_fullscreen(w, true)`：告知客户端已最大化或全屏。
- `server.set_tiled(w, true)`：告知客户端边缘贴合（平铺），使终端与编辑器消除自带圆角与阴影。
- `server.set_resizing(w, true)`：告知客户端当前处于交互式调整大小过程中，优化绘制性能。
- `server.set_minimized(w, true)`：状态标记（需配合自行从 `set_scene` 中排除）。
- `server.close(w)`：礼貌向客户端发送关闭请求（`xdg_toplevel.close`）。

### 属性读取
- `server.title(w) -> String`：读取当前窗口标题。
- `server.app_id(w) -> String`：读取应用 ID（如 `org.wezfurlong.wezterm`）。
- `server.icon_name(w) -> String`：读取 `xdg-toplevel-icon` 提供的图标名。
- `server.size(w) -> (i32, i32)`：当前已提交内容的像素宽高。
- `server.min_size(w) -> (i32, i32)`：客户端声明的最小限制尺寸。
- `server.parent(w) -> Option<WindowId>`：读取瞬态父窗口（如果是对话框）。

---

## 6. 快捷键与输入设备管理

### 全局按键绑定 (`bind` / `bind_key`)
使用直观的字符串规范绑定快捷键，返回 `BindingId`。当按下时触发 `Event::Binding(id)`：

```rust
let spawn = server.bind("Logo+Return")?;
let close = server.bind("Logo+q")?;
let lock  = server.bind("Ctrl+Alt+l")?;
```
支持的修饰键前缀：`Shift`、`Ctrl` / `Control`、`Alt`、`Logo` / `Super` / `Mod4`。

### 输入法与指针抓取
- `server.grab_pointer(true)`：截断向客户端分发指针移动事件。用于交互式拖动标题栏或拉伸窗口边框时，避免指针划过其它窗口引发悬停反馈。
- `server.set_keyboard_layout("us,ru")`：在裸机后端切换系统 XKB 键盘布局。

---

## 7. 屏幕与多输出管理

- `server.output_box(output_id) -> Rect`：输出在全局逻辑坐标系中的整体边界盒。
- `server.output_usable(output_id) -> Rect`：输出扣除 layer-shell 任务栏/面板（独占区）后的可用工作区区域。
- `server.output_modes(output_id) -> Vec<Mode>`：获取显示器支持的硬件显示模式列表。
- `server.set_output_mode(output_id, mode) -> Result<(), Error>`：切换分辨率与刷新率。
- `server.set_output_scale(output_id, scale)`：设置 HiDPI 缩放倍率。
- `server.set_output_transform(output_id, transform)`：设置屏幕朝向（如 `Transform::Rotate90`）。
- `server.set_output_position(output_id, x, y)`：调整多显示器拼接排列的原点位置。

---

## 8. 多工作区与锁屏

### 工作区 (`ext-workspace`)
向系统任务栏/桌面分页器发布工作区：
```rust
let ws1 = server.add_workspace("1");
let ws2 = server.add_workspace("2");

server.activate_workspace(ws1); // 标记激活
server.remove_workspace(ws2);
```

### 屏幕锁 (`ext-session-lock`)
`server.is_locked() -> bool` 用于检测是否已被锁屏程序接管。锁定期间仅锁屏界面可以渲染并接受输入，所有全局快捷键均失效（Fail-Closed 设计）。

---

## 9. 外部事件循环集成 (`AsRawFd`)

`Server` 实现了 `std::os::fd::AsRawFd`，返回其底层 Wayland 事件循环的文件描述符。可以无缝嵌入 `epoll`、`mio`、`calloop` 或任何异步运行时：

```rust
use std::os::fd::AsRawFd;
use std::time::Duration;

let fd = server.as_raw_fd();
// 将 fd 注册为可读监听...

// 当 fd 就绪时，非阻塞派发：
server.dispatch(Some(Duration::ZERO))?;
while let Some(event) = server.next_event() {
    // 处理事件...
}
```

---

## 10. 完整开发示例

一个最小完整的平铺窗口管理器示例：

```rust
use std::ops::ControlFlow;
use luminaria::{Backend, Config, Event, Item, Rect, Server, WindowId};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut server = Server::new(Config::default())?;
    let terminal_cmd = server.bind("Logo+Return")?;
    let close_cmd    = server.bind("Logo+q")?;

    let mut usable = Rect::new(0, 0, 1280, 800);
    let mut stack: Vec<WindowId> = Vec::new();

    server.run(|server, event| {
        match event {
            Event::OutputNew { output, .. } | Event::OutputChange(output) => {
                usable = server.output_usable(output);
                for &w in &stack {
                    server.configure(w, usable.width, usable.height);
                }
            }
            Event::WindowNew(w) => {
                server.set_maximized(w, true);
                server.configure(w, usable.width, usable.height);
            }
            Event::WindowMap(w) => {
                stack.push(w);
                server.focus(Some(w));
            }
            Event::WindowUnmap(w) | Event::WindowDestroy(w) => {
                stack.retain(|&id| id != w);
            }
            Event::Binding(b) if b == close_cmd => {
                if let Some(&top) = stack.last() {
                    server.close(top);
                }
            }
            Event::CloseRequested => return ControlFlow::Break(()),
            _ => {}
        }

        // 重组场景：铺满可用区域
        let scene: Vec<Item> = stack.iter().map(|&id| Item::Window {
            id,
            x: usable.x,
            y: usable.y,
        }).collect();

        server.set_scene(&scene);
        ControlFlow::Continue(())
    })?;

    Ok(())
}
```
