# luminaria

[English](README.md)

用 [Mica](https://github.com/antinomie1/mica) 写 Wayland 合成器的库，用
[sheaf](https://github.com/antinomie1/sheaf) 构建。它提供机制：协议状态、合成和设备。策略和主循环
归合成器自己。设计见 [docs/](docs/README.md)。

| 包 | 作用 |
|----|------|
| `luminaria` | `Display`：一个事件循环接起下面所有的包，加上布局、输入路由、操纵 |
| `luminaria_server` | 协议状态：表面、事务、xdg-shell、layer shell、锁屏、seat 与输入法、选区与拖放、输出及其配置、截屏、沙盒、缓冲、时序；不碰 GPU |
| `luminaria_compose` | 基于 `vk_compose` 的合成：放置表比对、客户端缓冲的纹理（归它所有）、栅栏、时钟 |
| `luminaria_backend` | DRM/KMS、嵌套、无头三种输出；libseat、libinput、xkb |
| `luminaria_proto` | 由 `protocols/*.xml` 生成的接口 |
| `luminaria_wire` | 线协议：socket、fd 传递、编解码、对象表 |
| `luminaria_scanner` | 生成 `luminaria_proto`（开发工具） |

```
luminaria ─┬─ luminaria_server ─── luminaria_proto ── luminaria_wire
           ├─ luminaria_compose ── vk_compose, vk_draw2d, vk_gpu (github.com/antinomie1/vk)
           └─ luminaria_backend ── vk_gpu, sys/*；luminaria_proto（仅嵌套）
```

构建特性：`nested`（默认开启）是嵌套后端，`luminaria_backend` 只因它才依赖 `luminaria_proto` 和
`luminaria_wire`。关掉它（`--no-default-features`）后端只有 DRM 和无头两种输出，`Kind.Auto` 总选 DRM。

`protocols/` 是 wayland 1.26、wayland-protocols 1.50 和 wlr-protocols 的 XML。服务端支持：

| 方面 | 协议 |
|------|------|
| 核心与外壳 | wl_compositor 6、subcompositor、shm、wl_seat 9、wl_output 4、xdg-shell 6、wl_fixes |
| 缓冲与时序 | linux-dmabuf 4、linux-drm-syncobj、single-pixel-buffer、viewporter、fractional-scale、presentation-time、fifo、commit-timing、tearing-control、alpha-modifier、content-type |
| 窗口 | xdg-decoration、xdg-activation、xdg-toplevel-icon、xdg-toplevel-tag、xdg-dialog、xdg-foreign v2、xdg-system-bell、ext-foreign-toplevel-list |
| 桌面 | wlr-layer-shell 5、ext-session-lock、xdg-output 3、ext-idle-notify 2、idle-inhibit |
| 输入 | relative-pointer、pointer-constraints、pointer-gestures 3、cursor-shape 2、keyboard-shortcuts-inhibit、text-input-v3、input-method-v2、virtual-keyboard |
| 选区 | 剪贴板、主选区、ext-data-control、带动作协商的拖放 |
| 截屏与管控 | ext-image-copy-capture（输出源，shm）、wlr-output-management 4、security-context（沙盒客户端看不到特权 global） |

尚未支持：数位板、dmabuf 截屏和窗口截屏。

## 写一个合成器

```mica
import luminaria as lum

var d = try lum.Display.open(lum.Options {})   // DRM，或在 WAYLAND_DISPLAY 下嵌套
var wins: List[lum.Window] = []
while true {
    guard let Some(e) = try d.next() else {
        break
    }
    match e {
        WindowMapped(w) => {
            wins.push(w)
            d.focus(Some(w))
            d.layout(fn(mut l) {
                for i, x in wins {
                    l.window(x, f64(40 * i), f64(40 * i), lum.Style { radius: 8.0, shadow: 16.0, transition_ms: 150 })
                }
            })
        }
        Key(k) => {
            if k.pressed and k.mods.logo and k.sym == lum.backend.keysym.key_q {
                break
            }
            d.accept(e)
        }
        _ => d.accept(e)
    }
}
```

- **一个循环，没有回调。** `next` 以值的形式返回事件。队列空了，它先完成到期的帧、给每个客户端
  flush（每个一次 `sendmsg`），再睡在 epoll 上；空闲的桌面不挂任何定时器。合成器自己的 fd 和定时器
  也进这个循环（`watch`、`timer`）；`fd` 和 `poll` 可以把它嵌进别的循环。
- **`accept` 是默认动作。** 输入发给指针下的表面（命中测试与隐式抓取）或键盘焦点；新窗口得到
  configure；最大化和全屏请求在窗口所在的输出上批准；移动和缩放在库内进行；弹出菜单按 positioner
  定位、点外面就关；被遮住的窗口收到 suspended；Ctrl+Alt+Fn 切 VT。想改的自己处理，其余交给它。
- **是布局，不是场景树。** `layout` 接收整张放置表（窗口连同子表面和弹出菜单、合成器自己的图像）；
  每个输出拿到它显示的部分，只重绘变化的部分。`transition_ms` 让库把放置动画到新位置。
- **操纵按输入速率进行。** `manipulate_move`、`manipulate_resize` 和 `manipulate_pan`（滑动手势，
  带惯性）在每个指针或手势事件上移动窗口，不经过合成器往返；光标平面立即移动。`Moved`、`Resized`、
  `Panned` 报告结果。
- **事务。** `begin` 和 `end` 之间的 configure 与其间给出的布局一起显示：等每个窗口按新尺寸画好，或
  到超时为止按已到的显示。
- **外壳自己运转。** 面板、壁纸、启动器（layer shell）排在布局周围，`usable` 是它们留下的区域。锁屏
  立刻隐藏一切，每个输出出完一帧不含会话内容的画面后才告诉锁屏客户端 `locked`。输入法候选窗跟着文本
  光标；指针锁定与限定、相对运动、手势、空闲通知、剪贴板管理器都不需要合成器写代码。

[`examples/tiny`](examples/tiny/src/main.mica) 是一个完整的堆叠式合成器，约 250 行：新窗口居中，
点击聚焦并置顶，Alt+左键拖动移动、Alt+右键拖动缩放，Alt+F1 切换，Alt+Q 关闭，Alt+Escape 退出；
`-s` 在启动后运行一条命令。

```sh
sheaf run -p tiny -- -s foot    # 在 VT 上，或作为当前桌面里的一个窗口
```

### 出帧

- 每个输出有一个合成器时钟：帧在下一个 vblank 允许的范围内尽量晚开始（帧迟到后余量变大，再慢慢
  缩回）；可变刷新率时有东西要显示就开始。
- 提交在事件循环里等缓冲就绪，从不在渲染路径上等：dmabuf 的隐式栅栏导出成 sync_file，syncobj 时间点
  变成 eventfd，fifo 屏障和 commit-timing 目标等呈现。同一客户端的提交按顺序生效，慢客户端只挡住它
  自己。
- shm 提交被合并：每帧只上传一次、只传损坏矩形，然后释放；在这一帧之前被替换的缓冲不拷贝直接释放。
- 不缩放、不透明、正好铺满输出的客户端缓冲直接显示：原子测试后作为 KMS framebuffer（零拷贝，它的
  损坏作为 `FB_DAMAGE_CLIPS`），嵌套时直接交给父合成器。
- 合成出来的帧只重绘损坏、跳过被不透明窗口挡住的部分，把 GPU 的 sync_file 作为 `IN_FENCE_FD` 交给
  内核（CPU 从不等待），把变化的矩形作为 `FB_DAMAGE_CLIPS` 或父合成器的 damage 传下去。
- 帧回调和呈现反馈只发给这一帧显示了的表面；看不见的客户端停止绘制。

### 后端

| 后端 | 输出 | 输入 |
|------|------|------|
| DRM | 原子 KMS：主平面和光标平面、来自 `IN_FORMATS` 的修饰符、翻转时间戳、撕裂客户端的异步翻转、VRR、DPMS、热插拔 | libinput（经 libseat）、xkb |
| 嵌套 | 父合成器里的一个窗口（标题栏和边框经 xdg-decoration 由父合成器画），dmabuf 经 linux-dmabuf 交出，带隐式栅栏 | 父合成器的 seat 和 keymap |
| 无头 | 按模式刷新率定时的纹理 | `inject` |

release 构建，本机实测（`sheaf test --release`）：

| 项目 | 时间 |
|------|------|
| 1920x1080 一帧，50 个圆角带阴影的窗口、一个在动：CPU 录制并提交 | 8 µs |
| 同上，直到 GPU 完成 | 46 µs |
| 命中测试，512 个表面，最坏情况（整数 SIMD，一步 8 个） | 251 ns |

## 线协议与协议

`luminaria_wire` 用 Mica 实现 Wayland 线协议，不依赖 libwayland，能和 libwayland 客户端互通。

- **接收。** 一次 `recvmsg` 填满每个连接固定的 8 KiB 缓冲。消息原地解码：数字直接从缓冲读，只有
  字符串和数组会拷贝。
- **发送。** 事件编码进一个发送缓冲。`flush` 每次分派交给内核一次，除非排队的 fd 超过 28 个，否则
  只用一次 `sendmsg`；每个 fd 不晚于它的消息到达。
- **稳态不分配。** 缓冲和对象表复用。
- **内存有界。** 不读数据的对端在积压 1 MiB 后被断开。
- **每个合成器都需要的检查：**
  - 对象 id 是稠密表，每项 8 字节。客户端的新 id 只能是下一个或已释放的，不能让表无限增长。
  - 参数检查接口、空值、长度和 `since` 版本。失败时发 `wl_display.error` 并关闭客户端。
  - 析构自动释放 id 并发 `delete_id`。
  - 高于对象版本的事件被跳过。

`luminaria_proto` 由 `luminaria_scanner` 生成。每个协议文件是一个模块（`wayland`、`xdg_shell` ……），
包含：

- 类型化 id（`wl.Surface`）
- 请求枚举，用 `proto.next` 读取
- 事件发送函数，是类型化 id 上的方法
- 协议枚举，位域用 `Flags`

客户端一侧（`proto.next_event`、请求方法）也会生成，给嵌套后端和测试用。不调用它的合成器不带额外
代码：链接器会剔除。

```mica
while true {
    guard let Some(r) = try proto.next(mut conn) else {
        break
    }
    match r {
        WlDisplay(_, Sync(cb)) => cb.done(mut conn, serial)
        WlSurface(s, Attach(buffer, x, y)) => ...
        _ => {}
    }
}
discard try conn.flush()
```

release 构建，经 Unix socket，每次 flush 128 条消息：

| 方向 | 每条消息 |
|------|----------|
| 请求：编码、发送、接收、解码 | 约 60 ns |
| 事件（`wl_pointer.motion`） | 约 48 ns |

```sh
sheaf run -p luminaria_scanner -- protocols luminaria_proto/src   # 修改 protocols/ 之后
```

## 绑定

`sys/` 是合成器所依赖的 Linux 库的绑定。它们是通用的：任何 Mica 程序都可以依赖。

| 包 | 库 | 内容 |
|----|----|------|
| `xkbcommon_sys` | libxkbcommon 1.x | keymap、状态、compose、全部 2638 个 keysym |
| `libinput_sys` | libinput 1.x | path 与 udev 上下文、事件、设备配置 |
| `libseat_sys` | libseat | seat 与设备访问（seatd、logind、noop） |
| `libudev_sys` | libudev | 枚举、设备、netlink 监视、hwdb |
| `libdrm_sys` | libdrm | xf86drm、KMS（`xf86drmMode`）、内核 uapi 在 `libdrm_sys.uapi` |

由 `mica bind`（`sys/gen.sh`）生成，并做了这些修正：

- 名字去掉库前缀：`libinput_get_event` 是 `get_event`，`struct xkb_state` 是 `State`。结构体字段是
  `pub`，其他 bundle 可以构造和读取。
- keysym 保留大小写，因为 `bind` 会把 `XKB_KEY_a` 和 `XKB_KEY_A` 折叠成同一个名字。它们是单独模块里的
  纯常量：`xkb.keysym.key_a`、`xkb.keysym.key_A`，由 `luminaria_backend.keysym` 再导出。
- 由函数式宏构成的宏用 `cc` 求值。这覆盖了所有 `DRM_FORMAT_*` 和 `*_FORMAT_MOD_*` 常量，以及配合
  `uapi` 结构体做裸 `drm.ioctl` 调用所需的 `DRM_IOCTL_*` 编号。
- libdrm 自己的类型（`drmVersion`、`drmModeCrtc`）和内核的类型（`drm_version`、`drm_mode_crtc`）会
  映射成同样的名字，所以 uapi 头文件放在单独的模块里：`import libdrm_sys.uapi`。
- `libinput_sys` 使用 `libudev_sys` 的句柄，所以 `udev.Udev` 可以直接传给 `udev_create_context`，
  `device_get_udev_device` 返回 `udev.Device`。
- 日志回调接收 `va_list`，Mica 不支持。一个小的 C 文件格式化每条消息并去掉结尾换行再转交：
  `log_set_handler`（libinput）、`set_log_handler`（libseat）和 `context_set_log_fn`（xkbcommon）。
  每个进程一个回调。

回调是带函数体的 `extern fn`（参数里有 `Ptr` 时用 `extern unsafe fn`）：

```mica
import libseat_sys as seat

extern unsafe fn on_enable(s: seat.Libseat, data: Ptr[u8]) {
}

let l = seat.SeatListener { enable_seat: Some(on_enable) }
```

限制：可选的 `const char *` 参数是 `str`，不能传 NULL；字符串宏（`XKB_MOD_NAME_SHIFT`）不生成。

## 重新生成

```sh
MICA=/path/to/mica sys/gen.sh    # 默认 INC=/usr/include
```

## 测试

```sh
sheaf test    # luminaria：libwayland xdg 窗口（用 cc 编译）和 foot 在无头输出上：
              # configure、合成、读回、焦点、点击；server：提交、事务、toplevel、
              # 子表面、弹出菜单、焦点、dmabuf 等待；compose：比对、上传、时钟；
              # backend：xkb、无头帧、DRM 卡上已连接的显示器、嵌套窗口；
              # protocols：layer 表面、锁屏、选区、输入法、装饰与激活、
              # 窗口列表、指针锁定、空闲、单像素缓冲、拖放、截屏、沙盒、
              # 输出配置；无头输出上的外壳：面板独占区、锁屏、截一帧、
              # 应用输出配置；wire、proto、sys 同前
sheaf test --release -p luminaria_compose   # 帧开销
sheaf test --release -p luminaria           # 命中测试
sheaf test --release -p luminaria_proto     # 线协议吞吐
```

## 许可证

Apache-2.0（[LICENSE-APACHE](LICENSE-APACHE)）或 BSD-2-Clause（[LICENSE-BSD](LICENSE-BSD)），任选其一。
