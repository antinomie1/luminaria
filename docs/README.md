# luminaria 设计

luminaria 是用来写 Wayland 合成器的库：协议状态、合成、设备由库负责，窗口放哪、焦点给谁、
快捷键做什么、每帧显示什么由合成器决定。库只提供机制，不带策略。

## 设计目标

| 目标 | 落到实处 |
|---|---|
| 低延迟 | 帧在下一个 vblank 前尽量晚地开始，提交线程在 vblank 前最后时刻提交、光标并进同一次提交；事件循环和提交线程实时调度；移动、缩放、平移在库内按输入事件驱动；GPU 栅栏交给内核（`IN_FENCE_FD`），CPU 不等 |
| 零开销 | 事件是值，没有回调网和引用计数；句柄是可复制的 id；稳态不分配；没用到的协议代码被链接器剔除 |
| 高性能 | 一个 pass 只画损坏区域、跳过被遮挡的内容；一次 `sendmsg` 发完一个客户端的事件；命中测试用整数 SIMD；放置表 diff 按索引表 O(N)/O(N log N) 与复用的 scratch 缓冲零拷贝重建 |
| 轻量 | 合成器只接触 `Display`、句柄和 `Event`；依赖只有 libdrm、libinput、libseat、libudev、xkbcommon 和 Vulkan |
| 低功耗 | 空闲时零唤醒；看不见的客户端停帧并收到 suspended；能直接扫描输出或提升至硬件 overlay 就不合成；`FB_DAMAGE_CLIPS` 让带自刷新的面板省电 |
| 鲁棒 | 请求 `VK_KHR/EXT_global_priority` 队列优先级；`VK_ERROR_DEVICE_LOST` 自动重置扫描目标与纹理并通知 `GpuReset`；支持多 seat 与多平面（1..4 平面）dmabuf |
| 内存安全 | `unsafe` 只在 FFI、映射与线协议缓冲边界且先查边界；客户端数值在请求入口校验或夹紧（坐标尺寸 ±2^24 px）；客户端内存只经 SIGBUS 安全窗口读取；fd 断开即关；模糊测试守住服务端不崩、不漏 |
| 边缘设备友好 | 一个事件循环线程（DRM 时另有一个提交线程）；交换缓冲数可选（2 省内存）；显存从 4 MiB 的块按需增长、空块释放；平铺 / 压缩修饰符省显存带宽；GPU 只需 Vulkan 1.3 |

## 分层

```
luminaria ─┬─ luminaria_server ─── luminaria_proto ── luminaria_wire
           ├─ luminaria_compose ── vk_compose, vk_draw2d, vk_gpu   （特性 vulkan）
           └─ luminaria_backend ── sys/*；luminaria_proto（仅嵌套） （特性 backend）
                  它们都依赖 ──── luminaria_types
```

| 包 | 负责 | 不碰 |
|---|---|---|
| `luminaria_wire` | 线协议：socket、fd 传递、编解码、对象表 | 协议语义 |
| `luminaria_proto` | 由 `protocols/*.xml` 生成的类型化接口 | 状态 |
| `luminaria_server` | 协议状态：表面、事务、xdg-shell、seat、输出、缓冲、时序 | GPU、策略 |
| `luminaria_types` | 各层交换的纯数据：dmabuf、fourcc、放置、输出描述、输入、渲染目标、测量；共用的 CPU 像素拷贝（整行 memcpy，alpha 按 64 字节缓存行对齐的向量写） | 其他行为 |
| `luminaria_compose` | Vulkan 渲染器：持有设备和所有纹理（客户端缓冲、合成器图像、渲染目标），放置表比对成场景、只画损坏 | 协议、显示 |
| `luminaria_backend` | DRM/KMS、嵌套（特性 `nested`，默认开）、无头三种输出；`Devices`（libseat、libinput、xkb，可单独用）、keysym 常量 | 协议、GPU |
| `luminaria` | `Display[O, R]`：一个事件循环把服务端、输出端 `O`、渲染器 `R` 接起来，加上布局、输入路由、操纵快速路径、帧时钟、栅栏；`display.mica` 只放状态、循环和公开调用，实现按主题分文件（outputs、clients、waits、place、seats、input、manipulation、frames、capture、shell） | 策略 |

server、compose、backend 之间互不依赖，只交换 `luminaria_types` 里的纯数据，由门面 `luminaria` 接线。
所以每一层都能单独测试、单独拿去用，也能整层换掉：输出端和渲染器是 `plug.mica` 里的两个结构化 trait，
有同名方法的类型就能接上（`display.Display.with`），`Display` 单态化，没有间接调用；可选能力带默认实现。
`plug.Output` 继承 `plug.Input`（keymap、修饰键、设备配置、切 VT，全部可选），自己的输出端内嵌
`backend.Devices` 就能用本机键盘鼠标。
输出端和渲染器之间只有一条约定：输出端要渲染目标（`Acquire.Need(Want)`），渲染器造出来（导出成 dmabuf），
每帧画进其中一个并交回 sync_file，输出端拿它显示。服务端不懂的协议可以在运行时按签名加上。

## 核心决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 策略接口 | 立即模式放置表：`layout` 整表替换，库与上一张比对 | 不维护第二份窗口真相（延续 luminaria-cpp ADR 0001）；放置表是纯数据，日后可以挪到合成线程 |
| 事件模型 | 拉取式：`next` 返回值，`accept` 是默认动作 | 没有 listener 网和回调；合成器只写它要改的部分 |
| 客户端栅栏 | 渲染路径上 CPU 和 GPU 都不等；栅栏 fd 进事件循环，就绪才生效 | 一个慢应用拖不住整帧，没就绪的缓冲继续显示上一帧 |
| 出帧方式 | 库来选：不出帧 → 直接扫描输出 → 合成 | 对策略层不可见，总选最省的 |
| 帧驱动 | 有东西要显示才按时钟定时 | 空闲零唤醒 |
| 可插拔 | 输出端、渲染器是结构化 trait，`Display` 单态化；两者只交换渲染目标和栅栏 | 换层不付运行时代价；后端不碰 GPU |
| 线程 | 一个事件循环线程；DRM 时加一个提交线程，两者实时调度（`SCHED_FIFO`，子进程不继承） | KMS ioctl 不卡协议处理和录制，满载 CPU 下帧不抖；放置表与帧数据都是纯数据，需要时再拆出合成线程不改接口 |

## 一帧和一次输入

```
客户端 commit ──▶ server：快照成事务 ──(需等待)──▶ Committed ──▶ Display：栅栏 fd 进 epoll
                                     └─(不需等待)─▶ 立即生效
事务生效 ──▶ Applied ──▶ Display：缓冲变纹理、损坏记入场景、重建放置表 ──▶ 输出变脏
输出变脏 ──▶ 按时钟定时 ──▶ Frame 事件（合成器可推进布局）──▶ 出帧：
             不出帧 / 直接扫描输出客户端缓冲 / 合成（只画损坏）──▶ backend 提交
周期结束（呈现 / 失败 / 无新帧）──▶ 呈现反馈、fifo 解除、归还无人读的缓冲、更新时钟

libinput / 父合成器 ──▶ backend：xkb ──▶ Display：光标立即移动、操纵中则直接移动窗口
                                     └──▶ Key / Motion / Button ... 事件 ──▶ 合成器 ──▶ accept：路由给客户端
```

## 文档

| 文档 | 内容 |
|---|---|
| [api.md](api.md) | 对外接口：事件循环、事件、`accept` 的默认动作、布局、事务、操纵、输出、图像 |
| [protocol.md](protocol.md) | 线协议、生成的接口、服务端状态、提交与事务、xdg-shell、seat、剪贴板、扩展协议 |
| [compose.md](compose.md) | 缓冲与同步、放置表落地、合成、合成器时钟、出帧、直接扫描输出 |
| [backend.md](backend.md) | DRM/KMS、嵌套、无头、会话、输入设备与键盘、光标 |
| [input.md](input.md) | 输入路由、命中测试、抓取、弹出菜单、操纵快速路径 |
| [borrowings.md](borrowings.md) | 借鉴 WDDM + DWM 与 Quartz 的设计、不借鉴的部分、尚未做的部分 |

## 实测

release 构建，i5-13600KF + Radeon RX 6800（`sheaf test --release`）：

| 项目 | 时间 |
|---|---|
| 1920x1080 一帧，50 个圆角带阴影的窗口、一个在动：CPU 录制并提交 | 9 µs |
| 同上，直到 GPU 完成 | 45 µs |
| 1000 个放置项：`Compositor.layout` diff / `Display.rebuild` | 75 µs / 160 µs |
| 命中测试，512 个表面，最坏情况 | 252 ns |
| 门禁 `tests/probe.mica`：1280x720@240 Hz，50 个窗口、输入按 1000 Hz 拖动一个，480 帧 | 0 漏帧，间隔 P99 4.17 ms，帧耗时 P50 67 µs / P99 214 µs，每帧损坏约 4.3 万像素（全屏的 4.7%） |
| 请求：编码、发送、接收、解码 | 39 ns / 条 |
| 事件（`wl_pointer.motion`） | 48 ns / 条 |
| `tiny` 嵌套在 1080p60 桌面、开一个 foot | 二进制 1.3 MB（strip），PSS 12 MB（私有 5 MB），3 个线程，显存 17 MB、GTT 20 MB；空闲 0% CPU，foot 每帧重绘时 1.1% 单核 |
