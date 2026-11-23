# 协议层

## 线协议（`luminaria_wire`）

不依赖 libwayland，用 Mica 实现 Wayland 线协议，能和 libwayland 客户端互通。

- **接收**：每个连接一个 8 KiB 的固定缓冲，一次 `recvmsg` 填满，最多同时收 64 个 fd。消息在缓冲里
  原地解码：数字直接读，只有字符串和数组拷贝。
- **发送**：事件编码进一个发送缓冲，`flush` 一次交给内核；超过 28 个 fd 时按 libwayland 的上限分批，
  每个 fd 不晚于它的消息到达。
- **稳态不分配**：缓冲和对象表复用。
- **内存有界**：对端不读，积压超过 1 MiB 就断开。
- **对象表**：稠密数组，每项 8 字节。客户端的新 id 只能是下一个或已释放的，不能让表无限增长。
  析构请求自动释放 id 并发 `delete_id`；发给已销毁对象的事件在客户端一侧被解码后丢弃。
- **检查**：参数的接口、空值、长度、`since` 版本都检查，失败发 `wl_display.error` 并断开。
  高于对象版本的事件不发。

## 恶意客户端

- **数值**：坐标、尺寸、偏移、外边距、独占区在请求入口用 `ids.clamp` / `ids.rect` 夹到 ±2^24 px，此后
  几个值相加也不出 i32；`u32` 尺寸先截到 `ids.limit` 再转 `i32`；越界的时间戳、shm 缓冲（行距按 64 位
  比较）、池大小按协议报错。
- **fd**：解码失败或断开时收到的 fd 全部关掉；服务端长期持有的 fd 先验证类型（沙盒监听 fd 用
  `SO_ACCEPTCONN` 确认在监听，否则 `invalid_listen_fd`）。
- **模糊测试**（`luminaria_server/tests/fuzz.mica`）：读 `protocols/*.xml` 得到每个请求的签名，带种子地给
  活着的对象发随机请求：参数取边界值、同接口的对象、新 id、字符串、数组、memfd，缺的对象先按签名造出来，
  偶尔夹杂乱码；另有破坏帧格式的随机字节。要求服务端不崩溃、断开后 fd 数回到原值、之后正常客户端照常
  工作。`FUZZ_SEED`、`FUZZ_ROUNDS` 调种子和客户端数；整数溢出在 debug 构建里 panic，所以长跑用 debug。

## 生成的接口（`luminaria_proto`）

`luminaria_scanner` 从 `protocols/*.xml` 生成，每个协议文件一个模块：

- 类型化 id（`wl.Surface`，`derive(Hash, Show)`）
- 请求枚举，用 `proto.next` 读
- 事件发送函数，是类型化 id 上的方法；析构事件自动释放 id
- 协议枚举，位域用 `Flags`
- 客户端一侧也生成（`proto.next_event`、请求方法），给嵌套后端和测试用；不调用就被链接器剔除

## 服务端状态（`luminaria_server`）

没有 GPU，也没有策略。需要决定的请求以事件出来，合成器通过方法回应。

### 存储

- 每类对象一个 slab：稳定槽位加代数（奇数表示使用中）。对外句柄是 `(index, gen)`，失效的句柄不会
  指到复用了槽位的新对象。
- 每个客户端一张表：协议 id → 对应种类 slab 的槽位。请求进来时先查表，再按接口分派到各模块。
- 客户端断开时，按 id 从大到小销毁它的所有对象（子对象先于父对象）。

### 提交与事务

每个表面有三份双缓冲状态：`pending`（请求写入）、`cached`（同步子表面暂存）、`current`（生效）。

1. `wl_surface.commit` 把 `pending` 快照下来。
2. 表面自己或任一祖先是同步子表面：并入 `cached`，等父表面提交时一起带走。
3. 否则组成一个**事务**：这次的状态，加上所有同步子孙的 `cached` 状态。
4. 事务需要等待时发 `Committed(txn)`，合成器（门面）解决 `waits(txn)` 后调 `ready(txn)`；不需要等待且
   该客户端没有排队的事务时立即生效。
5. 同一客户端的事务按顺序生效。慢栅栏只挡住这个客户端自己，挡不住别人。

等待的种类：

| `Wait` | 含义 | 门面怎么等 |
|---|---|---|
| `Implicit(buffer, fd)` | dmabuf 的隐式写栅栏 | 导出成 sync_file 放进 epoll |
| `Timeline(fd, point)` | linux-drm-syncobj 获取点 | 转成 eventfd 放进 epoll |
| `Fifo(surface)` | wp_fifo 屏障还没被呈现解除 | 等该表面下一次被显示（不可见的表面不等） |
| `Time(ns)` | wp_commit_timing 的目标时间 | 定时器 |

事务生效时：换缓冲（更新使用计数）、合并损坏（超过 16 个矩形就合成外接矩形）、更新区域、变换、
缩放、视口、帧回调、呈现反馈（之前未呈现的反馈发 `discarded`）、fifo 屏障、tearing 提示、
xdg 几何与尺寸限制、子表面位置与叠放，然后重算表面尺寸并发 `Applied(surface)`。门面用
`take_changes` 取走变化位和损坏。

### xdg-shell

| 时机 | 事件 |
|---|---|
| 初始提交（标题和 app id 已知） | `WindowNew` / `PopupNew` |
| ack 了 configure 后的第一个带缓冲的提交 | `WindowMapped` / `PopupMapped` |
| 每次 ack 的 configure 随提交生效 | `WindowAcked(w, width, height)` |
| 提交空缓冲 | `WindowUnmapped`（回到需要重新初始提交的状态） |
| 销毁 | `WindowClosed` / `PopupGone` |
| move、resize、maximize、fullscreen、minimize、window menu | `WindowRequest` |

还没 ack configure 就提交缓冲、给已有角色的表面再给角色等，都按协议报错。弹出菜单位置由 positioner
计算：锚点矩形、锚边、重力、偏移，超出约束矩形时按客户端允许的顺序翻转、滑动、缩小。

### seat

- 每个 seat 记住键盘焦点、指针焦点、`enter_serial`、修饰键、快捷键抑制、空闲时间 (`input_activity_on`)
  和触点所在的表面；支持多 seat 增删（`add_seat`、`remove_seat`）；事件只发给焦点客户端的对象。
- 键盘：每个 seat 维护共享 keymap memfd；enter 带已按下的键；修饰键变化才发 `modifiers`。
- 指针：enter / leave 配对；每组事件后发 `frame`（v5+）；滚轮按版本发 `axis_value120`（v8+）或
  `axis_discrete`，手指滚动结束发 `axis_stop`。
- `set_cursor` 和 cursor-shape 的 `set_shape` 只接受指针所在客户端、带该 seat 最近 `enter_serial` 的请求，以
  `Cursor` / `CursorShape` 事件交给门面；形状按 CSS 名字（`text`、`pointer`、`ew-resize` ……）取主题光标。
- 指针扩展只发给指针焦点客户端：相对运动（加速前后两组增量，微秒时间戳）、滑动 / 捏合 / 长按手势
  （开始时的焦点表面收到整组）、指针锁定与限定（锁定时指针不动只有相对运动；限定时夹在区域内；只在
  该客户端有键盘焦点、指针进入区域时生效，oneshot 的失效后不再生效，锁定结束按客户端给的位置提示放
  回光标）。
- serial 全局递增。

### 输入法

- text-input-v3：键盘焦点客户端的文本框收到 enter / leave；`enable` + `commit` 激活输入法，之后每次
  `commit` 把周围文本、改动原因、内容类型转给输入法并 `done`。
- input-method-v2：每个 seat 一个输入法（第二个收到 `unavailable`）。它的 `commit` 把预编辑、提交文本
  和删除一并发给当前文本框，`done` 带文本框的提交计数。键盘抓取后硬件按键先给输入法。候选窗是
  `zwp_input_popup_surface_v2`，门面把它放在文本光标下方，并告诉它光标矩形的位置。
- virtual-keyboard-v1：输入法转发的按键、屏幕键盘、wtype 直接发给键盘焦点（越过抓取）。它自带
  keymap；内容和焦点客户端已有的不同才重发（按 FNV 哈希比较），硬件按键到来时换回 seat 的 keymap。

### 选区

剪贴板（`wl_data_device_manager`）和主选区（primary selection，中键粘贴）按 seat 各存一个源。键盘焦点
客户端收到 offer；它要哪种 MIME 就把管道 fd 直接转给源客户端，数据不经过合成器。ext-data-control
（剪贴板管理器、wl-copy / wl-paste）不需要焦点，看得到也能设置两种选区。

拖放：`start_drag` 只在指针按着键、停在起点表面上时成立，否则源被取消。拖动期间起点收到指针
`leave`，指针下的表面改收 `wl_data_device` 的 enter（带新 offer、源的 MIME 和动作）/ motion / leave；
没有源的拖动只在本客户端内。目标 `accept` 一种类型、`set_actions` 给出动作后，按目标偏好、否则复制 /
移动 / 询问的顺序选定动作，源和目标都收到。松开最后一个键时目标接受了就 `drop` 加
`dnd_drop_performed`，否则取消；之后指针重新进入它下面的表面，目标 `finish` 时源收到
`dnd_finished`。拖动图标是 `DragIcon` 角色，不接收输入，门面把它画在指针处、一切之上。

### 桌面外壳

- **layer-shell（v5）**：面板、壁纸、启动器、通知。锚定边、尺寸、外边距、独占区、键盘交互、层（背景 /
  底 / 顶 / 覆盖）都是随提交生效的双缓冲状态。首次提交发 `LayerNew`，此后映射、取消映射、状态变化发
  `LayerChanged`；`layer(l)` 查状态，`configure_layer`、`close_layer` 回应。0 尺寸必须锚定两侧对边，
  ack 之前带缓冲提交是协议错误。layer 表面的弹出菜单（`get_popup`）以它为父表面。
- **ext-session-lock**：同一时间只有一个锁屏客户端，第二个收到 `finished`。`LockRequest` 后门面隐藏
  一切，`lock_done` 才发 `locked`；锁屏客户端死掉时会话保持锁定，下一个锁屏客户端接管。每个输出一个锁屏
  表面（`LockSurfaceNew` 后按输出尺寸 configure）。

### 窗口扩展

| 协议 | 作用 |
|---|---|
| xdg-decoration | 客户端想要的模式（`decoration(w)`、`WindowDecoration`）；`set_decoration` 决定，每次 configure 都先发装饰模式 |
| xdg-toplevel-icon | 图标名和图像（`set_icon` 时拷贝 ARGB8888 像素，客户端可以复用缓冲） |
| xdg-toplevel-tag | 跨重启不变的窗口标签和描述 |
| xdg-dialog | 模态对话框标记 |
| xdg-activation | 令牌（最多保留 64 个，用一次即失效）；有效令牌的 `activate` 发 `Activate`；合成器启动程序时可用 `activation_token` |
| xdg-foreign v2 | 导出窗口得到句柄，别的客户端（门户）导入后设为自己窗口的父窗口 |
| ext-foreign-toplevel-list | 任务栏看到每个已映射窗口的标识、标题、app id 及其变化和关闭 |
| xdg-system-bell | `Bell` 事件 |

### 输出

每个输出一个 `wl_output` global，绑定时发几何、模式、整数缩放（向上取整，客户端缩小）、名字、描述和
`done`，变化时重发。表面所在输出变化时发 enter / leave。首选缩放用 wp_fractional_scale（客户端要了
的话）和 `wl_surface` v6 的提示。

### 扩展协议

| 协议 | 版本 | 作用 |
|---|---|---|
| linux-dmabuf | 4 | 共享格式表（sealed memfd）、主设备、扫描输出分组优先；支持 1..4 平面、乱序 `add` 与 `PlaneSet`/修饰符校验 |
| linux-drm-syncobj | 1 | 获取点与释放点（GPU 支持时间线 syncobj 才公布） |
| presentation-time | 1 | CLOCK_MONOTONIC 时间戳、刷新间隔、vblank 序号、零拷贝标志 |
| viewporter | 1 | 源矩形与目标尺寸 |
| fractional-scale | 1 | 首选缩放（1/120） |
| fifo | 1 | 屏障与等待 |
| commit-timing | 1 | 目标呈现时间 |
| tearing-control | 1 | 允许撕裂（直接扫描输出时异步翻转） |
| single-pixel-buffer | 1 | 一个像素的缓冲（纯色背景、遮罩），配合 viewporter 拉伸 |
| alpha-modifier | 1 | 表面整体透明度（乘到放置的不透明度上，不透明区域随之失效） |
| content-type | 1 | 内容类型：照片、视频、游戏 |
| xdg-output | 3 | 逻辑位置与尺寸（缩放后）、名字、描述 |
| idle-inhibit | 1 | 表面可见时阻止空闲（视频播放） |
| ext-idle-notify | 2 | 空闲与恢复通知；普通通知被抑制器挡住，input-idle 通知不受影响 |
| keyboard-shortcuts-inhibit | 1 | 焦点表面要求拿到合成器快捷键（远程桌面、虚拟机） |
| ext-image-capture-source | 1 | 输出源与顶层窗口（foreign toplevel）作为截屏源 |
| ext-image-copy-capture | 1 | 截屏与录屏，见下 |
| wlr-output-management | 4 | 输出配置，见下 |
| wlr-virtual-pointer | 2 | 虚拟指针（相对/绝对移动、按键、滚轮，特权 global） |
| wlr-gamma-control | 1 | 独占式硬件 CRTC Gamma LUT（护眼色温，特权 global） |
| wp-color-management | 1 | 色彩空间描述与 HDR（sRGB、BT.2020 PQ/HLG、Windows scRGB，输出与表面反馈） |
| security-context | 1 | 沙盒，见下 |
| wl_fixes | 1 | 销毁 registry |

核心协议的版本：`wl_compositor` 6、`wl_subcompositor` 1、`wl_shm` 2（ARGB/XRGB/ABGR/XBGR8888）、
`wl_data_device_manager` 3、`wl_seat` 9、`wl_output` 4、`xdg_wm_base` 6。

析构请求的 id 由解码层释放。处理函数没有清理的对象，在本次分派后按断开连接时的路径拆除，所以复用同一
id 或同一客户端槽位的新对象不会继承旧记录。

### 运行时扩展

`ext.mica`：合成器用 `Interface`（名字、版本、请求和事件的签名，同 libwayland 的 `wl_message`）描述
一个服务端不懂的接口，`extend` 给它分配一个排在生成接口之后的编号，写进每个连接的名字表
（`Conn.names`，新连接也带上）；`global` 时进注册表。生成的 `wl_registry.bind` 解码时先查生成的
名字、再查这张表；`proto.next` 把编号超出生成范围的请求原样交出（`Request.Extension(msg)`），由
`ext.request` 按签名解码（新对象登记成签名里写明的接口，fd 交给合成器），发 `ExtRequest`；析构请求
处理后释放 id。`ext_send` 按事件签名编码。只有扩展消息走这条路径，生成的协议代码不受影响。

### 截屏与录屏

ext-image-capture-source 支持输出源（`ext_output_image_capture_source_manager_v1`）与顶层窗口源
（`ext_foreign_toplevel_image_capture_source_manager_v1`）；ext-image-copy-capture 的会话公布源的
像素尺寸、shm 格式（ARGB8888 / XRGB8888）以及渲染器支持的 `dmabuf_device` 与 `dmabuf_format` 列表。

- **输出源**：会话第一帧立即触发 `Capture(o)`，之后的帧等输出有新内容；支持 shm 读回与 GPU `copy_dmabuf`
  零拷贝复制，等候输出截屏时跳过直接扫描输出和硬件 overlay 提升。
- **窗口源**：窗口几何或映射变化触发 `CaptureWindow(w)` / `window_changed` / `window_gone`；
  由 `render_offscreen`（shm 逐行拷入）或 `render_offscreen_dmabuf`（dmabuf 离屏渲染/复制）单独绘制
  窗口及其子表面树，不改动主输出场景。
- **异步完成**：dmabuf 截屏标记缓冲忙并发起 GPU 提交后把 `sync_file` 放进 epoll，就绪后才调用
  `capture_done`（发 transform、damage、呈现时间与 `ready`）并经 `release` 归还 `wl_buffer`；
  尺寸、格式不匹配或目标消失分别发 `failed(buffer_constraints)` / `stopped`。硬件光标不强制包含在
  单独会话里；光标会话直接 `stopped`。

### 输出配置

wlr-output-management v4：每个绑定的管理器为每个输出建一个 head，带名字、描述、物理尺寸、厂商型号、
模式列表（`OutputInfo.modes`，后端没给就用当前模式）、启用、当前模式、位置、变换、缩放和自适应同步，
再发 `done(serial)`。输出的增删改都让 serial 加一并重发。按旧 serial 建的配置 `cancelled`，漏配 head
是协议错误。`apply` / `test` 发 `OutputConfig(c, test)`，`output_config(c)` 读每个输出要的启用、模式、
位置、变换、缩放、VRR，合成器用 `output_config_done(c, ok)` 回答。

### 沙盒

security-context：沙盒（Flatpak）交来一个监听 socket 和一个关闭 fd，`commit` 后发 `Listen(k)`，
门面监听它，从这里连进来的客户端带上沙盒引擎、app id、实例 id（`sandbox(c)`）；关闭 fd 挂断时停止
接受，已连上的客户端不受影响。监听 fd 必须是在监听的 socket。沙盒客户端看不到也绑不了特权 global：security-context 本身（不能嵌套）、
data-control、layer-shell、session-lock、input-method、virtual-keyboard、virtual-pointer、gamma-control、keyboard-shortcuts-inhibit、
foreign-toplevel-list、截屏和输出配置。
