# 功能矩阵

> 已实现并有测试覆盖的东西，按层列出。未完成的工作见 [../TODO.md](../TODO.md)。

（下表的"测试"列是 `tests/test_*.cpp` 的名字，`xmake test test_<名>/*` 可单独跑。）

## 核心底层

| 模块 | 内容 | 测试 |
|---|---|---|
| core | `Result<T>` / `Error`、`CUnique` 句柄 RAII、`Signal<Event>` + RAII `Connection`（emit 期间 connect/disconnect 安全）、`Display`、`EventLoop` + `EventSource` | signal, core |
| core | `SurfaceId{index,generation}` + `surface_from_id()`：销毁后清槽并推进代际，槽复用也不会让旧身份指向新客户端；焦点、拖放、帧摆位、纹理缓存和直出 hold 全部按 id 留存 | surface-handle, seat-input, dnd-surface-destroy, frame |
| util | `Box`、`Color`、`Pixel`、`Rect`（constexpr） | box |
| 测试防线 | 常态化畸形协议流客户端：48 条固定种子状态机交错 surface / region / buffer 生命周期，覆盖非法 scale / transform、重复 xdg role、自父 subsurface 与 libwayland 会放行的短 stride | protocol-fuzz |

## 协议对象（服务端）

| 模块 | 内容 | 测试 |
|---|---|---|
| 协议 | `wl_compositor` v6 + `wl_surface` — **全部请求实现，无空操作**：attach / commit / frame、damage + damage_buffer（按 buffer scale/transform 反算）、`set_opaque_region` / `set_input_region`（真 region，决定遮挡剔除与命中测试）、`set_buffer_scale` / `set_buffer_transform`、`offset`；`preferred_buffer_scale` / `preferred_buffer_transform` 事件。`frame` 回调**推迟到实际上屏**而非 commit | compositor, frame-timing, surface-state |
| 协议 | `wl_region` — `add` / `subtract` 由不相交矩形集实现（`region.cppm`），不是空操作 | region |
| 协议 | `wl_shm` buffer → RGBA 读回（ARGB8888 + XRGB8888） | client-texture |
| `luminaria.gpu` 协议 | `linux-dmabuf-unstable-v1` (v3) + GBM 分配器 — GPU 客户端 dmabuf buffer 导入（ARGB8888 / XRGB8888，**任意 GPU modifier**）；广告 GPU 实际支持的 modifier 列表。合成路径**零拷贝**：`Frame` 的 GPU bridge 经 Vulkan external-memory 直接把客户端 dmabuf 变成 `GpuTexture`，不落 CPU（CPU RGBA 读回只留给 screencopy / 老式 shm buffer） | dmabuf, gpu-scanout |
| 协议 | `xdg_wm_base` v5 / `xdg_surface` / `xdg_toplevel` 全生命周期（配置握手 → map/unmap）；窗口状态机：maximize / fullscreen / activated / resizing 随 configure 下发，title / app_id / min-max size / window geometry 全部记录，交互式 move/resize 与 minimize 以信号交给 compositor 仲裁；`configure_bounds`（v4）+ `wm_capabilities`（v5） | xdg, toplevel-state |
| 协议 | `xdg_popup` + `xdg_positioner` v3 — 菜单 / tooltip / 下拉框：anchor / gravity / offset 完整求解，**`constraint_adjustment` 真正施加**（flip → slide → resize，逐轴独立；父窗口位置由 `XdgShell::set_popup_constraint_query()` 提供），`grab`、`reposition` + `repositioned`、父级销毁时级联 `popup_done` | popup |
| 协议 | `zxdg_decoration_manager_v1` — 谁画标题栏。客户端问、compositor 答，答案有约束力。库不画装饰，所以默认答 client-side（无框窗口比双标题栏更糟） | scaling |
| 协议 | `wp_viewporter` — 裁剪 + 拉伸一块 buffer。视频播放器免重编码加黑边；小数缩放的客户端用它声明"我这块整数 buffer 代表多大的逻辑尺寸" | scaling |
| 协议 | `wp_fractional_scale_v1` — 输出的真实缩放，以 120 分之一为单位（180 = 1.5x）。`wl_output.scale` 是整数，说不出 150% | scaling |
| 协议 | `wl_subcompositor` / `wl_subsurface` — 子表面树、相对定位、`place_above`/`place_below` 堆叠、**sync/desync 语义**（同步子表面的 commit 缓存到父级 commit 时原子提交）；`Surface::surface_tree()` / `surface_at()` 供渲染与命中测试共用 | subsurface |
| 协议 | `wl_seat` v5 — 键盘（xkb keymap）+ 指针 + 触摸，焦点 enter/leave + 事件路由；滚轮（平滑 axis / 离散 notch / axis_stop）、`set_cursor`（发信号给 compositor 合成光标；**"客户端要求不显示指针"与"客户端没表态"是两种状态**，`cursor_hidden()` 分开报告，合成器据此决定画不画自己的箭头；光标表面自动标记为 `input_transparent()` —— 它就画在指针上，不排除的话指针下面的命中测试永远命中光标自己）；**焦点安全**：键盘/指针/触摸/光标都留 `SurfaceId`，每次发送前解析，被销毁的聚焦表面自动失效 | seat, seat-input |
| 协议 | `wl_data_device_manager` v3 — 剪贴板（选区随键盘焦点转移）+ 拖放（drag 期间 seat 把指针交给 data device，enter/motion/drop 全流程，dnd actions）；数据经管道在客户端之间直传，compositor 不读内容 | data-device, dnd |
| 协议 | `zwp_primary_selection_device_manager_v1` — X11 式中键粘贴选区 | data-device |
| 协议 | `wp_single_pixel_buffer_v1` — 1×1 纯色 `wl_buffer`，无 shm pool、无 GPU 分配。客户端画背景/压暗层/黑边不必再为一块纯色开整屏 buffer | single-pixel |
| 协议 | `wp_presentation`（presentation-time v2）— 帧真正上屏的时刻 + 刷新周期，时间戳直接来自 KMS 的 vblank（CLOCK_MONOTONIC，`hw_clock` 标志）。动画不再靠猜时间 | frame-timing |
| 协议 | `wp_tearing_control_v1` — 客户端（游戏）请求不等 vblank 直接上屏；hint 是双缓冲 surface 状态，DRM 后端转成 `DRM_MODE_PAGE_FLIP_ASYNC` | tearing |
| 协议 | `wp_fifo_v1` — 客户端说「这帧要等上一帧真的上了屏才生效」，也就是 swapchain 的 FIFO 模式。语义落在 `Surface` 的提交闸门上：`wait_barrier` 的那次 commit 被**整个扣住**——不发 commit 信号、不产生 damage、不排帧回调——直到 barrier 清除。清除点就是 `Surface::send_frame_done()`，所以只要 compositor 按规矩在 present 里答帧回调，FIFO 不需要额外接线。不可见的表面得由 compositor 用 `FifoManager::unblock_hidden()` 放行，否则最小化的客户端会永久卡住 | fifo |
| 协议 | `wp_commit_timing_v1` — 另一半节拍：直接指定这次 commit 最早何时生效（CLOCK_MONOTONIC）。同一个闸门，多一个时间条件；到点的唤醒由这个 global 自带的 EventLoop 定时器发起（按最近的截止时间重整），compositor 除了创建 global 什么都不用做 | commit-timing |
| 协议 | `wp_content_type_v1` — 客户端一句「我在放视频 / 我是游戏」。compositor 自己推不出来的东西：据此选刷新率、决定要不要直出、要不要别息屏。粘性双缓冲状态，读 `Surface::content_type()` | content-type |
| 协议 | `wp_cursor_shape_v1` (v2) — 客户端说要哪种光标（`text` / `ns-resize` / …）而非自带位图；36 种 shape 全部映射到 XDG 光标名，交给 compositor 画 | cursor-shape |
| `luminaria.desktop` 协议 | `ext_workspace_v1` — 工作区列表 / 切换（面板、pager）。服务端拥有工作区集合，客户端只能请求；group ↔ output 关联、state（active/urgent/hidden）、请求经 `commit` 批量下发 | workspace |
| 协议 | `zwlr_layer_shell_v1` (v5) — 桌面自己的表面：面板 / 状态栏 / 壁纸 / 通知 / 锁屏层。四个 layer 各有 z 序，锚定到输出边缘；exclusive zone 从可用区里切出一条谁也不许盖的带子。layer / anchor / size / margin / exclusive zone 全是双缓冲状态，随 `wl_surface.commit` 生效并发 `state_change`；映射流程与 xdg-shell 同规矩（先无 buffer 提交、configure、再贴 buffer）。`arrange_layer_surface()` 一次算完摆放 + 收缩可用区 + 下发 configure —— 这段锚点算术每个 compositor 都得写一遍，写错就是面板互相压 | layer-shell |
| `luminaria.desktop` 协议 | `zwlr_foreign_toplevel_management_v1` (v3) — 任务栏 / 窗口切换器看到的窗口列表。不用手工发布：`track(shell)` 之后自维护，窗口 map 时进列表，title / app_id / 状态跟着 toplevel 自己的信号走，unmap 或销毁时消失，晚启动的任务栏照样拿到全量列表；客户端的 activate / minimize / maximize / fullscreen / close 经 `request()` 交给 compositor 仲裁。注意这个 global 让一个客户端能枚举并操作**所有别人的**窗口，只该给桌面自家组件 | foreign-toplevel |
| 协议 | `xdg_activation_v1` — 「把那个窗口提到前面」。两步握手：有焦点的客户端拿 token（附 seat + 输入 serial），带外（环境变量 / D-Bus）传给另一个客户端，后者拿 `activate` 换焦点。token 随机 128 位、一次性；compositor 在 `new_token` 里核对 serial 决定发不发 —— 这就是它防焦点窃取的全部机制，`request_activate` 仍只是请求，给不给焦点是 compositor 的事 | xdg-activation |
| 协议 | `zwp_relative_pointer_manager_v1` — 指针的**位移**而非位置。光标被锁住不动时 `wl_pointer.motion` 一个都不会发，游戏/3D 视口就是靠这条通道看见鼠标在动；加速后与设备原始两套 delta 都给，后者是游戏要的。事件只发给持有指针焦点的客户端 | relative-pointer |
| 协议 | `zwp_pointer_constraints_v1` — 锁定（locked，光标钉死不动）与限制（confined，光标能动但出不了表面/区域）。客户端只能**请求**，`activate()` 之前一律无效，且**表面没有指针焦点时拒绝激活** —— 这条规则写在库里，compositor 想忘也忘不掉；焦点一走自动解除。region / cursor position hint 是双缓冲状态，随 `wl_surface.commit` 生效 | pointer-constraints |
| 协议 | `zwp_text_input_manager_v3` — 中文/日文输入法的通道。客户端侧状态（enable、周边文本、内容类型、光标矩形）双缓冲，`commit` 时整体生效；我们回的 preedit / commit_string / delete_surrounding_text 同样攒到 `send_done()` 才发，done 的 serial 就是收到的 commit 次数。焦点跟随 seat 键盘焦点，不由客户端选。本库只终结协议，接 IBus/Fcitx 或 input-method-v2 是 compositor 的事 | text-input |
| `luminaria.desktop` 协议 | `zwp_input_method_v2` — text-input 的**另一端**：IBus / Fcitx / 屏幕键盘作为普通客户端接上来，库在中间把两边抄通。焦点文本框 enable → 输入法收到 `activate` + 周边文本 + 内容类型 + `done`；输入法回的 preedit / commit_string / delete_surrounding_text 攒在文本框上、由它的 `commit` 一次生效；`commit` 的 serial 对不上就丢弃（那是对着旧状态算的）。两件事仍归 compositor：**键盘 grab** 的路由（`keyboard_grab()` 非空时按键喂给它而不是 seat，keymap 已经替你发了）和候选窗的摆放（`InputPopupSurface` 带光标矩形）。输入法能看见所有客户端的每一次按键，所以它属于 desktop module | input-method |
| 协议 | `ext_idle_notifier_v1` (v2) — 反方向：告诉客户端「用户已经 N 毫秒没动了」。锁屏器要 10 分钟、调光要 30 秒，各自一个独立定时器互不干扰。库看不见输入，所以活动信号由 compositor 调 `notify_activity()` 送进来；v2 的 `get_input_idle_notification` 与普通的差别只有一条——它**无视 idle inhibitor**，把 `IdleInhibitManager::changed` 接到 `set_inhibited()` 上，两种语义就都对了 | idle-notify |
| 协议 | `zwp_idle_inhibit_manager_v1` — 「正在放视频，别息屏」。协议本身零回程流量，全部意义在服务端：`inhibited()` 只统计**可见**的 inhibitor（`set_visible(false)` 表示表面被最小化/切走了），`changed` 只在跨越 0 的那一刻发一次，可以直接接到息屏计时器上 | idle-inhibit |
| `luminaria.desktop` 协议 | `zwlr_data_control_manager_v1` (v2) — 没有窗口的剪贴板：`wl-copy` / `wl-paste` / 剪贴板历史工具要在没有焦点、没有 surface 的情况下读写选区。两块剪贴板都覆盖（普通 + 中键）。写入方向经 `SelectionSource` 桥接进 `DataDeviceManager`，粘贴的客户端看到的就是一个普通 offer，分辨不出源不是 `wl_data_source`。选区易主时旧 offer 立即作废，免得剪贴板管理器把新内容当旧的。**这个 global 绕过了「选区跟随焦点」这条安全规则**，`set_filter()` 可以只发给受信任的客户端 | data-control |
| `luminaria.desktop` 协议 | `ext_session_lock_v1` — 锁屏。安全属性是**失败向关**：`send_locked()` 之后客户端才能认为屏幕被盖住了；锁屏客户端崩了不会解锁——`SessionLockDestroy::unlocked` 区分「客户端 unlock_and_destroy」与「客户端死了」，后者 `session_locked()` 保持为真，compositor 该继续黑屏、继续吞输入。锁定表面必须一屏一个（duplicate_output）、必须先 ack configure 再提交（commit_before_first_ack）、必须提交非空且尺寸严格相符的 buffer（null_buffer / dimensions_mismatch）——四个协议错误都发，因为每一个都是锁屏上的一个洞。同时只有一个锁，第二个直接答 `finished` | session-lock |
| 协议 | `linux-drm-syncobj-v1` — 显式 GPU 同步，**全异步、无 CPU 等待**：acquire point 导出成 sync_file 交给渲染器当 `VkSemaphore` 等；渲染的 out-fence 直接写进客户端的 release point，客户端在 GPU 停止读取的那一刻就能复用 buffer | syncobj |
| 协议 | `wl_output` v4 — geometry / mode / scale / name / description / done（客户端 map 前需要；`name` 是 `grim -o` 等工具寻址输出用的）。`set_scale()` / `set_transform()` 会重发几何信息并以 `done` 收尾 | output-scale |
| 协议 | `xdg-output-unstable-v1`（`zxdg_output_manager_v1` v3）— 输出的**逻辑**位置与尺寸（mode ÷ scale，旋转时长宽互换），随 scale/transform/位置变化实时更新。wl_output 只描述物理模式；要把截图摆到画布上的工具（grim / slurp / 录屏器）读的是这个。缺了它 `grim` 只会警告并写出 0×0 的 PNG | — |
| 协议 | 截图/录屏 — `wlr-screencopy-unstable-v1` (v3) + `ext-image-copy-capture-v1` + `ext-image-capture-source-v1`：客户端捕获整块输出到 **wl_shm 或 dmabuf** buffer（`grim` 截图、`wf-recorder` 录屏），逐帧回调 GPU 合成结果；dmabuf 目标 LINEAR 走 mmap，tiled 走 Vulkan 导出，ext 路径广告 dmabuf device + modifier。**光标捕获会话**（`create_pointer_cursor_session`）：把指针作为独立捕获源，录屏可以把光标排除在视频外、播放时再按自己的帧率合成 —— enter/leave、position、hotspot 齐全，buffer 约束按光标自己的尺寸而非输出尺寸广告；compositor 经 `set_cursor_source()` 提供像素、`notify_cursor_changed()` 通知移动；未注册光标源时会话仍然正常创建并立刻发 `stopped`（此前是静默 no-op，客户端下一个请求就会因 invalid object 被踢） | cursor-capture, dmabuf |

## 渲染

| 模块 | 内容 | 测试 |
|---|---|---|
| render | Vulkan-Hpp RAII：纯色背景、矩形填充、客户端纹理合成（真 GPU 读回验证） | vulkan, composite, texture |
| render | 纹理裁剪 — 部分越界的 surface 只渲染可见部分（之前跨输出边缘直接丢弃） | texture |
| render | **GPU 合成链**：`GpuTexture`（客户端 dmabuf 零拷贝导入 / shm 上传一次）→ `render_to()` 直接合成进 `ScanoutTarget` —— 一块用 `VK_EXT_image_drm_format_modifier` 分配、再导出成 dmabuf 的 Vulkan 渲染目标。整条链没有一次 CPU 读回 | gpu-scanout |
| present | `Output::import_scanout(dmabuf)` / `commit_scanout(id, in_fence)` —— 渲染目标即 KMS framebuffer（`drmModeAddFB2WithModifiers`），双缓冲 atomic 翻页；渲染 out-fence 作为 `IN_FENCE_FD` 交给显示硬件，`OUT_FENCE_PTR` 经 `take_present_fence()` 反向喂回下一帧 | gpu-scanout |
| present | `Output::set_cursor/move_cursor/hide_cursor` —— **硬件光标平面**：移动指针不重绘任何东西。光标状态优先折进下一次 primary plane 翻页；屏幕空闲时（按需帧下根本没有下一次翻页）后端自己用屏上那块 fb 再提交一次，只更新 cursor plane，并靠 `flip_pending` 压到刷新率 | drm（需 tty） |
| present | `Output::present` 信号 —— 帧上屏时刻（DRM 给真 vblank 时间戳 + 序号，其余后端给 CLOCK_MONOTONIC）。`wl_surface.frame` 与 `wp_presentation` 都由它驱动 | frame-timing |
| render | **damage 渲染**：`render_to(..., damage)` 把脏区折成不相交 `Region`，**逐矩形 `setScissor`** 各画一次 —— 两块分散的小脏区就是两个小 scissor，不是横跨它们的大矩形。未触及的像素原样保留；双缓冲下调用方需并上上一帧的 damage（buffer age） | frame-timing, gpu-scanout |
| render | **离屏合成**：`OffscreenTarget` 是可写也可采样的 RGBA GPU image；`render_offscreen()` 和 `render_to()` 共用 pass/遮挡/damage 路径，写完就停在 shader-read layout，可直接作为下一趟 `GpuTextureFill`。整窗淡入淡出不会把重叠子表面混合两次（ADR 0005） | offscreen |
| render | **预乘 alpha**：`GpuTextureFill::alpha` 作用于整张 quad，片元颜色与 alpha 一起缩放，再由 `ONE / ONE_MINUS_SRC_ALPHA` 混合；半透明内容正确叠加，离屏的整窗结果可一次淡入淡出 | offscreen |
| render | **遮挡剔除**：`GpuTextureFill::opaque` 声明的不透明区从前往后累积，被完全盖住的表面一次都不采样 —— 最大化窗口下的壁纸不进 GPU | gpu-scanout |
| render | **异步提交**：`RenderSync` 让 `render_to` 等一组 sync_file（客户端 acquire point）并吐出 out-fence，自己不阻塞；未完成的提交挂在 in-flight 列表上按 fence 回收 | gpu-scanout |
| render | **纹理缓存**：`Frame` 的 GPU bridge 按 `SurfaceId` + `wl_buffer` 缓存。dmabuf 导入是客户端内存的实时视图，跨帧保留；shm 上传是快照，客户端重绘时才重传；表面销毁后旧 id 立即失效，旧 placement 再 submit 也不会碰悬空纹理 | frame, texture-cache |
| render | **光标主题**：自带 XCursor 解析器（`cursor_theme.cpp`），读 `/usr/share/icons/<主题>/cursors`，支持主题继承与动画帧。不依赖 libXcursor / X11 | cursor-theme |
| render | **纹理四边形管线**（取代原先的 blit）：每个 surface 一次 draw，位置/UV 全走 push constant。由此一并拿到 **整数 scale**、**8 种 transform 全部（含 90/270 转置）**、以及**真正的 alpha 混合**（预乘 `ONE`/`ONE_MINUS_SRC_ALPHA`）。SPIR-V 由 `glslangValidator --vn` 编进二进制 | gpu-scanout |
| present | `Output::commit_frame(pixels)` 呈现渲染帧（headless 存帧，DRM 写 dumb buffer）—— 保留给无 dmabuf 的降级路径 | render-output |
| render | **描述符集按纹理缓存**：一个纹理绑定的 image view 终生不变，所以描述符集只在它第一次被画时写一次，之后每帧只是 `bindDescriptorSets`。此前是每帧新建一个描述符池、每个 fill 分配并重写一次描述符 —— 一屏没动过的窗口每秒重写六十遍。纹理销毁后描述符集进空闲表复用，但要等到所有可能仍在采样它的提交都完成（按提交序号比对 in-flight 队列），否则会改掉正在飞行中的那一帧 | texture-cache |
| render | **每帧零堆分配**：`render_to()` 的 region 是渲染器自己的 scratch，framebuffer 按 render pass 挂在 `ScanoutTarget` 上建一次，command buffer 与 fence 走按提交 fence 回收的空闲表，连导入信号量的那个 vector 都跟着提交槽复用。`Region` 的 subtract/intersect 也改成两块缓冲乒乓——原先每次都是一次分配加一次释放，而遮挡剔除每个表面要做好几次 | render-alloc |
| render | `read_scanout()` —— 需要 CPU 像素时（嵌套 wl_shm 呈现、screencopy）从渲染目标**自己的 VkImage** 拷出，暂存 buffer 建一次并常驻映射，优先要 HOST_CACHED 内存。之前每帧重新 import 一遍自己的 dmabuf 再从非缓存内存逐像素读，800×600 要 90ms —— 指针拖动卡顿就是它 | gpu-scanout |

## 后端

| 模块 | 内容 | 测试 |
|---|---|---|
| backend | 抽象 `Backend` + `HeadlessBackend`（软件帧泵，无 GPU/显示） | headless |
| backend | **按需帧**：`Output::schedule_frame()` 要一个 `frame` 事件，一次一个；没人要就不发。提交本身**不**隐含下一帧，所以静止的屏幕不订阅 vblank、不叫醒进程。DRM 的帧来自翻页完成（空闲时改由 1ms 定时器补一次），嵌套的靠一次空 commit 向父混成器要 frame callback（拿的还是父的 vblank 节奏），headless 的定时器只管节流不再自续 | idle-frame, idle-wake |
| backend | `WaylandBackend`（嵌套）：连父 compositor 开窗；**零拷贝呈现** —— 绑父 compositor 的 `zwp_linux_dmabuf_v1` (v3)，把渲染目标的 dmabuf 用 `create_immed` 包成 wl_buffer 直接 attach，合成帧一个像素都不经过 CPU；父 compositor 没有该 global 时 `scanout_modifiers()` 返回空表，调用方退回 wl_shm 路径；**转发父 compositor 输入**（指针 enter/leave/motion/button、滚轮 axis/discrete/stop 按 `wl_pointer.frame` 聚合、键盘按键 + 修饰键），经命中测试路由到 seat；**原生窗口装饰**：以 `xdg-decoration-unstable-v1` 客户端身份向父 compositor 请求 server-side 装饰（附 title + app_id），拿到宿主桌面的真标题栏，协商结果由 `decoration_mode()` 报告 | wayland-nested |
| backend | `DrmBackend`（裸机 KMS）：**多输出 + 热插拔** —— 每个已连接 connector 各分一套 CRTC + primary plane，各自 modeset/翻页；udev netlink 监听 `drm` 子系统的 `HOTPLUG=1` 事件后重扫 connector 并做差集，新显示器发 `new_output`、拔掉的发 `Output::destroy`（并还原它自己的 CRTC）；**atomic 模式设置**（connector CRTC_ID + CRTC MODE_ID/ACTIVE + primary plane 全套 property，一次 `drmModeAtomicCommit`）、非阻塞翻页 + `page_flip_handler2` vblank 帧泵；scanout buffer 走 dmabuf 导入（`IN_FORMATS` 交出硬件真实支持的 modifier 列表），dumb buffer 仅作降级路径（真机未验证，测试 skip） | drm（需 tty） |
| backend | `LibinputBackend`（裸机输入）：发出 KeyEvent / PointerMotion / PointerButton / **PointerAxis**（滚轮 v120 转 notch + 触摸板平滑增量 + 抬指 stop）/ **ModifiersEvent** 信号；自带 xkb 状态机（libinput 只给键码，Shift 按没按只有 keymap 答得出），`keymap()` 交给 `Seat::set_keymap()` 两边就对齐；**设备能力**按已开设备聚合（keyboard/pointer/touch），增删设备时发 `capabilities_changed`，直接喂 `Seat::set_capabilities()`；`create_path()` + `add_device()` 按路径开指定设备（可测，也给自管设备表的 compositor 用）；给了 `Session` 就经 libseat 开设备，VT 切换时 `libinput_suspend/resume` | libinput, libinput-uinput（需 input 组 / root） |
| session | `Session`（libseat）—— 谁现在有权碰 GPU 和输入设备。VT 切走时 DRM 后端 `drmDropMaster` 并停止提交，切回时重取 master + 重新 modeset。没有它也能从已登录 VT 跑，只是切 VT 不安全 | session（需 seat） |

## 布局

| 模块 | 内容 | 测试 |
|---|---|---|
| layout | `OutputLayout` —— 输出在全局坐标系里的位置：`add_auto()` 横向排列、`box_of()` / `bounds()` / `at(x,y)` 命中、`intersecting(box)` 求窗口跨屏时各屏该画哪一块。用的是**逻辑尺寸**，所以旋转/HiDPI 输出占的格子跟它的 mode 不一样 | output-layout |
| layout | `Transform` + `Output::scale()` —— 每输出旋转/翻转（值与 `WL_OUTPUT_TRANSFORM_*` 一致）与整数缩放。`transform_box()` 是逻辑坐标 → 帧缓冲像素的唯一映射，`transform_invert()` 供输入反向映射 | output-layout, output-scale |

## 外壳层 / Xwayland / 示例

| 模块 | 内容 | 测试 |
|---|---|---|
| shell | `Frame` —— 每输出的帧账本：`begin`/`place` 排摆位（画与命中测试同一份 `SurfaceId` 列表）、`surface_at()` 返回代际身份、`submit()` 一手包办直出判断 / damage 记账（含 buffer age 债务）/ fence 编排 / 翻页，`read_back()` 供截屏。稳态重排零堆分配 | frame |
| shell | **摆位串 diff 出 damage** —— `submit()` 把这一帧的摆位串与上一次上屏的那串逐位比较（位置进身份，所以换 z 序也算变），差异处旧矩形与新矩形各记一笔。窗口开 / 关 / 移动 / 缩放 / 换层次没有任何客户端会报 damage，但全写在摆位串里，所以混成器只欠一次 `invalidate()` 唤醒；移动一个窗口的代价是它跨过的两个矩形而非一屏。`damage_all()` 退为钝器（底色变了、直出后合成缓冲失效） | frame-damage |
| shell | **无 damage 不提交** —— `submit()` 发现这一帧与屏上那一帧完全一致时答 `unchanged`：不渲染、不翻页、不轮换缓冲，输出随即安静。唤醒是 `Frame` 自己的事——它盯着自己画过的每个表面的 commit，`invalidate()` / `damage_all()` 与 `reset()` 也各要一帧；混成器只需在收到 `unchanged` 时补发帧回调（那一帧没有 `present`） | idle-wake, frame |
| xwayland | 启动 Xwayland + 最小 XWM（xcb 连接、重定向 root、map/configure） | xwayland |
| example | `tinyluminaria`（嵌套/headless 参考 compositor）、`luminaria-drm-demo`、`luminaria-tty`（裸机 compositor） | tinyluminaria-smoke |
| 生命周期 | 关闭的窗口在下帧回收，无残留条目 | — |

---
