# luminaria_compose 设计

> 状态：已实现，接口在 `luminaria`（`Display`）。合成在 `luminaria_compose` 与 vk（`vk_compose`、`vk_draw2d`、`vk_gpu`），提交与等待在 `luminaria_server`，显示与输入在 `luminaria_backend`。下表“状态”一栏是现状；“未做”的几项见文末。

## 定位

夹在 `luminaria_server`（协议状态）、`vk_compose`（合成）和 `luminaria_backend`（输出）之间，
负责三件事：客户端提交 → 纹理；提交何时生效；每帧怎么出（不出帧 / 直接扫描输出 / 合成）。
只提供机制：窗口放哪、叠放、动画曲线由合成器决定。

依赖 `vk_compose, vk_draw2d, vk_gpu`，不依赖 server 与 backend；三者只交换纯数据，由门面
`luminaria` 转接。

```
luminaria_server ──Commit(纯数据)──▶ luminaria_compose ──Present(纯数据)──▶ luminaria_backend
        ▲                              │  ▲                                    │
        └──Applied/FrameDone/Release───┘  └──────Flipped(seq, 时间戳)──────────┘
                                     合成器策略：layout(放置表)、Frame 时推进动画
```

## 核心决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 策略接口 | 立即模式放置表：`layout()` 整表替换，库与上一张比对，转成 `Scene` 的 add/set/remove/place_above | 延续 luminaria-cpp ADR 0001；设备丢失后可从放置表重建；放置表是 `Send` 数据，日后可挪到合成线程 |
| 客户端栅栏 | 渲染路径上不做 CPU 等待也不做 GPU 等待；栅栏 fd 进事件循环，信号到了才换纹理 | 一个慢应用拖不住整帧，没就绪的缓冲显示上一帧 |
| shm 释放 | `write_texture_rect` 在 CPU 端拷进上传环，拷完立即 `wl_buffer.release` | 客户端少用一个缓冲，延迟更低 |
| 帧驱动 | 有损坏才按合成器时钟定时；空闲不挂定时器 | 低功耗 |
| 出帧方式 | 库来选：不出帧 → 直接扫描输出 → 合成 | 策略层不可见 |
| 事件循环 | 对外一个 fd（内部嵌套 epoll，挂栅栏 fd 与 timerfd），`dispatch()` / `events()` | 主循环归合成器 |

## API

用法见 README 的“Writing a compositor”。合成器只接触 `Display`、句柄（`Window`、`Output`、`Client`）和 `Event`：
`next` 取事件，`accept` 做协议默认动作，`layout` 给整张放置表，`configure` / `focus` / `close` 管窗口，
`begin` / `end` 包住布局事务，`manipulate_*` 启动库内的移动、缩放、平移。

## 模块

| 包 | 文件 | 内容 |
|---|---|---|
| `luminaria` | `display.mica` | 事件循环、glue（提交 → 纹理、等待、弹出菜单）、放置表落地、输入路由与 `accept`、操纵快速路径、帧调度、直接扫描输出、光标 |
| | `layout.mica` / `hit.mica` / `cursor.mica` | 放置表构建器；整数 SIMD 命中测试；XCursor 主题加载 |
| `luminaria_server` | `surface` `buffer` `xdg` `seat` `data` `output` `extra` `route` | 协议状态、事务、xdg-shell、seat、剪贴板、输出、扩展协议 |
| `luminaria_compose` | `lib` `format` `fence` `clock` | 按 key 比对的放置表、纹理与上传、栅栏 fd、合成器时钟 |
| `luminaria_backend` | `drm` `nested` `headless` `swap` `session` `input` `keyboard` | 三种显示后端、扫描输出缓冲、libseat、libinput、xkb |

## 每一帧

1. 时钟到点 → `Frame(out, vblank)`；合成器可按预测呈现时间更新布局。
2. 就绪提交生效 → `Applied`。
3. 选出帧方式：`idle()` 不出帧；`scanout()` 候选测试通过就直接翻转；否则 `render` 合成（一个 pass、只画损坏、跳过遮挡）。
4. 给本输出可见的表面发 `FrameDone`。
5. 翻转完成 → `flipped` → `Presented`，更新时钟预测。

## 借鉴：WDDM + DWM

| # | WDDM / DWM | Linux 对应 | luminaria 中 | 状态 |
|---|---|---|---|---|
| 1 | Monitored fence | 时间线信号量、drm_syncobj 时间线 | vk_gpu `Ticket`；syncobj 点即时间线值 | 已实现：Ticket；syncobj 获取点变 eventfd、释放点在呈现后打点 |
| 2 | DWM 不被单个应用拖住 | 栅栏 fd 进事件循环 | `display.mica` | 已实现：栅栏进 epoll，按客户端排队 |
| 3 | Flip model + 帧统计 | KMS 原子翻转、wp_presentation | `Presented`：Composited / Scanout / Unchanged + vblank 时间戳 | 已实现：`Presented` 与 wp_presentation |
| 4 | Flip queue（内核里等 GPU） | `IN_FENCE_FD` | `display.mica` | 已实现 |
| 5 | Independent Flip / DirectFlip | dmabuf 直接扫描输出 | `Scene.scanout()` + `display.mica` | 已实现：KMS 原子测试后直接翻转；嵌套时交给父合成器 |
| 6 | MPO | overlay / 光标平面 | `display.mica` | 光标平面已实现；overlay 平面未做 |
| 7 | Allow tearing | tearing-control-v1、异步翻转 | 全屏直接扫描输出走异步翻转 | 已实现：直接扫描输出时按 tearing 提示异步翻转 |
| 8 | 合成器时钟 | 预测 vblank | `clock.mica` | 已实现：自适应余量 |
| 9 | 每显示器刷新率、Win11 DRR | 每输出时钟、VRR | `clock.mica` | 每输出时钟、VRR 已实现；按内容降刷新（DRR）未做 |
| 10 | Present1 脏矩形、flip-discard 缓冲年龄 | 缓冲年龄、`FB_DAMAGE_CLIPS` | 最多 8 矩形损坏 + 4 帧历史；`changed()` | 已实现：`FB_DAMAGE_CLIPS`、父合成器 damage |
| 11 | 内容没变不呈现 | 不提交 | `Scene.idle()`；空闲无定时器 | 已实现：空闲时不挂定时器 |
| 12 | 不渲染看不见的 | 遮挡剔除 | vk_draw2d 剔除；`visible()` | 已实现：遮挡剔除；看不见的表面不收帧回调 |
| 13 | 共享资源 | dmabuf | `import_dmabuf` / `export_dmabuf`；只导入一次 | 已实现：每个 buffer 只导入一次 |
| 14 | 缩略图 / 重定向 | 一张纹理放多处 | 视图共享纹理 | 已实现 |
| 15 | GPU 优先级与抢占 | `VK_EXT_global_priority` | 合成队列高优先级 | 未做 |
| 16 | VidMM 驻留 | `VK_EXT_memory_budget` | 驱逐看不见的 shm 纹理（配合 Q5） | 未做 |
| 17 | TDR | `VK_ERROR_DEVICE_LOST` | `display.mica` | 未做 |
| 18 | DComp 事务 | configure / ack / commit | `display.mica` | 已实现：`begin` / `end` |

不借鉴：内核态 / 用户态驱动拆分（DRM 与 Mesa 已是如此）、GDI 重定向、DComp 保留式 Visual 树 API（只取事务）。

## 借鉴：Quartz / Core Animation

| # | Quartz | luminaria 中 | 位置 | 阶段 |
|---|---|---|---|---|
| Q1 | Core Animation 渲染服务器插值动画声明 | `place(...).animate(to, ms, curve)`，库按预测呈现时间插值 | `display.mica` | 已实现：`Style.transition_ms` |
| Q2 | NSWindowOcclusionState、App Nap | 可见性稳定 N ms 后报告 `Occluded`；门面默认发 xdg_toplevel `suspended`（xdg-shell v6） | `display.mica` | 已实现：`Occluded`，`accept` 发 suspended |
| Q3 | Coalesced Updates | 一个周期内只应用最新就绪提交，跳过的缓冲立即释放；fifo-v1 客户端每周期一个 | `display.mica` | 已实现：shm 每帧只上传一次，被替换的缓冲不拷贝直接释放 |
| Q4 | ProMotion / LTPO、CADisplayLink 帧率范围 | 按内容节奏建议刷新率，空闲降到最低或 PSR；backend 执行 | `clock.mica` | 未做 |
| Q5 | IOSurface purgeable | 驱逐挂起窗口的 shm 纹理，留 1/4 快照；取消挂起时发 configure 请客户端重新提交 | `display.mica` | 未做 |
| Q6 | 定时器合并 | 所有唤醒对齐到帧，每帧最多一次；相近输出合并 | `clock.mica` | 部分：每输出一个帧定时器，空闲为零 |
| Q7 | vibrancy 模糊缓存 | 模糊结果按下方区域缓存，下方有损坏才重算 | 等 vk_draw2d 支持模糊 | 未做（vk_draw2d 尚无模糊） |

不借鉴：保留式 CALayer 树 API（与 ADR 0001 冲突）、2× 渲染再缩小（fractional-scale-v1 更省）、
轮询 IOSurface 使用计数（`wl_buffer.release` + 显式同步已覆盖）、WindowServer 管层级和装饰（策略）。

## vk 前置

| 需要 | 状态 |
|---|---|
| `export_dmabuf_with` 接受修饰符列表 | 已加 |
| `Frame.new`（自定义 Target） | 已加 |
| `dmabuf_modifiers`、`drm_node` | 已加（linux-dmabuf feedback 与主设备） |
| 设备丢失单独成为一种错误、`VK_EXT_global_priority`、`VK_EXT_memory_budget` | 未做 |
| GPU 时间戳查询 | 未做：时钟用迟到反馈自适应余量代替 |

## 未做

- overlay 平面（目前只用 primary 与光标平面）、按内容降刷新率（Q4）。
- 设备丢失恢复、GPU 全局优先级、显存驻留与快照（Q5）、模糊缓存（Q7）。
- 可选协议：xdg-decoration、primary selection、xdg-activation、cursor-shape、
  text-input、layer-shell、session-lock；拖放目前被拒绝（源被取消）。
- 多 seat（目前一个 seat0）。
