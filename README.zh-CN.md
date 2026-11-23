# luminaria

[English](README.md)

用 [Mica](https://github.com/antinomie1/mica) 写 Wayland 合成器的库，用
[sheaf](https://github.com/antinomie1/sheaf) 构建。它提供机制：协议状态、合成和设备。策略和主循环
归合成器自己。设计见 [docs/](docs/README.md)。

| 包 | 作用 |
|----|------|
| `luminaria` | `Display`：一个事件循环接起输出端、渲染器和服务端；布局、输入路由、操纵、帧时钟、栅栏 |
| `luminaria_server` | 协议状态：表面、事务、xdg-shell、layer shell、锁屏、seat 与输入法、选区与拖放、输出及其配置、截屏、沙盒、缓冲、时序；不碰 GPU |
| `luminaria_compose` | 基于 `vk_compose` 的 Vulkan 渲染器：放置表比对、所有纹理、渲染目标 |
| `luminaria_backend` | DRM/KMS、嵌套、无头三种输出；`Devices`（libseat、libinput、xkb），也能放在自己的输出端下；不碰 GPU |
| `luminaria_types` | 各层交换的纯数据：dmabuf、放置、输出、输入、渲染目标、测量；共用的 CPU 像素拷贝 |
| `luminaria_proto` | 由 `protocols/*.xml` 生成的接口 |
| `luminaria_wire` | 线协议：socket、fd 传递、编解码、对象表 |
| `luminaria_scanner` | 生成 `luminaria_proto`（开发工具） |

```
luminaria ─┬─ luminaria_server ─── luminaria_proto ── luminaria_wire
           ├─ luminaria_compose ── vk_compose, vk_draw2d, vk_gpu (github.com/antinomie1/vk)   [vulkan]
           └─ luminaria_backend ── sys/*；luminaria_proto（仅嵌套）                             [backend]
                  它们都依赖 ──── luminaria_types
```

`Display` 通过结构化 trait 接起一个输出端和一个渲染器：`plug.Output`（它同时是键盘和输入设备所经的
`plug.Input`）和 `plug.Renderer`；`Display` 单态化，换上自己的类型没有间接调用。两者只交换
`luminaria_types`：输出端要渲染目标，渲染器造出来（要显示的 dmabuf）、画进其中一个、交回栅栏。可选能力
（直接扫描输出、光标平面、dmabuf 导入、遮挡、keymap）带默认实现。`lum.open` 用内置的两个；
`display.Display.with(o, output, renderer)` 接任意别的，自己的输出端可以内嵌 `backend.Devices` 复用本机的
会话、libinput 和 keymap。服务端不懂的协议可在运行时按消息签名加上（`d.extend`）。

`luminaria` 的构建特性：`backend`（内置输出端）、`vulkan`（内置渲染器）、`nested`（嵌套输出；后端
只因它才依赖 `luminaria_proto` 和 `luminaria_wire`），默认全开。关掉 `nested` 时 `Kind.Auto` 总选 DRM。

`protocols/` 是 wayland 1.26、wayland-protocols 1.50 和 wlr-protocols 的 XML。服务端支持：

| 方面 | 协议 |
|------|------|
| 核心与外壳 | wl_compositor 6、subcompositor、shm、wl_seat 9、wl_output 4、xdg-shell 6、wl_fixes |
| 缓冲与时序 | linux-dmabuf 4（8-bit / 10-bit / FP16）、linux-drm-syncobj、single-pixel-buffer、viewporter、fractional-scale、presentation-time、fifo、commit-timing、tearing-control、alpha-modifier、content-type、wp-color-management v1（HDR / BT.2020 / PQ / scRGB） |
| 窗口 | xdg-decoration、xdg-activation、xdg-toplevel-icon、xdg-toplevel-tag、xdg-dialog、xdg-foreign v2、xdg-system-bell、ext-foreign-toplevel-list |
| 桌面 | wlr-layer-shell 5、ext-session-lock、xdg-output 3、ext-idle-notify 2、idle-inhibit、wlr-gamma-control v1（硬件 CRTC Gamma LUT / 护眼色温） |
| 输入 | relative-pointer、pointer-constraints、pointer-gestures 3、cursor-shape 2、keyboard-shortcuts-inhibit、text-input-v3、input-method-v2、virtual-keyboard、wlr-virtual-pointer v2 |
| 选区 | 剪贴板、主选区、ext-data-control、带动作协商的拖放 |
| 截屏与管控 | ext-image-copy-capture（输出源与窗口源，shm、零拷贝 dmabuf 及按窗口 `Style.block_capture` 隐私遮罩）、wlr-output-management 4、security-context（沙盒客户端看不到特权 global） |

尚未支持：数位板（`tablet-v2`）。

## 写一个合成器

```mica
import luminaria as lum

var d = try lum.open(lum.Options {})   // DRM，或在 WAYLAND_DISPLAY 下嵌套
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
- **`Display.layout` 与 `rebuild` 可扩展。** 放置表 diff 按索引表 + 耐心排序 LIS 重排以
  O(N) / O(N log N) 完成，稳态不拷贝放置表或弹出菜单表（避免 O(窗口 × 弹出菜单) 遍历），
  shm 上传按脏表面队列 O(脏表面) 合并提交。
- **多平面 dmabuf 与硬件 overlay。** 客户端 dmabuf 支持 1..4 平面（单 BO 与 `DISJOINT`）；符合条件的
  dmabuf 在通过 KMS `TEST_ONLY` 与 z-order 遮挡检查后提升为硬件 overlay 平面，主合成跳过对应图层。
- **GPU 优先级与设备丢失恢复。** Vulkan 队列请求 `VK_KHR/EXT_global_priority`（`Realtime` -> `High` ->
  `Medium` -> `Default` 回退）；遇到 `VK_ERROR_DEVICE_LOST` 时自动重置扫描目标、重建设备与纹理并
  发出 `Event.GpuReset`。
- **多 seat。** 支持多个逻辑 seat（`add_seat`、`assign_device_seat`），各自维护独立指针、焦点、
  keymap、抓取、空闲状态及辅助软件光标。
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
- DRM 时原子提交在提交线程上发出，每个 CRTC 一个队列，在瞄准的 vblank 允许的最晚时刻提交（减去垂直
  消隐和按提交耗时学到的余量）；光标移动并进帧的提交，显示的是最后的位置。看门狗区分事件循环卡顿和
  驱动不完成的翻转。
- 事件循环和提交线程用 `SCHED_FIFO`（单核 `SCHED_RR`，无权限时 nice −15），启动的客户端不继承；
  有输入时每 500 ms 至多一次写 `$XDG_RUNTIME_DIR/luminaria-input-boost`，供调频守护进程提频。
- `d.stats(o)` 测量运行中的显示：帧间隔和帧耗时的分位数、漏帧、损坏、提交、卡顿、调度。

### 后端

| 后端 | 输出 | 输入 |
|------|------|------|
| DRM | 原子 KMS（提交线程）：主平面、overlay 平面和光标平面、来自 `IN_FORMATS` 的修饰符、翻转时间戳、撕裂客户端的异步翻转、VRR、DPMS、CRTC `GAMMA_LUT`、HDR 静态元数据（`HDR_OUTPUT_METADATA` / `BT2020_RGB` / 10-bit `XRGB2101010`）、热插拔 | libinput（经 libseat，多 seat、平坦/自适应加速、DWT）、xkb（多布局切换） |
| 嵌套 | 父合成器里的一个窗口（标题栏和边框经 xdg-decoration 由父合成器画），dmabuf 经 linux-dmabuf 交出，带隐式栅栏 | 父合成器的 seat 和 keymap |
| 无头 | 按模式刷新率的 vblank 网格显示的纹理 | `inject` / `inject_on` |

release 构建，i5-13600KF + Radeon RX 6800（`sheaf test --release`）：

| 项目 | 时间 |
|------|------|
| 1920x1080 一帧，50 个圆角带阴影的窗口、一个在动：CPU 录制并提交 | 9 µs |
| 同上，直到 GPU 完成 | 45 µs |
| 1000 个放置项：`Compositor.layout` diff / `Display.rebuild` | 75 µs / 160 µs |
| 命中测试，512 个表面，最坏情况（整数 SIMD，一步 8 个） | 252 ns |
| 门禁（`tests/probe.mica`）：1280x720@240 Hz，50 个窗口、输入按 1000 Hz 拖动一个，480 帧 | 0 漏帧，间隔 P99 4.17 ms，帧耗时 P50 67 µs / P99 214 µs，每帧损坏约 4.3 万像素（全屏的 4.7%） |

`tiny` 嵌套在 1080p60 桌面里、开一个 foot：strip 后的二进制 1.3 MB，PSS 12 MB（私有 5 MB），3 个线程，
显存 17 MB、GTT 20 MB；空闲 0% CPU，foot 每帧重绘时占一个核的 1.1%。

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
  - 客户端给的坐标和尺寸在请求入口夹到 ±2^24 px，几何运算不会溢出；长期持有的 fd 先验证（沙盒的
    监听 fd 必须是在监听的 socket）。
- **模糊测试。** 带种子的恶意客户端按协议 XML 生成随机请求（活着的对象、边界值、新 id、fd），以及
  破坏帧格式的随机字节；服务端不能崩溃、要关掉它们打开的一切，并且仍能服务正常客户端。

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
| 请求：编码、发送、接收、解码 | 39 ns |
| 事件（`wl_pointer.motion`） | 48 ns |

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
sheaf test    # libwayland 与 foot 客户端在无头输出上、各协议、自定义输出端与渲染器、
              # 多 seat、GPU 重置、性能门禁、模糊测试；wire、proto、sys
sheaf test --release -p luminaria_compose   # 帧开销
sheaf test --release -p luminaria           # 命中测试、门禁的数字
sheaf test --release -p luminaria_proto     # 线协议吞吐
FUZZ_SEED=3 FUZZ_ROUNDS=2000 sheaf test -p luminaria_server   # 更长的模糊测试
```

## 许可证

Apache-2.0（[LICENSE-APACHE](LICENSE-APACHE)）或 BSD-2-Clause（[LICENSE-BSD](LICENSE-BSD)），任选其一。
