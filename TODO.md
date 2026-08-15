# TODO

未完成的工作，按执行顺序排。方向与理由见 [docs/adr/](docs/adr/)；已完成的东西不在这里，
在 [docs/features.md](docs/features.md)。

---

## 执行顺序

排序原则：**能不能观测**。为"低功耗"做的每一个决定，效果都只在真机上看得见，所以真机验证
排在最前面；bench 排第二，因为它是后面所有优化的验收手段。

### 0. 真机验证 DRM 剩余路径 — 多输出、热插拔、直出

2026-08-15 已在一块 1920×1080@60Hz 的真实显示器上完成第一轮空闲 VT 验证：atomic
模式设置与连续翻页、GPU 合成的 in/out fence 链、libinput 键鼠、硬件光标平面、
Ctrl+Alt+Fn/libseat VT 切换均正常；Konsole 的客户端装饰、popup 菜单、最大化/还原、最小化、
关闭以及窗口消失后的双缓冲重绘也经过反复手测。证据由 `scripts/tty-check.sh` 留在 `logs/`。

仍需真机覆盖的部分：

- 两台及以上显示器的 CRTC/primary/cursor plane 分配与跨屏指针。
- udev connector 热插拔，以及拔屏后所有 per-output 状态的回收。
- `Output::set_mode()` 后 scanout target、layout、`wl_output` 的整链重建。
- 客户端 dmabuf 的直接扫描（当前 Konsole 验证只有 GPU 合成，日志中 `scanout=0`）。

做法：空闲 VT 上跑 `luminaria-tty`，用 `scripts/tty-check.sh` 留证；下一轮优先接第二台显示器，
再用可全屏 dmabuf 客户端触发直出。

### 1. bench harness + 三个数进 CI

- **空闲态每秒 atomic 提交次数**，目标 **0**。第 2 步按设计已经做到（空闲时没有提交，
  `luminaria-tty` 那行 `frames/s` 就不该再出现），但只在 headless 上量过；真机复核与
  CI 门禁都还欠着，否则半年后又会悄悄回到 60。
- **稳态 RSS**（单客户端）。
- **每帧 GPU 提交耗时 p99**。

没有 CI 门禁的话，"低占用"六个月后就只是 README 上的一句话。

### 2. 低功耗一二级

**已做**（2026-08-15）：无 damage 不提交 + 按需帧。`Frame::submit()` 答 `unchanged` 时
不再渲染也不再翻页；`frame` 事件改成一次 `Output::schedule_frame()` 换一个，DRM 空闲时因此
不再有翻页、也就没有 vblank 回调，headless 的定时器不再自续，嵌套后端靠一次空 commit 向父
混成器要 callback。唤醒由 `Frame` 自己发起（它盯着画过的每个表面的 commit，加上
`damage_all()` / `reset()`），混成器只需在 `unchanged` 时补发帧回调——那一帧没有 `present`。
`test_idle_frame` 守后端契约，`test_idle_wake` 用一个按帧回调节流的客户端守「不空转也不冻住」
（20 轮 20ms，20 帧；自由运行的 1ms 帧泵是 400 帧）。

渲染器那一半的每帧零分配也做了：`Region` 的 subtract/intersect 改成两块缓冲乒乓（原先每次
`rects_ = std::move(out)` 都是一次分配加一次释放），`render_to()` 的 region 全部改成渲染器
自己的 scratch，framebuffer 按 render pass 挂在 `ScanoutTarget` 上建一次，command buffer
与 fence 走按提交 fence 回收的空闲表。`test_render_alloc` 守着：稳态 32 帧，两条路径都是
**0 次** C++ 堆分配。

仍未做：

- 真机复核：第 0 步的 tty 验证要重跑一遍，看空闲时 `frames/s` 那行是不是真的不再出现。

### 3. 协议补齐 — 已完成（2026-08-15）

五个协议全部落地，协议对象数 25 → 30。

- **`wp-fifo-v1` + `wp-commit-timing-v1`** —— 两者共用 `Surface` 上新加的**提交闸门**：
  被扣住的那次 commit 对下游完全不存在（不发 commit 信号、不产生 damage、不排帧回调），
  这正是两个协议承诺的语义。FIFO 的 barrier 由 `Surface::send_frame_done()` 清除，
  所以按规矩在 present 里答帧回调的 compositor 不需要额外接线；commit-timing 的到点唤醒
  由 global 自带的 EventLoop 定时器发起。`test_fifo` / `test_commit_timing` 守着。
- **`wp-content-type-v1`** —— 粘性双缓冲状态，`Surface::content_type()`。`test_content_type`。
- **`ext-session-lock-v1`** —— 失败向关：客户端崩了不解锁（`SessionLockDestroy::unlocked`
  区分两种消失），四个锁定表面协议错误全发。`test_session_lock`。
- **`input-method-v2`** —— 接上了 `text-input-v3` 的另一端，两边状态由库自动抄通，
  `test_input_method` 用一个同时扮演应用与输入法的客户端跑完整个来回。XML 上游不发，
  vendored 到 `protocol/`。

三个核心协议已注册进 `tinyluminaria`；两个 desktop 协议按 ADR 0003 只在 `luminaria.desktop`
里，需要 compositor 自己创建。剩下的收尾在下面「已知的天花板」里。

---

## 明确不做

- **摆放策略**（平铺 / 浮动 / 工作区切换逻辑）—— 每个混成器自己实现，见 `CONTEXT.md`。
- **窗口装饰绘制** —— 库不画标题栏，所以 `xdg-decoration` 默认答 client-side。
- **output-management / gamma-control / tablet-v2 / pointer-gestures** —— 完整桌面要，
  但混成器可以自己实现，且都不影响那三个数。
- **VRR / 动态刷新率**（低功耗第三级）—— 收益大，但依赖真机数据，等第 0 步之后再判断。

---

## 源码里活着的标记

只剩四处（空操作桩为 0）：

| 位置 | 内容 |
|------|------|
| `xwayland.cppm:5` | 最小 XWM —— map/configure 已处理，完整 ICCCM/EWMH 未做 |
| `region.cppm:14` | `Region` 的操作 O(n)、合并 O(n²)。矩形数量成为问题时换 `pixman_region32` |
| `tearing_control.cppm:96` | 重复的 tearing_control 对象不发协议错误 |
| `tty_compositor.cpp:13` | 示例里窗口固定偏移层叠，无移动/缩放/堆叠 UI |

## 已知的天花板（不是 bug，是没做完的地方）

- **闸门里只停一次提交** —— `wp-fifo-v1` / `wp-commit-timing-v1` 的规范说排队的提交按序
  排成一列；这里第二次提交合并进已经停着的那一次（gate 取两者的并集，时间戳取较晚的），
  也就是塌缩成一帧。动画客户端每帧一次提交，落不到这个差异上；真要排队得让 `Surface`
  持有一条 `State` 队列。
- **不可见表面的 FIFO 放行归混成器** —— barrier 只在 `send_frame_done()` 时清除，所以最小化
  或被完全遮挡、混成器不再 present 的客户端会一直停在闸门里。`FifoManager::unblock_hidden()`
  是那把钥匙，但什么算"不可见"只有混成器知道，示例里还没接。
- **输入法的键盘 grab 要混成器路由** —— `InputMethod::keyboard_grab()` 非空时按键该先给它
  再给客户端，库拿不到键盘就替不了这个决定；keymap 已经替你发了。候选窗的摆放同理。
- **popup 的 `constraint_adjustment` 要混成器配合** —— 不调
  `XdgShell::set_popup_constraint_query()` 就拿不到父窗口位置，贴边菜单会溢出。
- **GPU 设备热插拔不处理** —— 只跟踪 connector 状态，整块显卡消失不管。
- **headless 后端每帧一次全屏 CPU 读回** —— wl_shm 呈现的代价；嵌套后端只在父混成器没有
  `zwp_linux_dmabuf_v1` 时才付，DRM 后端从不付。
- **`xdg_activation` 不校验 serial** —— 库没有"这个客户端最近收到过哪些 serial"的账，
  防焦点窃取要混成器自己核对。
- **`data_control` 的 filter 无法链式叠加** —— libwayland 的 `wl_display` 只有一个 global
  filter 槽且拿不回旧值。混成器若自己要 filter，应把 data-control 的判断写进自己那个里。
- **嵌套模式下没有独立于 Shift/Ctrl 的修饰键状态**（父混成器报告的除外）。裸机一侧已经有
  了：`LibinputBackend` 自带 xkb 状态机。
- **`LibinputBackend` 只发相对指针运动** —— 绝对定位设备（触摸屏、数位板，以及虚拟机给客户机
  的那块 tablet）的 `LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE` 不处理，于是虚拟机里指针根本
  不动。缺的是"设备映射到哪块输出"这个决定该由谁做：坐标只能归一化后交给混成器。
- **`LibinputBackend` 报告 touch 能力但不发 touch 事件** —— `capabilities()` 如实说有触摸屏，
  触摸事件本身还没接（`Seat` 那一侧 `touch_down/motion/up/frame` 是齐的）。所以示例里
  `set_capabilities()` 的 touch 一律传 false。
