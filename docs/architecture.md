# 架构

> 词汇以 [../CONTEXT.md](../CONTEXT.md) 为准；不可逆的设计决定记在 [adr/](./adr/)。

## 分层

自底向上，每层对应 `src/` 下的一个目录：

- **core** — `Display`（持有 `wl_display` + socket + 主循环）、`EventLoop` / `EventSource`、
  `Signal<Event>`、`Result<T>`、`CUnique`、`UniqueFd`。
- **backend** — `Backend` 发 `new_output`；`Output` 发 `frame` / `present` / `destroy` /
  `mode_changed`。四个实现：`HeadlessBackend`（软件帧泵）、`WaylandBackend`（嵌套在父
  混成器里）、`DrmBackend`（KMS atomic）、`LibinputBackend`（裸机输入）。`Session`（libseat）
  决定现在谁有权碰 GPU 和输入设备。
- **protocol** — 每个 Wayland global 一个分区，见 [features.md](./features.md)。
- **render** — Vulkan。GPU 路径：客户端 dmabuf 零拷贝导入成纹理 → 合成进一块导出为 dmabuf 的
  `ScanoutTarget` → KMS 直接扫描输出，全程无 CPU 读回。CPU 路径（`composite()`）保留给无
  dmabuf 的后端与截图。
- **shell** — 外壳层，即时模式，没有树（见 [ADR 0001](./adr/0001-no-retained-scene-graph.md)）。
  `Frame` 是每个输出一份的帧账本：混成器每帧 `begin(view)` + 逐窗口 `place()` 排出一串
  **摆位**，这串摆位既用来画也用来命中测试（`surface_at()`），所以点击不可能跟像素对不上；
  `submit()` 干完剩下的所有事——直出判断、遮挡与 damage 记账（含 buffer age 的跨缓冲债务）、
  fence 编排、翻页，返回 `Presented::{composited,scanout,unchanged,fallback}`。
  跨帧不保存任何窗口信息，只保存**内存**（所有 vector 只清空不释放，摆位的不透明区是指向
  帧内 arena 的下标区间而不是 `Region` 拷贝）与 damage 债务。
  另有 `OutputLayout`（输出在全局坐标系里的位置，逻辑单位）与 `DirectScanout`（全屏单窗口
  免合成的直出决策）。

## 一帧长什么样

`Output::frame` 触发 → `Frame::begin(view)` → 逐窗口 `Frame::place()` 排出这一帧的摆位 →
`Frame::submit(background)`。submit 内部：`Surface::buffer_texture()` 把客户端 buffer 放上
GPU → `VulkanRenderer::render_to(ScanoutTarget)` → `Output::commit_scanout()`；或者整个合成
被跳过，某个客户端的 buffer 直接进 CRTC。不能直出 dmabuf 的输出（headless、嵌套）照样在 GPU
上合成，只有成品帧过一次 CPU：`read_scanout()` → `commit_frame()`。输入反向走：后端输入信号
→ `Frame::surface_at()` → `Seat` 焦点与路由。

参考实现见 `examples/tty_compositor.cpp`（裸机）与 `examples/tinyluminaria.cpp`（嵌套/无头）。

## 几条会咬人的规矩

- **表面坐标不是 buffer 像素。** 客户端可以给更密的 buffer（`set_buffer_scale`）、旋转的
  buffer（`set_buffer_transform`）、或裁剪拉伸（`wp_viewporter`）。布局、命中测试、子表面
  偏移一律用 `surface_width()/surface_height()`；只有真的要碰像素时才用 `buffer_width()`。
- **帧回调是混成器的责任。** `wl_surface.frame` **不**在 commit 时应答——那样客户端会画出
  永远不上屏的帧。它攒着，直到混成器在 `Output::present` 里调 `Surface::send_frame_done()`。
  忘了调，所有客户端画完第一帧就冻住。damage 同理：`Surface::damage()` 一直累积到
  `clear_damage()`。
- **渲染器必须比 Display 活得久。** `Surface` 缓存的 `GpuTexture` 属于 `VulkanRenderer`，
  所以渲染器要声明在 `Display` 之前（局部变量逆序析构）。搞反了会在 `vk::raii` 里 abort。
- **绝不听客户端的话去索引内存。** 宽高、stride、offset 是客户端独立声明的四个整数，上游
  谁也没有按真正的单位交叉验证过。碰像素之前先过 `layout_fits()` / `layout_length()`。
- **缓存了裸 `Surface*` 就得订阅 `Surface::destroy`。**（这条正在被
  [ADR 0002](./adr/0002-surface-generational-handle.md) 的代际句柄取代。）`Frame` 的摆位列表
  不在此列——它不保留：需要时重排一遍（`begin` + `place`，零堆分配），不要跨 dispatch 持有
  `Placement`。

## 模块结构

没有 `include/`——这里没有任何东西是头文件，所以不存在「接口与实现分家」这回事。
`src/` 按职责分目录，分区名则是扁平的（跟文件名走，不跟路径走），所以挪动文件不构成 API 变化。

```
src/luminaria.cppm       主接口单元，只做 export import
src/core/                display event_loop expected handle signal
src/util/                box color dmabuf pixel rect_fill region transform
src/backend/             backend output input_event session drm headless libinput wayland
src/render/              vulkan cursor_theme + quad.{vert,frag}
src/shell/               frame output_layout direct_scanout
src/protocol/            25 个 Wayland global，一个一文件
src/xwayland/            X11 桥
src/detail/wayland_fwd.h 唯一剩下的头文件
```

每个 `.cppm` **接口与实现同文件**：先是 `export namespace luminaria { … }`，然后一条
`// --- implementation` 分隔线，之后是 pimpl 的 `struct X::Impl`、匿名 namespace 里的
协议胶水和成员定义。

分区之间必须**显式 `import` 且不能成环**。旧布局把这件事藏起来了——实现单元白拿整个主
接口，所以谁也不用声明依赖；现在 `seat.cppm` 想看见 `Surface` 就得写
`import :compositor;`。当前的依赖图是一个八层的 DAG，底层是 `box` / `signal` /
`expected`，顶层是 `data_control`；新加一条闭合成环的边会直接编译不过，这时候该做的是把
共用的类型下沉到第三个更低的分区里。

合并之后还有两条只有编译器会告诉你的规矩：**模块链接的声明不能在签名里出现匿名 namespace
里的类型**（`Impl::light_up(DrmOutput&)` 这种，`-WTU-local-entity-exposure`），把那个类型
提到 `namespace luminaria` 作用域即可，它仍然不导出；以及**实现部分的 namespace 级名字现在
全模块共享**，所以 `using Mgr = …` 这类别名要带分区前缀（`DcMgr` / `DdMgr` / `FtImpl` …），
而匿名 namespace 里的 `manager_bind` 们照旧各不相干。

用 xmake 而不是 Meson 的原因很实际：Meson 的模块依赖扫描器把输出名硬编码成 MSVC 的
`.ifc`（`mesonbuild/scripts/depscan.py`），GCC 下 ninja 直接报
`inputs may not also have inputs`，根本跑不起来。

---


## 已知限制与刻意的取舍

- **无独立于 Shift/Ctrl 的修饰键状态**（除父 compositor 报告的之外，嵌套模式）— 实践中够用。
- **嵌套窗口装饰取决于父 compositor**：KDE/Plasma、wlroots 系给 server-side（有原生标题栏）；
  GNOME/Mutter 不实现 `xdg-decoration`，窗口裸奔。我们**不画** client-side 装饰 ——
  「原生装饰」按定义就是宿主画的，自绘一套是另一回事。
- **popup 的 `constraint_adjustment` 需要 compositor 配合**：必须调
  `XdgShell::set_popup_constraint_query()` 告诉 shell 父窗口在哪，否则贴边菜单仍会溢出
  —— shell 层无从知道窗口摆在哪个位置。`tinyluminaria` 已接线。
- **DRM 路径在当前环境未在真机上验证**（无裸 VT）—— 含 atomic 提交、`IN_FENCE_FD` /
  `OUT_FENCE_PTR`、多输出分配、udev 热插拔、硬件光标平面与 VT 切换。渲染与几何这一侧有测试
  覆盖（`output-layout` / `output-scale` / `gpu-scanout` / `region` / `cursor-theme`），
  但"真实显示器上的翻页"只能在真机上确认。`session` 测试在没有 seat 的环境里 skip。
- **热插拔只覆盖 connector 状态**：GPU 本身热插拔（拔掉整块显卡 / DRM 设备消失）不处理。
- **headless 后端每帧有一次全屏 CPU 读回**（`read_scanout`，800×600 约 2.6ms release /
  13ms debug）。这是 wl_shm 呈现的代价。嵌套后端只在父 compositor 没有
  `zwp_linux_dmabuf_v1` 时才付这个代价，DRM 后端从来不付 —— 两者都直接扫描输出 dmabuf。
- **默认构建是 debug（`-O0`）**，逐像素转换会慢 5 倍。跑真实负载用 `xmake f -m release`。
- **模块化踩到两个 gcc 16 的坑**：接口单元里 defaulted 的 hidden-friend `operator==` 会
  ICE（改成成员形式），`std::function` / `std::make_shared` 要求每个实例化点都能看见
  `<typeinfo>`，不像头文件那样继承得到。
- **异步同步链路需要 GPU 支持 `VK_KHR_external_semaphore_fd`**。没有它时
  `render_to()` 退回 fence 阻塞，功能不变、延迟变差；同理驱动缺 `IN_FENCE_FD` /
  `OUT_FENCE_PTR` 属性时那两段自动跳过。
- **`Region` 不合并相邻矩形**（O(n) 矩形向量）。真实负载让矩形数量成为问题时换
  `pixman_region32`；已在源码里标了 `ponytail:`。
