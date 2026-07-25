# TODO & Corners Cut — Luminaria 源码审计

更新于 2026-07-25（P1 完工后重新梳理，IME 除外）。列出源码中所有 `TODO` 注释与"偷工减料"处
（为免真实客户端崩溃而设的空操作桩，以及有已知天花板的简化实现）。

> **本轮变化：** 上一轮剩下的桩基本清空了。`wl_region`（add/subtract）、
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
| 未实现的协议 | IME（`text-input-v3` / `input-method-v2`）、layer-shell、session-lock |

---

## 四、与 README 路线图对照

**P0 与 P1（IME 除外）已全部为 ✓。** 本文档剩余的项对应：

| README 项 | 对应 |
|-----------|------|
| ✓ damage tracking (P1) | §2.7 多矩形 scissor + 遮挡剔除已落地；粒度天花板见表 |
| ✓ HiDPI / fractional-scale (P1) | §2.1 buffer scale/transform + viewporter + fractional-scale 全齐 |
| ✓ `xdg-decoration` (P1) | 服务端 global 已实现；库不画装饰，默认答 client-side |
| ✓ libseat (P1) | §2.8 |
| ✓ 光标 (P1) | §2.8 主题加载 + 硬件平面 |
| ✓ explicit sync（非协议部分） | §2.5 / §2.6 全异步 |
| ✗ IME (P1) | 唯一未做的 P1 项 |
| △ XWM（非协议部分） | `xwayland.hpp` TODO：minimal |
