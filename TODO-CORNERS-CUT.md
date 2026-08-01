# TODO & Corners Cut — Luminaria 源码审计

更新于 2026-08-02（补齐 relative-pointer / pointer-constraints / text-input-v3 /
idle-inhibit / wlr-data-control 之后）。列出源码中所有 `TODO` 注释与"偷工减料"处
（为免真实客户端崩溃而设的空操作桩，以及有已知天花板的简化实现）。

> **本轮变化：** 新增五个协议（§2.13–§2.17）。`text-input-v3` 落地后，IME 只剩
> `input-method-v2` 那一半；`data-control` 顺带在 `data_device` 里开出了 `SelectionSource`
> 这条口子，让剪贴板的所有者可以不是 `wl_data_source`。
>
> **上一轮变化：** 剩下的桩基本清空了。`wl_region`（add/subtract）、
> `set_opaque_region` / `set_input_region`、`set_buffer_scale` / `set_buffer_transform`、
> `wl_surface.offset`、`xdg_toplevel.set_parent` / `show_window_menu`、
> `xdg_positioner` 的 `constraint_adjustment` 全部落地。同时补上了
> **异步 explicit sync**（CPU 50ms 等待彻底移除）、**多矩形 scissor damage + 不透明遮挡剔除**、
> **libseat 会话 / VT 切换**、**光标主题 + 硬件光标平面**、
> **viewporter / fractional-scale / xdg-decoration（服务端）**、
> 以及 **按 wl_buffer 缓存的 GPU 纹理**。

---

## 一、TODO 注释清单

### `.cpp`

| 文件 | TODO |
|------|------|
| `examples/tty_compositor.cpp` | 窗口以固定偏移层叠（无移动/缩放/堆叠 UI）。 |

### `include/luminaria/` 头文件

| 文件 | TODO |
|------|------|
| `backend/drm.hpp` | 无客户端 buffer 直出（全屏免合成）；无模式切换 —— 每输出固定用 connector 的首选模式。 |
| `backend/wayland.hpp` | 嵌套后端呈现走 wl_shm CPU 路径（父 compositor 那一侧）。 |
| `xwayland.hpp` | 最小 XWM —— 已处理 map/configure 请求；完整 ICCCM/EWMH 待补。 |

**其余头文件的 TODO 已全部清除。**

---

## 二、Corners Cut — 仍在的简化

### 2.1 `wl_surface`（`src/types/compositor.cpp`）

**全部请求均已实现，无空操作桩。**

| 请求 | 状态 | 备注 |
|------|------|------|
| `damage` | 已实现 | 累积进 `Surface::damage()`，按 surface 尺寸裁剪 |
| `damage_buffer` | 已实现 | 按 buffer scale/transform 反算回 surface 坐标 |
| `set_opaque_region` | **已实现** | 存进 `Surface::opaque_region()`，渲染器据此剔除被遮挡内容 |
| `set_input_region` | **已实现** | `Surface::accepts_input()` 用它做命中测试（圆角/阴影可穿透） |
| `set_buffer_transform` | **已实现** | 取逆后存为 buffer→surface，与输出 transform 复合成一次采样 |
| `set_buffer_scale` | **已实现** | 影响 `surface_width/height`；HiDPI 客户端的 2x buffer 仍占 1x 窗口 |
| `offset` | **已实现** | 累积进 `offset_x/y`，参与 `surface_tree()` 定位 |

`wl_compositor` 版本升到 **6**，新增 `preferred_buffer_scale` /
`preferred_buffer_transform` 事件。

### 2.2 `wl_region`（`src/types/compositor.cpp`）

`add` / `subtract` 由 `util/region.hpp` 的不相交矩形集实现（见 `tests/test_region.cpp`）。

> **天花板：** `Region` 是 O(n) 的矩形向量，不合并相邻矩形。真实负载下矩形数量成为问题时
> 换 `pixman_region32`。已标 `ponytail:`。

### 2.3 `xdg_toplevel`（`src/types/xdg_shell.cpp`）

**13 个请求全部实现，无空操作桩。** `set_parent` 维护 transient-for 关系并发
`ToplevelParentChange`（两端销毁时自动解链）；`show_window_menu` 发
`ToplevelRequestWindowMenu` 交给 compositor（库里不画菜单，这是设计）。

> **注意一处刻意设计：** `set_maximized` / `set_fullscreen` 在**没有任何监听者**时会自动
> 应答。这是为了让不接管窗口状态的最小 compositor 也不会把客户端挂住。

### 2.4 `xdg_positioner` 的 `constraint_adjustment`

**已实现**（`constrain()`，按协议规定的 flip → slide → resize 顺序，逐轴独立求解）。

需要知道父窗口在布局中的绝对位置，这由 compositor 通过
`XdgShell::set_popup_constraint_query()` 提供。

| 项 | 状态 |
|----|------|
| anchor / gravity / offset / size 求解 | 已实现 |
| `flip_x` / `flip_y` | 已实现（翻转后仍溢出则撤销） |
| `slide_x` / `slide_y` | 已实现（先推右/下边，再推左/上边） |
| `resize_x` / `resize_y` | 已实现（裁到可用区，最小 1px） |

> **天花板：** 不设置 `set_popup_constraint_query()` 的 compositor 拿不到约束
> —— shell 层无从知道窗口在哪。`tinyluminaria` 已接线，见 `tests/test_popup.cpp`。

### 2.5 `linux-drm-syncobj` —— **CPU 等待已移除**

| 项 | 状态 | 天花板 |
|----|------|--------|
| acquire point | commit 时 `drmSyncobjTransfer` + `drmSyncobjExportSyncFile` 导出成 sync_file，交给 `Surface::acquire_fence_fd()` | fence 尚未 materialize 时退回一次有界 CPU 等待再重试导出 |
| 渲染等待 | 该 fd 作为 `VkSemaphore`（`VK_KHR_external_semaphore_fd`，`eTemporary` 导入）进入 queue submit | GPU 不支持该扩展时退回 fence 阻塞 |
| release point | 渲染的 out-fence 经 `drmSyncobjImportSyncFile` + `drmSyncobjTransfer` 写进 release point | compositor 必须调 `Surface::notify_rendered(fence)`；不调则退回"下次 commit 时 signal" |

### 2.6 渲染提交与扫描输出 —— **停等已移除**

| 项 | 状态 |
|----|------|
| `render_to()` | 可返回 out-fence（`RenderSync::out_fence_fd`）而不等 fence；未完成的提交挂在 `in_flight` 列表上按 fence 回收 |
| KMS 提交 | 该 out-fence 作为 primary plane 的 `IN_FENCE_FD` 交给 `drmModeAtomicCommit` |
| 反向 | atomic 的 `OUT_FENCE_PTR` 回来，经 `Output::take_present_fence()` → `ScanoutTarget::set_acquire_fence()` 喂给下一帧渲染 |
| 纹理导入 | **已按 `wl_buffer` 缓存**：dmabuf 导入是客户端内存的实时视图，跨 commit 保留；shm 上传是快照，每次 commit 重传 |
| `commit_frame(pixels)` | CPU 读回路径仍在 —— 故意保留：无 dmabuf 的 GPU / headless / 嵌套后端要靠它 |

> **生命周期约束（新）：** Surface 缓存的 `GpuTexture` 属于 `VulkanRenderer`，
> 所以**渲染器必须在 Display 之前构造、之后析构**。两个示例都已按此排列并加了注释。

### 2.7 damage —— 多矩形 scissor + 遮挡剔除

`render_to()` 把 damage 折成不相交 `Region`，逐矩形 `setScissor` 后各画一次；
render pass 的 renderArea 取整个 region 的包围盒（一个 render pass 只能有一个）。
纹理按前后顺序累积 `GpuTextureFill::opaque`，被完全遮挡的表面一次都不采样。

| 项 | 天花板 |
|----|--------|
| `GpuTextureFill::opaque` | 单个矩形，不是 region —— 声明不透明区的客户端几乎都声明整个表面。已标 `ponytail:` |
| 描述符池 | 每帧新建。缓存 per-texture set 需要我们还没有的失效机制。已标 `ponytail:` |

### 2.8 会话与裸机限制

| 项 | 状态 |
|----|------|
| libseat 会话 / VT 切换 | **已实现**（`luminaria/session.hpp`）。DRM 后端在失活时 `drmDropMaster` 并暂停提交，恢复时重取 master + 重新 modeset；libinput 走 `libinput_suspend/resume` |
| 硬件光标平面 | **已实现**。每输出额外认领一个 `DRM_PLANE_TYPE_CURSOR`，独立 atomic 提交，移动指针不触发重绘 |
| 光标主题 | **已实现**（`luminaria/cursor_theme.hpp`）。自带 XCursor 解析器（含主题继承与动画帧），不依赖 libXcursor / X11 |
| 客户端 buffer 直出 | 未做 —— 全屏单窗口免合成 |
| 模式切换 | 未做 —— 固定用 connector 首选模式 |
| GPU 本身热插拔 | 未做 —— 只跟踪 connector 状态 |

### 2.9 截图/录屏（`src/types/screencopy.cpp`）

一处 `// no-op: unsupported`（不支持的捕获格式组合）。

### 2.10 layer-shell（`src/types/layer_shell.cpp`）

| 项 | 状态 |
|----|------|
| 锚点 / margin / exclusive zone 求解 | **已实现**（`arrange_layer_surface()`）：含 v5 的 `set_exclusive_edge`，角落锚定时按显式边消歧，锚定组合本身有歧义的按协议不预留 |
| 双缓冲状态 | **已实现** —— layer / anchor / size / margin / exclusive zone / keyboard interactivity 全部在 `wl_surface.commit` 上生效 |
| 协议错误 | 已发：`invalid_size`（0 宽高未锚定对边）、`invalid_anchor`、`invalid_keyboard_interactivity`、`invalid_exclusive_edge`、首次 configure 前贴 buffer 的 `already_constructed` |
| `keyboard_interactivity` | 库只**如实报告**客户端的请求，不代 compositor 决定焦点。`exclusive`（锁屏 / 密码框）由谁独占键盘，是 compositor 的策略 |
| `get_popup` | 只发 `LayerSurface::new_popup` 信号 —— 库不替 xdg_popup 换父级。面板菜单的定位要 compositor 自己按 layer surface 的位置摆（`Popup::parent_surface()` 对这种 popup 是 null） |
| 输出选择 | `output_resource()` 原样交出客户端要的 wl_output（可为 null = 由 compositor 挑）。库不做"最近使用的输出"这类策略 |

### 2.11 foreign-toplevel（`src/types/foreign_toplevel.cpp`）

| 项 | 状态 |
|----|------|
| 列表自维护 | **已实现** —— `track(shell)` 后跟着 `Toplevel` 的 map / unmap / destroy / identity_change / **state_change** / parent_change 走；晚绑定的客户端拿全量列表 |
| `set_rectangle` | **校验后丢弃**（负尺寸发 `invalid_rectangle`）。它是最小化动画飞向任务栏按钮的目标矩形，本库不做动画，存下来也没人用 |
| `output_enter/leave` | 要 compositor 调 `set_output()` 才有 —— 窗口在哪块屏上是 compositor 的布局知识，库看不到 |
| minimized 状态 | 借 `Toplevel::set_minimized()` 记账。xdg-shell 没有 minimized 状态，所以这一位**不下发给客户端**，只用于窗口列表与 compositor 自己 |

### 2.12 xdg-activation（`src/types/xdg_activation.cpp`）

| 项 | 状态 |
|----|------|
| token 一次性 | **已实现** —— 无论 compositor 认不认，`activate` 一到就作废，杜绝重放 |
| token 强度 | 128 位，来自 `getrandom(2)`（内核 CSPRNG）。不用 `<random>` —— 那些发生器看够输出就能预测，而能猜到 token 就等于能随时抢焦点。取不到熵时干脆不记录该 token，发个永远兑不出来的空串 |
| serial 校验 | **不做** —— 库没有"这个客户端最近收到过哪些 serial"的账。`ActivationTokenRequest` 把 seat + serial 原样交给 compositor，`granted` 默认 true。防焦点窃取要 compositor 自己核对 |
| 未兑换 token | 最多存 32 个，超了丢最旧的。要了 token 又不用的客户端不会把内存吃光，代价是极端情况下老 token 提前失效 |

### 2.13 relative-pointer（`src/types/relative_pointer.cpp`）

| 项 | 状态 |
|----|------|
| `get_relative_pointer` 的 `pointer` 参数 | **忽略** —— 库只有一个 seat，客户端的哪个 `wl_pointer` 与"哪个客户端"是同一件事，路由按客户端做 |
| 事件路由 | 只发给持有**指针焦点**的客户端；没有焦点时 `send_motion()` 静默丢弃 |
| delta 来源 | 加速后与未加速两对值都照单转发，库不自己算加速曲线 —— 只有一对可用的后端应当把同一对传两次 |

### 2.14 pointer-constraints（`src/types/pointer_constraints.cpp`）

| 项 | 状态 |
|----|------|
| 激活门槛 | **表面无指针焦点时 `activate()` 静默拒绝**，焦点一走自动 `deactivate()` —— 写在库里而不是留给 compositor，因为忘了就是"任意客户端可劫持鼠标" |
| 逃生快捷键 | **不提供** —— 按哪个键解开锁定是 compositor 的策略，库不替它选 |
| `already_constrained` | 已实现（同一 surface + pointer 第二次约束是致命错误） |
| region / cursor hint | 双缓冲，随 `wl_surface.commit` 生效；创建请求里带的初始 region 立即生效 |
| 光标实际的钉住 / 限制 | **不做** —— 库不拥有光标位置。`active_constraint()` 告诉 compositor 该不该动光标、`region()` 给出边界，真正的钳位在 compositor 的指针处理里 |

### 2.15 text-input-v3（`src/types/text_input.cpp`）

| 项 | 状态 |
|----|------|
| 双缓冲状态 | 已实现（两侧都是）：客户端的 enable / 周边文本 / 内容类型 / 光标矩形随 `commit` 生效，我们的 preedit / commit_string / delete_surrounding_text 随 `send_done()` 生效 |
| done serial | 等于收到的 `commit` 次数，符合协议要求 |
| 焦点 | 跟随 seat 键盘焦点；`leave` 之后到下一次 `enter` 之前，该对象的请求一律忽略 |
| `input-method-v2` | **未实现** —— 本库只终结 text-input 一侧。接 IBus / Fcitx 或实现 input-method global 是 compositor 的事，这是 P1 剩下的唯一缺口 |
| "同一 seat 上只允许一个 text input 处于 enabled" | **不强制** —— 协议说重复 `enable` 应被忽略，实际工具包不会这么干；`focused()` 取第一个 enabled 的 |

### 2.16 idle-inhibit（`src/types/idle_inhibit.cpp`）

| 项 | 状态 |
|----|------|
| 可见性 | inhibitor 默认 `visible = true`，`inhibited()` 只数可见的。表面被最小化 / 切到别的工作区时由 compositor 调 `set_visible(false)` —— 库看不到什么是"可见" |
| `changed` 信号 | 只在跨越 0 的那一刻发，可以直接接息屏计时器 |
| `ext-idle-notify-v1` | **未实现** —— 反方向的协议（告诉客户端"用户闲了 N 秒"），与本条正交 |

### 2.17 data-control（`src/types/data_control.cpp`）

| 项 | 状态 |
|----|------|
| 权限 | 这个 global 按设计绕过"选区跟随焦点"。默认**发给所有客户端**；`set_filter()` 可以只发给受信任的 |
| filter 的实现 | 用 libwayland 的 `wl_display_set_global_filter`，而 wl_display **只有一个** filter 槽且拿不回旧值 —— 所以无法链式叠加。compositor 若自己要 filter，应当把 data-control 的判断写进自己那个里，而不是调 `set_filter()` |
| 陈旧 offer | 选区易主时旧 offer 立即作废，之后 `receive` 只是把管道关掉。否则剪贴板管理器会把新内容当成它手里那份旧的 |
| `finished` 事件 | **不发** —— 它用于 compositor 中途收回权限，本库没有运行时撤权的入口 |
| 桥接方向 | 写入方向经 `SelectionSource` 进 `DataDeviceManager` / `PrimarySelectionManager`；粘贴的客户端看到的是普通 offer，分辨不出源不是 `wl_data_source` |

---

## 三、汇总

| 类别 | 数量 |
|------|------|
| TODO 注释 (`.cpp` / `examples/`) | 1 |
| TODO 注释 (`.hpp`) | 3 |
| 空操作桩函数 | **0** |
| **总计标记点** | **~4**（上一轮 ~26） |

### 按影响面分类

| 影响 | 剩余项 |
|------|--------|
| 不影响客户端运行 | 无 |
| 客户端可感知但有降级 | 未设置 `set_popup_constraint_query()` 时贴边菜单溢出 |
| 性能天花板 | `Region` 不合并相邻矩形；`opaque` 只取包围盒；描述符池每帧新建 |
| 裸机限制 | 无模式切换、无客户端 buffer 直出、GPU 设备热插拔不处理、键码原始值 |
| 未实现的协议 | `input-method-v2`（IME 的输入法一侧）、`ext-session-lock-v1`、`ext-idle-notify-v1`、output-management / gamma-control、tablet-v2 / pointer-gestures |

---

## 四、与 README 路线图对照

**P0 与 P1（`input-method-v2` 除外）已全部为 ✓。** 本文档剩余的项对应：

| README 项 | 对应 |
|-----------|------|
| ✓ damage tracking (P1) | §2.7 多矩形 scissor + 遮挡剔除已落地；粒度天花板见表 |
| ✓ HiDPI / fractional-scale (P1) | §2.1 buffer scale/transform + viewporter + fractional-scale 全齐 |
| ✓ `xdg-decoration` (P1) | 服务端 global 已实现；库不画装饰，默认答 client-side |
| ✓ libseat (P1) | §2.8 |
| ✓ 光标 (P1) | §2.8 主题加载 + 硬件平面 |
| ✓ explicit sync（非协议部分） | §2.5 / §2.6 全异步 |
| △ IME (P1) | §2.15 —— `text-input-v3` 已做，`input-method-v2` 未做 |
| ✓ relative-pointer / pointer-constraints (P2) | §2.13 / §2.14 |
| △ idle (P2) | §2.16 —— `idle-inhibit` 已做，`ext-idle-notify` 未做 |
| ✓ data-control (P2) | §2.17 |
| △ XWM（非协议部分） | `xwayland.hpp` TODO：minimal |
