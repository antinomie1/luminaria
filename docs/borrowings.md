# 借鉴

## WDDM + DWM

| # | WDDM / DWM | Linux 上的对应 | luminaria 中 | 状态 |
|---|---|---|---|---|
| 1 | Monitored fence | 时间线信号量、drm_syncobj 时间线 | vk_gpu `Ticket`；syncobj 获取点变 eventfd，释放点在呈现后打 | 已实现 |
| 2 | DWM 不被单个应用拖住 | 栅栏 fd 进事件循环 | 渲染路径上不等客户端；事务按客户端排队 | 已实现 |
| 3 | Flip model + 帧统计 | KMS 原子翻转、wp_presentation | `Presented`、呈现反馈带 vblank 时间戳与零拷贝标志 | 已实现 |
| 4 | Flip queue（内核里等 GPU，按 vsync 执行排队的翻转） | `IN_FENCE_FD`、提交线程 | GPU 的 sync_file 交给 KMS；嵌套时挂成隐式栅栏；提交线程在 vblank 前最后时刻提交、合并光标，每个 CRTC 自己的节奏 | 已实现 |
| 5 | Independent Flip / DirectFlip | 客户端 dmabuf 直接扫描输出 | 原子测试后直接翻转；嵌套时交给父合成器 | 已实现 |
| 6 | MPO | overlay、光标平面 | 光标平面 | overlay 平面未做 |
| 7 | Allow tearing | tearing-control-v1、异步翻转 | 直接扫描输出时按提示用 `PAGE_FLIP_ASYNC` | 已实现 |
| 8 | 合成器时钟 | 预测 vblank | 每帧尽量晚开始，余量按迟到反馈自适应 | 已实现 |
| 9 | 每显示器刷新率、Win11 DRR | 每输出时钟、VRR | 每输出时钟、VRR | 按内容降刷新未做 |
| 10 | Present1 脏矩形、flip-discard 缓冲年龄 | 缓冲年龄、`FB_DAMAGE_CLIPS` | 只画损坏；变化的矩形交给 KMS 或父合成器 | 已实现 |
| 11 | 内容没变不呈现 | 不提交 | 空闲时不出帧、不挂定时器 | 已实现 |
| 12 | 不渲染看不见的 | 遮挡剔除 | 跳过被遮挡的形状；看不见的表面不收帧回调 | 已实现 |
| 13 | 共享资源 | dmabuf | 每个缓冲只导入一次 | 已实现 |
| 14 | 缩略图 / 重定向 | 一张纹理放多处 | 同一表面可以出现在多个放置、多个输出上（`Style.scale`） | 已实现 |
| 15 | GPU 优先级与抢占 | `VK_EXT_global_priority` | — | 未做 |
| 16 | VidMM 显存驻留 | `VK_EXT_memory_budget` | — | 未做 |
| 17 | TDR（超时检测与恢复） | 翻转看门狗、`VK_ERROR_DEVICE_LOST` | 翻转 1 s 未完成时 ping 事件循环，分出循环卡顿和驱动不翻转并计数 | 检测已实现，设备丢失恢复未做 |
| 18 | DComp 事务 | configure / ack / commit | `begin` / `end` | 已实现 |
| 19 | MMCSS（DWM 的线程按实时类调度） | `SCHED_FIFO` / `SCHED_RR`、`RLIMIT_RTPRIO`、`SCHED_RESET_ON_FORK` | 事件循环 50、提交线程 51，单核用 RR；无权限退到 nice −15；子进程回到普通调度 | 已实现 |
| 20 | 输入时提升（输入唤醒的线程提优先级、电源管理按输入提频） | 调频守护进程 | 有输入时每 500 ms 至多一次写 `$XDG_RUNTIME_DIR/luminaria-input-boost` | 已实现 |
| 21 | DWM 合成计时统计、PresentMon 式端到端测量 | 库内探针 | `Display.stats`：间隔与帧耗时分位数、漏帧、损坏、提交、调度；`tests/probe.mica` 按固定交互脚本做门禁 | 已实现（过度绘制、离屏 pass 数未做） |

不借鉴：内核态 / 用户态驱动拆分（DRM 与 Mesa 本来如此）、GDI 重定向（Linux 上没有对应物）、DComp
的保留式 Visual 树 API（只取它的事务）。

## Quartz / Core Animation

| # | Quartz | luminaria 中 | 状态 |
|---|---|---|---|
| Q1 | Core Animation 渲染服务器插值动画声明 | `Style.transition_ms`：合成器只给目标，库按时间插值，动画期间不唤醒策略层 | 已实现 |
| Q2 | NSWindowOcclusionState、App Nap | 连续 500 ms 不可见发 `Occluded`，`accept` 发 xdg_toplevel `suspended` | 已实现 |
| Q3 | Coalesced Updates | shm 每帧只上传一次，一帧内被替换的缓冲不拷贝直接释放 | 已实现 |
| Q4 | ProMotion / LTPO、CADisplayLink 帧率范围 | 按内容节奏降低刷新率，空闲时最低或面板自刷新 | 未做 |
| Q5 | IOSurface purgeable | 驱逐挂起窗口的 shm 纹理、留低分辨率快照，取消挂起时请客户端重画 | 未做 |
| Q6 | 定时器合并 | 每个输出一个帧定时器，空闲时一个都不挂 | 部分 |
| Q7 | vibrancy 模糊缓存 | 模糊结果按下方区域缓存，下方有损坏才重算 | 未做（vk_draw2d 还没有模糊） |

不借鉴：保留式 CALayer 树 API、2× 渲染再缩小（fractional-scale-v1 让客户端按目标尺寸渲染，更省）、
轮询 IOSurface 使用计数（`wl_buffer.release` 加显式同步已覆盖）、由 WindowServer 管层级和装饰（策略）。

## 对 vk 的要求

| 需要 | 状态 |
|---|---|
| `export_dmabuf_with`：按修饰符列表导出 | 已加 |
| `Frame.new`：vk_gpu 之外的目标（KMS、父合成器） | 已加 |
| `dmabuf_modifiers`、`drm_node`：linux-dmabuf 格式表与主设备 | 已加 |
| 设备丢失单独成为一种错误、`VK_EXT_global_priority`、`VK_EXT_memory_budget` | 未做 |
| GPU 时间戳查询 | 未做：时钟用迟到反馈自适应余量代替 |
| `write_texture_rect` 按行紧凑暂存 | 未做：现在暂存从首行到末行的整段跨度（含行距），窄而高的损坏多拷了整行 |
| 绘制计数（实际画的像素、离屏 pass） | 未做：探针还算不出过度绘制率 |

## 尚未做的

- overlay 平面（现在只用主平面和光标平面）、按内容降低刷新率（Q4）。做 overlay 时，平面分配要成为出帧的
  一个阶段：从上往下给候选视图逐个试 `TEST_ONLY`，失败的那层和它下面的回退给 GPU 合成；探针的
  `tests` 计数用来看试探的代价。
- 模糊（Q7 的前提）：加进 vk_draw2d 时，损坏要先按效果扩张再剔除：从上往下，模糊视图把下方需要重画的
  范围按采样半径扩大，上方不透明区域再从下方的损坏和可见区域里扣掉，否则局部重画在边缘采到旧像素。
- 设备丢失恢复、GPU 全局优先级、显存驻留与快照（Q5）、模糊缓存（Q7）。
- 协议：数位板（tablet-v2）；截屏的 dmabuf 缓冲与窗口源（现在只有 shm 和输出，读回要等 GPU）。
- 输出配置的禁用只是关屏，输出仍在布局里；输出不能旋转。
- 多个 seat（现在只有 seat0）。
