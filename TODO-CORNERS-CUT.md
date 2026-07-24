# TODO & Corners Cut — Luminaria 源码审计

更新于 2026-07-25（P0 完工后重新梳理）。列出源码中所有 `TODO` 注释与"偷工减料"处
（为免真实客户端崩溃而设的空操作桩，以及有已知天花板的简化实现）。

> **本轮变化：** P0 完成后，原先的大块空操作（xdg_toplevel 全部状态请求、xdg_positioner
> 全部请求、`wl_pointer.set_cursor`、seat 无 destroy listener）都已落地实现，从本文档移除。
> 剩下的桩集中在 damage / region / transform / scale 这一类"不实现也不影响客户端跑"的项。

---

## 一、TODO 注释清单

### `src/types/compositor.cpp`

| TODO |
|------|
| `frame` 回调在 commit 时触发，而非实际展示时。暂时可用；若客户端空转则靠输出帧节奏限速。（对应 README P1 `presentation-time`） |
| 所有 `wl_surface` 请求均已接线（避免真实客户端命中空槽）；damage / region / transform / scale / offset 当前为空操作。 |
| 待 input/opaque region 真正起作用时，跟踪区域几何体。 |

### `src/types/xdg_shell.cpp`

| TODO |
|------|
| `constraint_adjustment`（flip/slide/resize）已解析并保存，但**未施加** —— 施加它需要父窗口在输出上的绝对位置，shell 层拿不到。见下 §2.4。 |
| `set_parent`（transient-for）与 `show_window_menu` 保持空操作 —— 库里没有窗口菜单概念，也没有 transient 堆叠。 |

### `include/luminaria/` 头文件

| 文件 | TODO |
|------|------|
| `backend.hpp` | `new_input` 在 Phase 3（输入）加入，目前暂不需要。 |
| `backend/drm.hpp` | dumb buffer + 无 libseat —— 无 VT 切换/恢复处理，无 atomic modesetting。 |
| `backend/libinput.hpp` | 无 libseat —— 依赖会话 ACL；键码为原始值。 |
| `backend/wayland.hpp` | wl_shm CPU 路径。 |
| `render/vulkan.hpp` | 基于拷贝的放置 —— 无缩放、无 alpha 混合；待需要半透明或缩放输出时升级为纹理四边形管线。 |
| `scene.hpp` | 尚无 damage tracking —— 待逐帧重绘开销大到需要时再加入。 |
| `util/rect_fill.hpp` | 目前仅纯色填充；纹理表面待渲染管线就绪后加入。 |
| `xwayland.hpp` | 最小 XWM —— 已处理 map/configure 请求；完整 ICCCM/EWMH 待补。 |

### `examples/tty_compositor.cpp`

| TODO |
|------|
| 窗口以固定偏移层叠（无移动/缩放/堆叠 UI）。 |

---

## 二、Corners Cut — 空操作桩

> 以下请求均接受但不执行任何操作。目的：真实客户端（GTK/Qt 等）启动时会调用这些协议请求，
> 若槽位为 null，`libwayland` 会 abort。桩的存在保证客户端不崩溃。

### 2.1 `wl_surface` 请求 (`src/types/compositor.cpp`)

```cpp
void surface_noop_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void surface_noop_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_noop_i32(wl_client*, wl_resource*, int32_t) {}
void surface_noop_offset(wl_client*, wl_resource*, int32_t, int32_t) {}
```

| 请求 | 状态 | 备注 |
|------|------|------|
| `damage` | 空操作 | damage tracking 未实现（README P1） |
| `damage_buffer` | 空操作 | 同上 |
| `set_opaque_region` | 空操作 | region 未跟踪 |
| `set_input_region` | 空操作 | 同上 —— 命中测试用 buffer 矩形，不用 input region |
| `set_buffer_transform` | 空操作 | transform 未实现 |
| `set_buffer_scale` | 空操作 | scale 未实现（README P1 HiDPI） |
| `offset` | 空操作 | — |

### 2.2 `wl_region` 请求 (`src/types/compositor.cpp`)

| 请求 | 状态 | 备注 |
|------|------|------|
| `add` | 空操作 | 待 input/opaque region 真正需要时实现 |
| `subtract` | 空操作 | 同上 |

### 2.3 `xdg_toplevel` 请求 (`src/types/xdg_shell.cpp`)

13 个请求中 **11 个已实现**，剩 2 个空操作：

| 请求 | 状态 | 备注 |
|------|------|------|
| `set_parent` | 空操作 | 无 transient 窗口层级 |
| `show_window_menu` | 空操作 | 无服务端窗口菜单 |

其余（`set_title` / `set_app_id` / `set_min_size` / `set_max_size` / `move` / `resize` /
`set_maximized` / `unset_maximized` / `set_fullscreen` / `unset_fullscreen` / `set_minimized`）
均已记录状态或发出信号交给 compositor 仲裁。

> **注意一处刻意设计：** `set_maximized` / `set_fullscreen` 在**没有任何监听者**时会自动
> 应答（直接置位并回 configure）。这是为了让不接管窗口状态的最小 compositor 也不会把客户端
> 挂住；接管者一旦 connect，控制权完全交给它。

### 2.4 `xdg_positioner` 的 `constraint_adjustment` (`src/types/xdg_shell.cpp`)

positioner 的全部请求都已实现并影响 popup 位置（anchor / gravity / offset / size /
`set_reactive` / `set_parent_size` / `set_parent_configure` 都被记录），**唯独
`constraint_adjustment` 存而不用**：

| 项 | 状态 | 天花板 |
|----|------|--------|
| anchor / gravity / offset / size 求解 | 已实现 | — |
| `flip_x` / `flip_y` / `slide_x` / `slide_y` / `resize_x` / `resize_y` | 解析但不施加 | 需要父窗口在输出上的**绝对**位置 + 输出可用区，shell 层没有这个信息 |

后果：屏幕内的菜单位置完全正确；贴近输出边缘的菜单可能溢出屏幕而不是翻转/滑动。
升级路径：给 `XdgShell` 加一个"父窗口绝对位置查询"回调（compositor 才知道窗口摆在哪），
或把 popup 定位整体上移到 scene 层。

### 2.5 `linux-drm-syncobj` 的 acquire 等待 (`src/types/drm_syncobj.cpp`)

| 项 | 状态 | 天花板 |
|----|------|--------|
| acquire point | commit 时 **CPU 阻塞** `drmSyncobjTimelineWait`，默认 50 ms 超时 | 合成本身就是 CPU 读回，没有可以把 fence 传下去的地方 |
| release point | buffer 被下次 commit 替换时 signal（等价于原本发 `wl_buffer.release` 的时刻） | 严格说应在"读完像素之后"；当前读像素发生在两次 commit 之间，故成立 |

超时只丢一帧，不会卡死事件循环。做到零拷贝 scanout 后，应改成把 fence 直接交给 KMS
（`DRM_MODE_ATOMIC` 的 `IN_FENCE_FD` / `OUT_FENCE_PTR`）而不是阻塞等待。

### 2.6 缓冲区一律 CPU 读回

`Surface::current_buffer_rgba` 把每个客户端 buffer（shm 或 dmabuf）读成 RGBA 再合成。
没有零拷贝 scanout 路径。这是全库最大的性能天花板，也是 README「非协议部分」里
「DRM atomic + GBM scanout」那一条的实际含义。

### 2.7 截图/录屏 (`src/types/screencopy.cpp`)

一处 `// no-op: unsupported`（不支持的捕获格式组合）。

---

## 三、汇总

| 类别 | 数量 |
|------|------|
| TODO 注释 (`.cpp`) | 5 |
| TODO 注释 (`.hpp`) | 8 |
| TODO 注释 (`examples/`) | 1 |
| 空操作桩函数 | ~12 |
| **总计标记点** | **~26**（上一轮 ~41） |

### 按影响面分类

| 影响 | 桩/缺失 |
|------|---------|
| 不影响客户端运行 | `damage`, `region`, `transform`, `scale`, `offset`, `set_parent`, `show_window_menu` |
| 客户端可感知但有降级 | popup `constraint_adjustment`（贴边菜单溢出） |
| 性能天花板 | buffer 一律 CPU 读回；无 damage tracking；syncobj acquire 为 CPU 阻塞 |
| 裸机限制 | 无 libseat (VT 切换)、DRM legacy 模式、键码原始值 |

---

## 四、与 README 路线图对照

README 中以 `✓=已有，✗=缺，△=部分` 标记。**P0 已全部为 ✓**；本文档剩余的桩对应以下项：

| README 项 | 对应桩 |
|-----------|--------|
| ✗ damage tracking (P1) | §2.1 `damage` / `damage_buffer` 空操作 |
| ✗ `presentation-time` (P1) | `compositor.cpp`：`frame` 在 commit 触发而非 vblank |
| ✗ HiDPI / fractional-scale (P1) | §2.1 `set_buffer_scale` / `set_buffer_transform` 空操作 |
| ✗ `xdg-decoration` (P1) | §2.3 `show_window_menu` 空操作；无服务端装饰 |
| △ 零拷贝 scanout（非协议部分） | §2.6 全量 CPU 读回；§2.5 syncobj 只能 CPU 等 |
| △ DRM atomic（非协议部分） | `drm.hpp` TODO：legacy only |
| △ XWM（非协议部分） | `xwayland.hpp` TODO：minimal |
| P0 ✓ popup（有降级） | §2.4 `constraint_adjustment` 存而不用 |
