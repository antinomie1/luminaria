# TODO

未完成的工作，按执行顺序排。方向与理由见 [docs/adr/](docs/adr/)；已完成的东西不在这里，
在 [docs/features.md](docs/features.md)。

---

## 执行顺序

排序原则：**能不能观测**。为"低功耗"做的每一个决定，效果都只在真机上看得见，所以真机验证
排在最前面；bench 排第二，因为它是后面所有优化的验收手段。

### 0. 真机验证 DRM 路径 — 阻塞后面一切

整条 DRM 路径**从未在真实显示器上跑过**：atomic 提交、`IN_FENCE_FD` / `OUT_FENCE_PTR`、
多输出 CRTC/plane 分配、udev 热插拔、硬件光标平面、VT 切换。渲染与几何那一侧有测试覆盖，
但"真实显示器上的翻页"只能在真机上确认，而下面每一条的效果都落在这 1,211 行上。

做法：一个空闲 VT + 一台显示器，跑 `luminaria-tty`，逐项对照上面的清单。

### 1. bench harness + 三个数进 CI

- **空闲态每秒 atomic 提交次数**，目标 **0**。这个数会立刻把第 2 步逼出来。
- **稳态 RSS**（单客户端）。
- **每帧 GPU 提交耗时 p99**。

没有 CI 门禁的话，"低占用"六个月后就只是 README 上的一句话。

### 2. 低功耗一二级

- **无 damage 不提交** —— 消灭空闲态的 60Hz 空转。`Frame::submit()` 已经能分辨这一帧
  「什么都没变」（返回 `Presented::unchanged`），但眼下照样渲染 + 翻页。
- **按需 vblank** —— 静止时不订阅 vblank，来了 damage 再武装一次翻页。省的是内核回调与
  进程唤醒。
- `headless.cppm` 的固定 16ms 软件定时器同理，无内容变化时不该转。
- 顺带：`VulkanRenderer::render_to()` 每帧仍在堆上建 `Region`、`vk::raii::Framebuffer`、
  `CommandBuffers`。外壳层那一半的每帧零分配已经由 `test_frame` 守住了，渲染器这一半没有。

### 3. 协议补齐

按低功耗价值排序：

- **`wp-fifo-v1` + `wp-commit-timing-v1`** —— 客户端声明"我按刷新率走"与"这帧什么时候上屏"。
  **不是锦上添花**：第 2 步要对"客户端在跑动画"这种常见情形真正省电，前提就是这两个。
- **`wp-content-type-v1`** —— 客户端说"我在放视频"，据此走直出或降刷新率。
- **`ext-session-lock-v1`** —— 锁屏，安全性硬缺口。
- **`input-method-v2`** —— IME 的输入法一侧。`text-input-v3` 已完整，缺的是接
  IBus / Fcitx 的那一半。

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

- **popup 的 `constraint_adjustment` 要混成器配合** —— 不调
  `XdgShell::set_popup_constraint_query()` 就拿不到父窗口位置，贴边菜单会溢出。
- **GPU 设备热插拔不处理** —— 只跟踪 connector 状态，整块显卡消失不管。
- **headless 后端每帧一次全屏 CPU 读回** —— wl_shm 呈现的代价；嵌套后端只在父混成器没有
  `zwp_linux_dmabuf_v1` 时才付，DRM 后端从不付。
- **`xdg_activation` 不校验 serial** —— 库没有"这个客户端最近收到过哪些 serial"的账，
  防焦点窃取要混成器自己核对。
- **`data_control` 的 filter 无法链式叠加** —— libwayland 的 `wl_display` 只有一个 global
  filter 槽且拿不回旧值。混成器若自己要 filter，应把 data-control 的判断写进自己那个里。
- **嵌套模式下没有独立于 Shift/Ctrl 的修饰键状态**（父混成器报告的除外）。
